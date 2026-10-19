/* Dispositivos da maquina ARM "virt": UART PL011, GICv2 e RTC PL031. */
#include "devices.h"

#include <stdlib.h>
#include <time.h>

static const uint8_t primecell_id[4] = {0x0d, 0xf0, 0x05, 0xb1};

/* ================================================================ PL011 */

#define PL011_FIFO 32
#define PL011_INT_RX 0x10
#define PL011_INT_TX 0x20
#define PL011_INT_RT 0x40

struct pl011 {
    mvm_vm *vm;
    mvm_chardev *chr;
    irq_line irq;
    uint8_t fifo[PL011_FIFO];
    unsigned rpos, count;
    uint32_t lcr, cr, ifls, imsc, ris, ibrd, fbrd, dmacr, ilpr;
};

static void pl011_update(pl011 *u) { irq_set(&u->irq, (u->ris & u->imsc) ? 1 : 0); }

pl011 *pl011_new(mvm_vm *vm, mvm_chardev *chr, irq_line irq)
{
    pl011 *u = calloc(1, sizeof(*u));
    u->vm = vm;
    u->chr = chr;
    u->irq = irq;
    u->cr = 0x300;
    u->ifls = 0x12;
    return u;
}

void pl011_poll(pl011 *u)
{
    bool got = false;
    while (u->count < PL011_FIFO) {
        int ch = chr_read_byte(u->chr);
        if (ch < 0)
            break;
        u->fifo[(u->rpos + u->count) % PL011_FIFO] = (uint8_t)ch;
        u->count++;
        got = true;
    }
    if (got) {
        u->ris |= PL011_INT_RX | PL011_INT_RT;
        pl011_update(u);
    }
}

static uint64_t pl011_read(void *opaque, uint64_t off, unsigned size)
{
    pl011 *u = opaque;
    (void)size;
    if (off >= 0xfe0 && off < 0x1000) {
        static const uint8_t pid[4] = {0x11, 0x10, 0x14, 0x00};
        unsigned i = (unsigned)(off - 0xfe0) >> 2;
        return i < 4 ? pid[i] : primecell_id[i - 4];
    }
    switch (off) {
    case 0x000: {
        uint32_t c = 0;
        if (u->count) {
            c = u->fifo[u->rpos];
            u->rpos = (u->rpos + 1) % PL011_FIFO;
            u->count--;
        }
        if (!u->count)
            u->ris &= ~(uint32_t)(PL011_INT_RX | PL011_INT_RT);
        pl011_update(u);
        return c;
    }
    case 0x004: return 0;
    case 0x018: { /* FR */
        uint32_t fr = 0x80; /* TXFE */
        if (!u->count) fr |= 0x10;             /* RXFE */
        if (u->count == PL011_FIFO) fr |= 0x40; /* RXFF */
        return fr;
    }
    case 0x020: return u->ilpr;
    case 0x024: return u->ibrd;
    case 0x028: return u->fbrd;
    case 0x02c: return u->lcr;
    case 0x030: return u->cr;
    case 0x034: return u->ifls;
    case 0x038: return u->imsc;
    case 0x03c: return u->ris;
    case 0x040: return u->ris & u->imsc;
    case 0x048: return u->dmacr;
    default: return 0;
    }
}

static void pl011_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    pl011 *u = opaque;
    uint32_t v = (uint32_t)val;
    (void)size;
    switch (off) {
    case 0x000: {
        uint8_t ch = (uint8_t)v;
        chr_write(u->chr, &ch, 1);
        u->ris |= PL011_INT_TX;
        pl011_update(u);
        break;
    }
    case 0x004: break;
    case 0x020: u->ilpr = v; break;
    case 0x024: u->ibrd = v; break;
    case 0x028: u->fbrd = v; break;
    case 0x02c: u->lcr = v; break;
    case 0x030: u->cr = v; break;
    case 0x034: u->ifls = v; break;
    case 0x038:
        u->imsc = v;
        pl011_update(u);
        break;
    case 0x044:
        u->ris &= ~v;
        if (u->count)
            u->ris |= PL011_INT_RX | PL011_INT_RT;
        pl011_update(u);
        break;
    case 0x048: u->dmacr = v; break;
    default: break;
    }
}

const mvm_io_ops pl011_ops = {pl011_read, pl011_write};

/* ================================================================ GICv2 */

#define GIC_NIRQ 128
#define GIC_MAX_CPUS 8

/* Interface de CPU e registradores "banked" (SGIs 0-15 e PPIs 16-31 sao de cada CPU) */
typedef struct {
    bool en;
    uint32_t pmr, bpr, abpr;
    int out;
    uint8_t enabled[32], pending[32], level[32], active[32], prio[32];
    uint8_t sgi_src[16];   /* SGIs pendentes: mascara das CPUs de origem */
    int act_stack[GIC_NIRQ]; /* interrupcoes ativas (para a prioridade em execucao) */
    int act_n;
} gic_cpu;

struct gicv2 {
    mvm_vm *vm;
    void (*cpu_irq)(void *cpu, int line, int level);
    void *cpus[GIC_MAX_CPUS];
    int ncpus;
    bool dist_en;
    /* SPIs (32 em diante); os indices < 32 nao sao usados aqui */
    uint8_t enabled[GIC_NIRQ];
    uint8_t pending[GIC_NIRQ]; /* pendente por software/borda */
    uint8_t level[GIC_NIRQ];   /* nivel da linha de entrada */
    uint8_t active[GIC_NIRQ];
    uint8_t edge[GIC_NIRQ];    /* configuracao (tambem das PPIs) */
    uint8_t prio[GIC_NIRQ];
    uint8_t target[GIC_NIRQ];
    uint8_t group[GIC_NIRQ];
    gic_cpu c[GIC_MAX_CPUS];
};

/* CPU que esta acessando o GIC (a thread da CPU); outras threads: CPU 0 */
static int gic_me(gicv2 *g)
{
    int i = vm_cpu_index(g->vm);
    return i >= 0 && i < g->ncpus ? i : 0;
}

static inline bool g_enabled(gicv2 *g, int k, int i) { return i < 32 ? g->c[k].enabled[i] : g->enabled[i]; }
static inline bool g_active(gicv2 *g, int k, int i) { return i < 32 ? g->c[k].active[i] : g->active[i]; }
static inline int g_prio(gicv2 *g, int k, int i) { return i < 32 ? g->c[k].prio[i] : g->prio[i]; }

static inline bool gic_is_pending(gicv2 *g, int k, int i)
{
    if (i < 16)
        return g->c[k].sgi_src[i] != 0;
    if (i < 32)
        return g->c[k].pending[i] || (!g->edge[i] && g->c[k].level[i]);
    return g->pending[i] || (!g->edge[i] && g->level[i]);
}

static int gic_running_prio(gicv2 *g, int k)
{
    int best = 0x100;
    for (int n = 0; n < g->c[k].act_n; n++) {
        int p = g_prio(g, k, g->c[k].act_stack[n]);
        if (p < best)
            best = p;
    }
    return best;
}

/* interrupcao pendente de maior prioridade para a CPU k (1023 = nenhuma) */
static int gic_best(gicv2 *g, int k)
{
    int best = 1023, bp = 0x100;
    for (int i = 0; i < GIC_NIRQ; i++) {
        if (i >= 32 && !((g->target[i] >> k) & 1))
            continue;
        if (!g_enabled(g, k, i) || g_active(g, k, i) || !gic_is_pending(g, k, i))
            continue;
        int p = g_prio(g, k, i);
        if (p < bp) {
            bp = p;
            best = i;
        }
    }
    return best;
}

static void gic_update_cpu(gicv2 *g, int k)
{
    gic_cpu *cc = &g->c[k];
    int lvl = 0;
    if (g->dist_en && cc->en) {
        int b = gic_best(g, k);
        if (b != 1023 && g_prio(g, k, b) < (int)cc->pmr && g_prio(g, k, b) < gic_running_prio(g, k))
            lvl = 1;
    }
    if (lvl != cc->out) {
        cc->out = lvl;
        g->cpu_irq(g->cpus[k], 0, lvl);
    }
}

static void gic_update(gicv2 *g)
{
    for (int k = 0; k < g->ncpus; k++)
        gic_update_cpu(g, k);
}

void gicv2_reset(gicv2 *g)
{
    g->dist_en = false;
    memset(g->enabled, 0, sizeof(g->enabled));
    memset(g->pending, 0, sizeof(g->pending));
    memset(g->active, 0, sizeof(g->active));
    memset(g->prio, 0, sizeof(g->prio));
    memset(g->target, 0, sizeof(g->target));
    memset(g->group, 0, sizeof(g->group));
    for (int i = 0; i < GIC_NIRQ; i++)
        g->edge[i] = i < 16; /* SGIs sao de borda */
    for (int k = 0; k < g->ncpus; k++) {
        gic_cpu *cc = &g->c[k];
        uint8_t lv[32];
        memcpy(lv, cc->level, sizeof(lv)); /* nivel das linhas (timers) continua */
        int out = cc->out;
        memset(cc, 0, sizeof(*cc));
        memcpy(cc->level, lv, sizeof(lv));
        cc->out = out;
        for (int i = 0; i < 16; i++)
            cc->enabled[i] = 1;
        cc->bpr = 2;
        cc->abpr = 3;
    }
    gic_update(g);
}

gicv2 *gicv2_new(mvm_vm *vm, void (*cpu_irq)(void *cpu, int line, int level), void *const *cpus, int ncpus)
{
    gicv2 *g = calloc(1, sizeof(*g));
    g->vm = vm;
    g->cpu_irq = cpu_irq;
    g->ncpus = ncpus < 1 ? 1 : ncpus > GIC_MAX_CPUS ? GIC_MAX_CPUS : ncpus;
    for (int k = 0; k < g->ncpus; k++)
        g->cpus[k] = cpus[k];
    gicv2_reset(g);
    return g;
}

/* linha de interrupcao compartilhada (SPI, INTID >= 32) */
void gicv2_set_irq(void *opaque, int irq, int level)
{
    gicv2 *g = opaque;
    if (irq < 32) {
        gicv2_set_ppi(g, 0, irq, level);
        return;
    }
    if (irq >= GIC_NIRQ)
        return;
    if (g->edge[irq] && level && !g->level[irq])
        g->pending[irq] = 1;
    g->level[irq] = (uint8_t)level;
    gic_update(g);
}

/* interrupcao privada (PPI 16-31) da CPU cpu: timers */
void gicv2_set_ppi(gicv2 *g, int cpu, int irq, int level)
{
    if (cpu < 0 || cpu >= g->ncpus || irq < 16 || irq >= 32)
        return;
    gic_cpu *cc = &g->c[cpu];
    if (g->edge[irq] && level && !cc->level[irq])
        cc->pending[irq] = 1;
    cc->level[irq] = (uint8_t)level;
    gic_update_cpu(g, cpu);
}

static uint32_t bits_of(gicv2 *g, int me, uint64_t off, int what)
{
    int base = (int)(off & 0x7f) * 8;
    uint32_t v = 0;
    for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) {
        int irq = base + i;
        bool b;
        switch (what) {
        case 0: b = g_enabled(g, me, irq); break;
        case 1: b = gic_is_pending(g, me, irq); break;
        default: b = g_active(g, me, irq); break;
        }
        v |= (uint32_t)b << i;
    }
    return v;
}

static uint64_t gicd_read(void *opaque, uint64_t off, unsigned size)
{
    gicv2 *g = opaque;
    int me = gic_me(g);
    if (off == 0x000) return g->dist_en;
    if (off == 0x004) return (uint32_t)((GIC_NIRQ / 32) - 1) | ((uint32_t)(g->ncpus - 1) << 5); /* sem seguranca */
    if (off == 0x008) return 0x0100143b;
    if (off >= 0x080 && off < 0x100) { /* IGROUPR */
        uint32_t v = 0;
        int base = (int)(off - 0x080) * 8;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) v |= (uint32_t)g->group[base + i] << i;
        return v;
    }
    if (off >= 0x100 && off < 0x200) return bits_of(g, me, off, 0);
    if (off >= 0x200 && off < 0x300) return bits_of(g, me, off, 1);
    if (off >= 0x300 && off < 0x400) return bits_of(g, me, off, 2);
    if (off >= 0x400 && off < 0x400 + GIC_NIRQ) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size && off - 0x400 + i < GIC_NIRQ; i++)
            v |= (uint64_t)g_prio(g, me, (int)(off - 0x400 + i)) << (8 * i);
        return v;
    }
    if (off >= 0x800 && off < 0x800 + GIC_NIRQ) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++) {
            unsigned irq = (unsigned)(off - 0x800) + i;
            uint8_t t = irq < 32 ? (uint8_t)(1u << me) : irq < GIC_NIRQ ? g->target[irq] : 0;
            v |= (uint64_t)t << (8 * i);
        }
        return v;
    }
    if (off >= 0xc00 && off < 0xd00) {
        int base = (int)(off - 0xc00) * 4;
        uint32_t v = 0;
        for (int i = 0; i < 16 && base + i < GIC_NIRQ; i++)
            if (g->edge[base + i]) v |= 2u << (2 * i);
        return v;
    }
    if (off >= 0xf10 && off < 0xf30) { /* CPENDSGIR/SPENDSGIR: origens de cada SGI */
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++) {
            unsigned id = (unsigned)(off & 0xf) + i;
            if (id < 16)
                v |= (uint64_t)g->c[me].sgi_src[id] << (8 * i);
        }
        return v;
    }
    if (off == 0xfe8) return 0x2b; /* ICPIDR2: GICv2 */
    return 0;
}

static void set_bits(gicv2 *g, int me, uint64_t off, uint32_t v, int what, bool set)
{
    int base = (int)(off & 0x7f) * 8;
    for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) {
        if (!(v & (1u << i)))
            continue;
        int irq = base + i;
        if (what == 0) {
            if (irq < 16)
                continue; /* SGIs sempre habilitadas */
            if (irq < 32) g->c[me].enabled[irq] = set;
            else g->enabled[irq] = set;
        } else if (what == 1) {
            if (irq < 16) {
                if (!set) g->c[me].sgi_src[irq] = 0;
            } else if (irq < 32) {
                g->c[me].pending[irq] = set;
            } else {
                g->pending[irq] = set;
            }
        } else {
            if (irq < 32) g->c[me].active[irq] = set;
            else g->active[irq] = set;
        }
    }
}

static void gicd_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    gicv2 *g = opaque;
    int me = gic_me(g);
    uint32_t v = (uint32_t)val;
    if (off == 0x000) {
        g->dist_en = v & 1;
    } else if (off >= 0x080 && off < 0x100) {
        int base = (int)(off - 0x080) * 8;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) g->group[base + i] = (v >> i) & 1;
    } else if (off >= 0x100 && off < 0x200) {
        set_bits(g, me, off, v, 0, off < 0x180);
    } else if (off >= 0x200 && off < 0x300) {
        set_bits(g, me, off, v, 1, off < 0x280);
    } else if (off >= 0x300 && off < 0x400) {
        set_bits(g, me, off, v, 2, off < 0x380);
    } else if (off >= 0x400 && off < 0x400 + GIC_NIRQ) {
        for (unsigned i = 0; i < size && off - 0x400 + i < GIC_NIRQ; i++) {
            unsigned irq = (unsigned)(off - 0x400) + i;
            uint8_t p = (uint8_t)(val >> (8 * i));
            if (irq < 32) g->c[me].prio[irq] = p;
            else g->prio[irq] = p;
        }
    } else if (off >= 0x800 && off < 0x800 + GIC_NIRQ) {
        for (unsigned i = 0; i < size && off - 0x800 + i < GIC_NIRQ; i++)
            if (off - 0x800 + i >= 32)
                g->target[off - 0x800 + i] = (uint8_t)(val >> (8 * i)) & (uint8_t)((1u << g->ncpus) - 1);
    } else if (off >= 0xc00 && off < 0xd00) {
        int base = (int)(off - 0xc00) * 4;
        for (int i = 0; i < 16 && base + i < GIC_NIRQ; i++)
            if (base + i >= 16)
                g->edge[base + i] = (v >> (2 * i + 1)) & 1;
    } else if (off == 0xf00) { /* SGIR: interrupcao entre CPUs */
        unsigned filter = (v >> 24) & 3, id = v & 15, list = (v >> 16) & 0xff;
        unsigned mask = filter == 0 ? list : filter == 1 ? (((1u << g->ncpus) - 1) & ~(1u << me)) : filter == 2 ? 1u << me : 0;
        for (int k = 0; k < g->ncpus; k++)
            if ((mask >> k) & 1)
                g->c[k].sgi_src[id] |= (uint8_t)(1u << me);
    } else if (off >= 0xf10 && off < 0xf30) {
        bool set = off >= 0xf20;
        for (unsigned i = 0; i < size; i++) {
            unsigned id = (unsigned)(off & 0xf) + i;
            uint8_t b = (uint8_t)(val >> (8 * i));
            if (id >= 16 || !b)
                continue;
            if (set) g->c[me].sgi_src[id] |= b;
            else g->c[me].sgi_src[id] &= (uint8_t)~b;
        }
    }
    gic_update(g);
}

static uint64_t gicc_read(void *opaque, uint64_t off, unsigned size)
{
    gicv2 *g = opaque;
    int me = gic_me(g);
    gic_cpu *cc = &g->c[me];
    (void)size;
    switch (off) {
    case 0x00: return cc->en;
    case 0x04: return cc->pmr;
    case 0x08: return cc->bpr;
    case 0x0c: case 0x20: { /* IAR */
        int b = gic_best(g, me);
        if (b == 1023 || !g->dist_en || g_prio(g, me, b) >= (int)cc->pmr || g_prio(g, me, b) >= gic_running_prio(g, me))
            return 1023;
        uint32_t r = (uint32_t)b;
        if (b < 16) { /* SGI: devolve a CPU de origem (bits 10-12) e consome so ela */
            int src = __builtin_ctz(cc->sgi_src[b]);
            cc->sgi_src[b] &= (uint8_t)~(1u << src);
            cc->active[b] = 1;
            r |= (uint32_t)src << 10;
        } else if (b < 32) {
            cc->pending[b] = 0;
            cc->active[b] = 1;
        } else {
            g->pending[b] = 0;
            g->active[b] = 1;
        }
        if (cc->act_n < GIC_NIRQ)
            cc->act_stack[cc->act_n++] = b;
        gic_update(g);
        return r;
    }
    case 0x14: return (uint32_t)(gic_running_prio(g, me) & 0xff);
    case 0x18: case 0x28: return (uint32_t)gic_best(g, me);
    case 0x1c: return cc->abpr;
    case 0xfc: return 0x0202143b;
    default: return 0;
    }
}

static void gic_deactivate(gicv2 *g, int me, int id)
{
    if (id < 0 || id >= GIC_NIRQ)
        return;
    gic_cpu *cc = &g->c[me];
    if (id < 32)
        cc->active[id] = 0;
    else
        g->active[id] = 0;
    for (int k = 0; k < cc->act_n; k++) {
        if (cc->act_stack[k] == id) {
            memmove(&cc->act_stack[k], &cc->act_stack[k + 1], (size_t)(cc->act_n - k - 1) * sizeof(int));
            cc->act_n--;
            break;
        }
    }
}

static void gicc_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    gicv2 *g = opaque;
    int me = gic_me(g);
    gic_cpu *cc = &g->c[me];
    uint32_t v = (uint32_t)val;
    (void)size;
    switch (off) {
    case 0x00: cc->en = v & 1; break;
    case 0x04: cc->pmr = v & 0xff; break;
    case 0x08: cc->bpr = v & 7; break;
    case 0x10: case 0x24: gic_deactivate(g, me, (int)(v & 0x3ff)); break;
    case 0x1c: cc->abpr = v & 7; break;
    case 0x1000: gic_deactivate(g, me, (int)(v & 0x3ff)); break;
    default: break;
    }
    gic_update(g);
}

const mvm_io_ops gicv2_dist_ops = {gicd_read, gicd_write};
const mvm_io_ops gicv2_cpu_ops = {gicc_read, gicc_write};

/* ================================================================ PL031 */

struct pl031 {
    mvm_vm *vm;
    irq_line irq;
    int64_t offset; /* segundos somados ao relogio do host */
    uint32_t mr, cr, imsc, ris;
};

pl031 *pl031_new(mvm_vm *vm, irq_line irq)
{
    pl031 *r = calloc(1, sizeof(*r));
    r->vm = vm;
    r->irq = irq;
    r->cr = 1;
    if (vm->cfg.rtc_base)
        r->offset = vm->cfg.rtc_base - (int64_t)time(NULL);
    return r;
}

static uint32_t pl031_now(pl031 *r) { return (uint32_t)(time(NULL) + r->offset); }

static uint64_t pl031_read(void *opaque, uint64_t off, unsigned size)
{
    pl031 *r = opaque;
    (void)size;
    if (off >= 0xfe0 && off < 0x1000) {
        static const uint8_t pid[4] = {0x31, 0x10, 0x14, 0x00};
        unsigned i = (unsigned)(off - 0xfe0) >> 2;
        return i < 4 ? pid[i] : primecell_id[i - 4];
    }
    switch (off) {
    case 0x00: return pl031_now(r);
    case 0x04: return r->mr;
    case 0x0c: return r->cr;
    case 0x10: return r->imsc;
    case 0x14: return r->ris;
    case 0x18: return r->ris & r->imsc;
    default: return 0;
    }
}

static void pl031_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    pl031 *r = opaque;
    uint32_t v = (uint32_t)val;
    (void)size;
    switch (off) {
    case 0x04: r->mr = v; break;
    case 0x08: r->offset = (int64_t)v - (int64_t)time(NULL); break;
    case 0x0c: r->cr = v & 1; break;
    case 0x10: r->imsc = v & 1; break;
    case 0x1c: r->ris &= ~v; break;
    default: break;
    }
    irq_set(&r->irq, (r->ris & r->imsc) ? 1 : 0);
}

const mvm_io_ops pl031_ops = {pl031_read, pl031_write};
