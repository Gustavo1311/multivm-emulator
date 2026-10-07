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

struct gicv2 {
    mvm_vm *vm;
    void (*cpu_irq)(void *cpu, int line, int level);
    void *cpu;
    bool dist_en, cpu_en;
    uint8_t enabled[GIC_NIRQ];
    uint8_t pending[GIC_NIRQ]; /* pendente por software/borda */
    uint8_t level[GIC_NIRQ];   /* nivel da linha de entrada */
    uint8_t active[GIC_NIRQ];
    uint8_t edge[GIC_NIRQ];
    uint8_t prio[GIC_NIRQ];
    uint8_t target[GIC_NIRQ];
    uint8_t group[GIC_NIRQ];
    uint32_t pmr, bpr, abpr;
    int out;
    /* pilha de prioridades ativas */
    int act_stack[GIC_NIRQ];
    int act_n;
};

static inline bool gic_is_pending(gicv2 *g, int i)
{
    return g->pending[i] || (!g->edge[i] && g->level[i]);
}

static int gic_running_prio(gicv2 *g)
{
    int best = 0x100;
    for (int k = 0; k < g->act_n; k++) {
        int p = g->prio[g->act_stack[k]];
        if (p < best)
            best = p;
    }
    return best;
}

static int gic_best(gicv2 *g)
{
    int best = 1023, bp = 0x100;
    for (int i = 0; i < GIC_NIRQ; i++) {
        if (!g->enabled[i] || g->active[i] || !gic_is_pending(g, i))
            continue;
        if (g->prio[i] < bp) {
            bp = g->prio[i];
            best = i;
        }
    }
    return best;
}

static void gic_update(gicv2 *g)
{
    int lvl = 0;
    if (g->dist_en && g->cpu_en) {
        int b = gic_best(g);
        if (b != 1023 && g->prio[b] < g->pmr && g->prio[b] < gic_running_prio(g))
            lvl = 1;
    }
    if (lvl != g->out) {
        g->out = lvl;
        g->cpu_irq(g->cpu, 0, lvl);
    }
}

void gicv2_reset(gicv2 *g)
{
    g->dist_en = g->cpu_en = false;
    memset(g->enabled, 0, sizeof(g->enabled));
    memset(g->pending, 0, sizeof(g->pending));
    memset(g->active, 0, sizeof(g->active));
    memset(g->prio, 0, sizeof(g->prio));
    memset(g->target, 0, sizeof(g->target));
    memset(g->group, 0, sizeof(g->group));
    for (int i = 0; i < GIC_NIRQ; i++)
        g->edge[i] = i < 16; /* SGIs sao de borda */
    for (int i = 0; i < 16; i++)
        g->enabled[i] = 1;
    g->pmr = 0;
    g->bpr = 2;
    g->abpr = 3;
    g->act_n = 0;
    gic_update(g);
}

gicv2 *gicv2_new(mvm_vm *vm, void (*cpu_irq)(void *cpu, int line, int level), void *cpu)
{
    gicv2 *g = calloc(1, sizeof(*g));
    g->vm = vm;
    g->cpu_irq = cpu_irq;
    g->cpu = cpu;
    gicv2_reset(g);
    return g;
}

void gicv2_set_irq(void *opaque, int irq, int level)
{
    gicv2 *g = opaque;
    if (irq < 0 || irq >= GIC_NIRQ)
        return;
    if (g->edge[irq] && level && !g->level[irq])
        g->pending[irq] = 1;
    g->level[irq] = (uint8_t)level;
    gic_update(g);
}

static uint64_t gicd_read(void *opaque, uint64_t off, unsigned size)
{
    gicv2 *g = opaque;
    if (off == 0x000) return g->dist_en;
    if (off == 0x004) return (GIC_NIRQ / 32) - 1; /* 1 CPU, sem seguranca */
    if (off == 0x008) return 0x0100143b;
    if (off >= 0x080 && off < 0x100) { /* IGROUPR */
        uint32_t v = 0;
        int base = (int)(off - 0x080) * 8;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) v |= (uint32_t)g->group[base + i] << i;
        return v;
    }
    if (off >= 0x100 && off < 0x200) { /* IS/ICENABLER */
        int base = (int)((off & 0x7f)) * 8;
        uint32_t v = 0;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) v |= (uint32_t)g->enabled[base + i] << i;
        return v;
    }
    if (off >= 0x200 && off < 0x300) {
        int base = (int)((off & 0x7f)) * 8;
        uint32_t v = 0;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) v |= (uint32_t)gic_is_pending(g, base + i) << i;
        return v;
    }
    if (off >= 0x300 && off < 0x400) {
        int base = (int)((off & 0x7f)) * 8;
        uint32_t v = 0;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) v |= (uint32_t)g->active[base + i] << i;
        return v;
    }
    if (off >= 0x400 && off < 0x400 + GIC_NIRQ) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size && off - 0x400 + i < GIC_NIRQ; i++)
            v |= (uint64_t)g->prio[off - 0x400 + i] << (8 * i);
        return v;
    }
    if (off >= 0x800 && off < 0x800 + GIC_NIRQ) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++) {
            unsigned irq = (unsigned)(off - 0x800) + i;
            uint8_t t = irq < 32 ? 1 : g->target[irq];
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
    if (off == 0xfe8) return 0x2b; /* ICPIDR2: GICv2 */
    return 0;
}

static void gicd_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    gicv2 *g = opaque;
    uint32_t v = (uint32_t)val;
    if (off == 0x000) {
        g->dist_en = v & 1;
    } else if (off >= 0x080 && off < 0x100) {
        int base = (int)(off - 0x080) * 8;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++) g->group[base + i] = (v >> i) & 1;
    } else if (off >= 0x100 && off < 0x200) {
        bool set = off < 0x180;
        int base = (int)((off & 0x7f)) * 8;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++)
            if (v & (1u << i)) g->enabled[base + i] = set;
    } else if (off >= 0x200 && off < 0x300) {
        bool set = off < 0x280;
        int base = (int)((off & 0x7f)) * 8;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++)
            if (v & (1u << i)) g->pending[base + i] = set;
    } else if (off >= 0x300 && off < 0x400) {
        bool set = off < 0x380;
        int base = (int)((off & 0x7f)) * 8;
        for (int i = 0; i < 32 && base + i < GIC_NIRQ; i++)
            if (v & (1u << i)) g->active[base + i] = set;
    } else if (off >= 0x400 && off < 0x400 + GIC_NIRQ) {
        for (unsigned i = 0; i < size && off - 0x400 + i < GIC_NIRQ; i++)
            g->prio[off - 0x400 + i] = (uint8_t)(val >> (8 * i));
    } else if (off >= 0x800 && off < 0x800 + GIC_NIRQ) {
        for (unsigned i = 0; i < size && off - 0x800 + i < GIC_NIRQ; i++)
            g->target[off - 0x800 + i] = (uint8_t)(val >> (8 * i));
    } else if (off >= 0xc00 && off < 0xd00) {
        int base = (int)(off - 0xc00) * 4;
        for (int i = 0; i < 16 && base + i < GIC_NIRQ; i++)
            if (base + i >= 16)
                g->edge[base + i] = (v >> (2 * i + 1)) & 1;
    } else if (off == 0xf00) { /* SGIR */
        unsigned filter = (v >> 24) & 3, id = v & 15;
        if (filter != 1) /* 1 = todos exceto este: nao ha outros */
            g->pending[id] = 1;
    } else if (off >= 0xf10 && off < 0xf30) {
        bool set = off >= 0xf20;
        int base = (int)(off & 0xf) * 1;
        for (unsigned i = 0; i < size; i++)
            if ((val >> (8 * i)) & 0xff) g->pending[base + (int)i] = set;
    }
    gic_update(g);
}

static uint64_t gicc_read(void *opaque, uint64_t off, unsigned size)
{
    gicv2 *g = opaque;
    (void)size;
    switch (off) {
    case 0x00: return g->cpu_en;
    case 0x04: return g->pmr;
    case 0x08: return g->bpr;
    case 0x0c: case 0x20: { /* IAR */
        int b = gic_best(g);
        if (b == 1023 || !g->dist_en || g->prio[b] >= g->pmr || g->prio[b] >= gic_running_prio(g))
            return 1023;
        g->pending[b] = 0;
        g->active[b] = 1;
        if (g->act_n < GIC_NIRQ)
            g->act_stack[g->act_n++] = b;
        gic_update(g);
        return (uint32_t)b;
    }
    case 0x14: return (uint32_t)(gic_running_prio(g) & 0xff);
    case 0x18: case 0x28: return (uint32_t)gic_best(g);
    case 0x1c: return g->abpr;
    case 0xfc: return 0x0202143b;
    default: return 0;
    }
}

static void gic_deactivate(gicv2 *g, int id)
{
    if (id < 0 || id >= GIC_NIRQ)
        return;
    g->active[id] = 0;
    for (int k = 0; k < g->act_n; k++) {
        if (g->act_stack[k] == id) {
            memmove(&g->act_stack[k], &g->act_stack[k + 1], (size_t)(g->act_n - k - 1) * sizeof(int));
            g->act_n--;
            break;
        }
    }
}

static void gicc_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    gicv2 *g = opaque;
    uint32_t v = (uint32_t)val;
    (void)size;
    switch (off) {
    case 0x00: g->cpu_en = v & 1; break;
    case 0x04: g->pmr = v & 0xff; break;
    case 0x08: g->bpr = v & 7; break;
    case 0x10: case 0x24: gic_deactivate(g, (int)(v & 0x3ff)); break;
    case 0x1c: g->abpr = v & 7; break;
    case 0x1000: gic_deactivate(g, (int)(v & 0x3ff)); break;
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
