/*
 * HPET (High Precision Event Timer) em 0xFED00000: contador de 64 bits a
 * 100 MHz e 3 comparadores (o 0 com modo periodico). No modo "legacy
 * replacement" o timer 0 substitui o PIT (IRQ0) e o 1 substitui o RTC (IRQ8).
 * Segue a especificacao IA-PC HPET 1.0a e o comportamento do HPET do QEMU.
 */
#include "hpet.h"

#include <stdlib.h>

#define HPET_NS_PER_TICK 10 /* 100 MHz */
#define HPET_FS_PER_TICK (HPET_NS_PER_TICK * 1000000ULL)

#define CFG_ENABLE (1u << 0)
#define CFG_LEGACY (1u << 1)

#define TN_LEVEL (1u << 1)
#define TN_ENABLE (1u << 2)
#define TN_PERIODIC (1u << 3)
#define TN_PER_CAP (1u << 4)
#define TN_SIZE_CAP (1u << 5)
#define TN_SETVAL (1u << 6)
#define TN_32BIT (1u << 8)
#define TN_ROUTE_SHIFT 9
#define TN_FSB (1u << 14)
#define TN_CFG_WRITABLE (TN_LEVEL | TN_ENABLE | TN_PERIODIC | TN_SETVAL | TN_32BIT | (0x1fu << TN_ROUTE_SHIFT))
#define ROUTE_CAP 0x00f00000u /* pinos 20-23 do IOAPIC */

typedef struct {
    struct hpet *h;
    int n;
    uint64_t config, cmp, period;
    mvm_timer t;
    int level; /* linha atual */
} hpet_timer;

struct hpet {
    mvm_vm *vm;
    void (*set_irq)(void *opaque, int line, int level);
    void *opaque;
    uint64_t config, isr;
    uint64_t counter;   /* valor com o HPET parado */
    int64_t base_ns;    /* com o HPET ligado: contador = (agora - base_ns) / 10 */
    hpet_timer tm[HPET_TIMERS];
};

static bool running(const hpet *h) { return h->config & CFG_ENABLE; }

static uint64_t counter_now(const hpet *h)
{
    if (!running(h))
        return h->counter;
    return (uint64_t)((mvm_now(h->vm) - h->base_ns) / HPET_NS_PER_TICK);
}

/* linha de interrupcao do timer: IRQ legada (0 ou 8) ou pino do IOAPIC */
static int timer_line(const hpet_timer *t)
{
    if ((t->h->config & CFG_LEGACY) && t->n < 2)
        return t->n == 0 ? HPET_LINE_IRQ0 : HPET_LINE_IRQ8;
    return (int)((t->config >> TN_ROUTE_SHIFT) & 0x1f);
}

static void set_line(hpet_timer *t, int level)
{
    if (t->level == level && !(level && !(t->config & TN_LEVEL)))
        return;
    t->level = level;
    t->h->set_irq(t->h->opaque, timer_line(t), level);
}

static void timer_fire_irq(hpet_timer *t)
{
    if (!(t->config & TN_ENABLE))
        return;
    if (t->config & TN_LEVEL) {
        t->h->isr |= 1ULL << t->n;
        set_line(t, 1);
    } else { /* borda */
        t->h->set_irq(t->h->opaque, timer_line(t), 1);
        t->h->set_irq(t->h->opaque, timer_line(t), 0);
        t->level = 0;
    }
}

static void timer_arm(hpet_timer *t)
{
    hpet *h = t->h;
    timer_del(h->vm, &t->t);
    if (!running(h))
        return;
    uint64_t now = counter_now(h);
    uint64_t diff;
    if (t->config & TN_32BIT)
        diff = (uint32_t)((uint32_t)t->cmp - (uint32_t)now);
    else
        diff = t->cmp - now;
    if ((int64_t)diff < 0 && !(t->config & TN_32BIT))
        return; /* comparador atras do contador: so dispara apos dar a volta */
    int64_t expire = h->base_ns + (int64_t)((now + diff) * HPET_NS_PER_TICK);
    timer_mod(h->vm, &t->t, expire);
}

static void timer_cb(void *opaque)
{
    hpet_timer *t = opaque;
    hpet *h = t->h;
    timer_fire_irq(t);
    if ((t->config & TN_PERIODIC) && t->period) {
        uint64_t now = counter_now(h);
        if (t->config & TN_32BIT) {
            uint32_t c = (uint32_t)t->cmp;
            do c += (uint32_t)t->period;
            while ((int32_t)(c - (uint32_t)now) <= 0);
            t->cmp = c;
        } else {
            do t->cmp += t->period;
            while ((int64_t)(t->cmp - now) <= 0);
        }
        timer_arm(t);
    } else if (t->config & TN_32BIT) {
        /* comparador de 32 bits: dispara de novo quando o contador der a volta */
        timer_arm(t);
    }
}

static uint32_t reg_read32(hpet *h, unsigned off)
{
    unsigned hi = off & 4;
    unsigned base = off & ~7u;
    uint64_t v;
    if (base >= 0x100 && base < 0x100 + 0x20 * HPET_TIMERS) {
        hpet_timer *t = &h->tm[(base - 0x100) >> 5];
        switch (base & 0x1f) {
        case 0x00:
            v = t->config | TN_SIZE_CAP | ((uint64_t)ROUTE_CAP << 32);
            if (t->n == 0)
                v |= TN_PER_CAP;
            break;
        case 0x08: v = (t->config & TN_32BIT) ? (uint32_t)t->cmp : t->cmp; break;
        default: v = 0; break; /* FSB nao suportado */
        }
    } else {
        switch (base) {
        case 0x000:
            v = 0x01 | ((HPET_TIMERS - 1) << 8) | (1u << 13) | (1u << 15) | (0x8086u << 16) |
                ((uint64_t)HPET_FS_PER_TICK << 32);
            break;
        case 0x010: v = h->config; break;
        case 0x020: v = h->isr; break;
        case 0x0f0: v = counter_now(h); break;
        default: v = 0; break;
        }
    }
    return (uint32_t)(hi ? v >> 32 : v);
}

static void reg_write32(hpet *h, unsigned off, uint32_t val)
{
    unsigned hi = off & 4;
    unsigned base = off & ~7u;
    uint64_t v = hi ? (uint64_t)val << 32 : val;
    uint64_t mask = hi ? 0xffffffff00000000ULL : 0xffffffffULL;
    if (base >= 0x100 && base < 0x100 + 0x20 * HPET_TIMERS) {
        hpet_timer *t = &h->tm[(base - 0x100) >> 5];
        switch (base & 0x1f) {
        case 0x00: {
            uint64_t writable = TN_CFG_WRITABLE & (t->n == 0 ? ~0ULL : ~(uint64_t)TN_PERIODIC);
            uint64_t old = t->config;
            t->config = (t->config & ~(mask & writable)) | (v & mask & writable);
            if (!(t->config & TN_LEVEL) || !(t->config & TN_ENABLE)) {
                /* borda ou desabilitado: derruba a linha */
                if (t->level)
                    set_line(t, 0);
                h->isr &= ~(1ULL << t->n);
            }
            if ((old ^ t->config) & (TN_32BIT)) {
                if (t->config & TN_32BIT) {
                    t->cmp = (uint32_t)t->cmp;
                    t->period = (uint32_t)t->period;
                }
            }
            timer_arm(t);
            break;
        }
        case 0x08: {
            uint64_t nv = v;
            if (t->config & TN_32BIT) {
                if (hi) break;
                nv = (uint32_t)val;
                mask = 0xffffffffULL;
            }
            if (!(t->config & TN_PERIODIC) || (t->config & TN_SETVAL))
                t->cmp = (t->cmp & ~mask) | (nv & mask);
            if (t->config & TN_PERIODIC)
                t->period = (t->period & ~mask) | (nv & mask);
            t->config &= ~(uint64_t)TN_SETVAL;
            timer_arm(t);
            break;
        }
        default: break;
        }
        return;
    }
    switch (base) {
    case 0x010: {
        uint64_t old = h->config;
        uint64_t nc = (h->config & ~mask) | (v & mask);
        nc &= CFG_ENABLE | CFG_LEGACY;
        if ((old ^ nc) & CFG_ENABLE) {
            if (nc & CFG_ENABLE) /* liga: o contador continua do valor guardado */
                h->base_ns = mvm_now(h->vm) - (int64_t)(h->counter * HPET_NS_PER_TICK);
            else
                h->counter = counter_now(h);
        }
        /* troca de rota (legado): derruba as linhas antigas */
        if ((old ^ nc) & CFG_LEGACY)
            for (int i = 0; i < 2; i++)
                if (h->tm[i].level)
                    set_line(&h->tm[i], 0);
        h->config = nc;
        for (int i = 0; i < HPET_TIMERS; i++)
            timer_arm(&h->tm[i]);
        break;
    }
    case 0x020: { /* escreve 1 para limpar (nivel) */
        uint64_t clr = v & mask;
        for (int i = 0; i < HPET_TIMERS; i++) {
            if ((clr >> i) & 1 && (h->isr >> i) & 1) {
                h->isr &= ~(1ULL << i);
                set_line(&h->tm[i], 0);
            }
        }
        break;
    }
    case 0x0f0:
        if (running(h))
            break; /* so pode ser escrito com o HPET parado */
        h->counter = (h->counter & ~mask) | (v & mask);
        break;
    default: break;
    }
}

static uint64_t hpet_read(void *opaque, uint64_t off, unsigned size)
{
    hpet *h = opaque;
    if (size == 8)
        return reg_read32(h, (unsigned)off) | ((uint64_t)reg_read32(h, (unsigned)off + 4) << 32);
    uint32_t v = reg_read32(h, (unsigned)off & ~3u);
    v >>= 8 * (off & 3);
    return size == 4 ? v : v & ((1u << (8 * size)) - 1);
}

static void hpet_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    hpet *h = opaque;
    if (size == 8) {
        reg_write32(h, (unsigned)off, (uint32_t)v);
        reg_write32(h, (unsigned)off + 4, (uint32_t)(v >> 32));
    } else if (size == 4) {
        reg_write32(h, (unsigned)off, (uint32_t)v);
    } else { /* acessos parciais: le-modifica-escreve */
        unsigned o = (unsigned)off & ~3u, sh = 8 * (unsigned)(off & 3);
        uint32_t m = ((1u << (8 * size)) - 1) << sh;
        uint32_t cur = reg_read32(h, o);
        if ((o & ~7u) == 0x020)
            cur = 0; /* ISR: nao reescreve bits que nao foram tocados */
        reg_write32(h, o, (cur & ~m) | (((uint32_t)v << sh) & m));
    }
}

const mvm_io_ops hpet_mmio_ops = {hpet_read, hpet_write};

bool hpet_legacy(const hpet *h) { return (h->config & (CFG_ENABLE | CFG_LEGACY)) == (CFG_ENABLE | CFG_LEGACY); }

uint32_t hpet_block_id(void) { return 0x01 | ((HPET_TIMERS - 1) << 8) | (1u << 13) | (1u << 15) | (0x8086u << 16); }

void hpet_reset(hpet *h)
{
    for (int i = 0; i < HPET_TIMERS; i++) {
        hpet_timer *t = &h->tm[i];
        timer_del(h->vm, &t->t);
        if (t->level)
            set_line(t, 0);
        t->config = 0;
        t->cmp = ~0ULL;
        t->period = 0;
    }
    h->config = 0;
    h->isr = 0;
    h->counter = 0;
}

hpet *hpet_new(mvm_vm *vm, void (*set_irq)(void *opaque, int line, int level), void *opaque)
{
    hpet *h = calloc(1, sizeof(*h));
    h->vm = vm;
    h->set_irq = set_irq;
    h->opaque = opaque;
    for (int i = 0; i < HPET_TIMERS; i++) {
        h->tm[i].h = h;
        h->tm[i].n = i;
        timer_init(&h->tm[i].t, timer_cb, &h->tm[i]);
    }
    hpet_reset(h);
    return h;
}

void hpet_free(hpet *h)
{
    if (!h)
        return;
    for (int i = 0; i < HPET_TIMERS; i++)
        timer_del(h->vm, &h->tm[i].t);
    free(h);
}
