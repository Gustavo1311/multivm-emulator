/*
 * APIC local (xAPIC, 0xFEE00000) e IOAPIC (0xFEC00000) para uma CPU.
 *
 * O LAPIC entrega as interrupcoes fixas (IOAPIC, timer, auto-IPI) com a
 * prioridade TPR/PPR, e repassa a saida do 8259 quando LINT0 esta em ExtINT
 * (modo "virtual wire") ou quando o APIC esta desabilitado no MSR 0x1B.
 */
#include "apic.h"

#include <stdlib.h>

#define APIC_BASE_ADDR 0xfee00000ULL
#define MSR_ENABLE (1ULL << 11)
#define MSR_BSP (1ULL << 8)

#define LVT_TIMER 0
#define LVT_THERMAL 1
#define LVT_PERF 2
#define LVT_LINT0 3
#define LVT_LINT1 4
#define LVT_ERROR 5
#define LVT_N 6
#define LVT_MASKED (1u << 16)

#define BUS_NS_PER_TICK 1 /* barramento do APIC a 1 GHz */

struct lapic {
    mvm_vm *vm;
    void *cpu;
    void (*set_intr)(void *cpu, int level);
    i8259 *pic;
    bool pic_level;
    uint64_t base;
    uint8_t id;
    uint8_t tpr;
    uint32_t ldr, dfr, svr, esr, icr_lo, icr_hi;
    uint32_t lvt[LVT_N];
    uint32_t irr[8], isr[8], tmr[8];
    /* timer */
    uint32_t initial, divide_cfg;
    int64_t start_ns;
    mvm_timer timer;
    void (*eoi_cb)(void *opaque, int vector);
    void *eoi_opaque;
};

struct ioapic {
    mvm_vm *vm;
    lapic *lapic;
    uint8_t sel, id;
    uint64_t redir[IOAPIC_PINS];
    uint32_t level; /* nivel atual de cada entrada */
};

/* ------------------------------------------------------------ LAPIC */

static int highest(const uint32_t *v)
{
    for (int i = 7; i >= 0; i--)
        if (v[i])
            return i * 32 + 31 - __builtin_clz(v[i]);
    return -1;
}

static inline void bit_set(uint32_t *v, int n) { v[n >> 5] |= 1u << (n & 31); }
static inline void bit_clr(uint32_t *v, int n) { v[n >> 5] &= ~(1u << (n & 31)); }
static inline bool bit_get(const uint32_t *v, int n) { return (v[n >> 5] >> (n & 31)) & 1; }

static bool hw_enabled(const lapic *a) { return a->base & MSR_ENABLE; }
static bool sw_enabled(const lapic *a) { return hw_enabled(a) && (a->svr & 0x100); }

static int ppr(const lapic *a)
{
    int isrv = highest(a->isr);
    if (isrv < 0)
        isrv = 0;
    return (a->tpr & 0xf0) >= (isrv & 0xf0) ? a->tpr : (isrv & 0xf0);
}

static int apic_pending_vector(const lapic *a)
{
    if (!hw_enabled(a))
        return -1;
    int v = highest(a->irr);
    if (v < 16 || (v & 0xf0) <= (ppr(a) & 0xf0))
        return -1;
    return v;
}

static bool accept_pic(const lapic *a)
{
    if (!hw_enabled(a))
        return true;
    uint32_t l = a->lvt[LVT_LINT0];
    return !(l & LVT_MASKED) && ((l >> 8) & 7) == 7;
}

static void lapic_update(lapic *a)
{
    bool pend = apic_pending_vector(a) >= 0 || (a->pic_level && accept_pic(a));
    a->set_intr(a->cpu, pend);
}

static void accept_irq(lapic *a, int vector, bool level)
{
    if (vector < 16) {
        a->esr |= 0x40; /* vetor ilegal recebido */
        return;
    }
    bit_set(a->irr, vector);
    if (level)
        bit_set(a->tmr, vector);
    else
        bit_clr(a->tmr, vector);
    lapic_update(a);
}

void lapic_deliver(lapic *a, int vector, bool level)
{
    if (!sw_enabled(a))
        return;
    accept_irq(a, vector & 0xff, level);
}

void lapic_pic_intr(void *opaque, int level)
{
    lapic *a = opaque;
    a->pic_level = level != 0;
    lapic_update(a);
}

int lapic_ack(lapic *a)
{
    int v = apic_pending_vector(a);
    if (v >= 0) {
        bit_clr(a->irr, v);
        bit_set(a->isr, v);
        lapic_update(a);
        return v;
    }
    if (a->pic_level && accept_pic(a))
        return i8259_ack(a->pic);
    if (hw_enabled(a) && highest(a->irr) >= 16)
        return a->svr & 0xff; /* interrupcao espuria (abaixo do PPR) */
    return -1;
}

static void do_eoi(lapic *a)
{
    int v = highest(a->isr);
    if (v < 0)
        return;
    bit_clr(a->isr, v);
    if (bit_get(a->tmr, v)) {
        bit_clr(a->tmr, v);
        if (a->eoi_cb)
            a->eoi_cb(a->eoi_opaque, v);
    }
    lapic_update(a);
}

/* timer */
static uint32_t timer_div(const lapic *a)
{
    uint32_t v = (a->divide_cfg & 3) | ((a->divide_cfg >> 1) & 4);
    return v == 7 ? 1 : 2u << v;
}

static int64_t period_ns(const lapic *a) { return (int64_t)a->initial * timer_div(a) * BUS_NS_PER_TICK; }

static uint32_t timer_current(lapic *a)
{
    if (!a->initial)
        return 0;
    int64_t el = mvm_now(a->vm) - a->start_ns;
    int64_t per = period_ns(a);
    if (el < 0)
        el = 0;
    if (((a->lvt[LVT_TIMER] >> 17) & 3) == 1) /* periodico */
        el %= per;
    else if (el >= per)
        return 0;
    return (uint32_t)((per - el) / ((int64_t)timer_div(a) * BUS_NS_PER_TICK));
}

static void timer_arm(lapic *a)
{
    timer_del(a->vm, &a->timer);
    if (!a->initial)
        return;
    int64_t per = period_ns(a);
    int64_t now = mvm_now(a->vm);
    int64_t next = a->start_ns + per;
    if (((a->lvt[LVT_TIMER] >> 17) & 3) == 1 && next <= now) {
        int64_t n = (now - a->start_ns) / per + 1;
        a->start_ns += (n - 1) * per; /* descarta periodos perdidos */
        next = a->start_ns + per;
    }
    timer_mod(a->vm, &a->timer, next);
}

static void timer_cb(void *opaque)
{
    lapic *a = opaque;
    uint32_t l = a->lvt[LVT_TIMER];
    if (!(l & LVT_MASKED) && sw_enabled(a))
        accept_irq(a, (int)(l & 0xff), false);
    if (((l >> 17) & 3) == 1) {
        a->start_ns += period_ns(a);
        timer_arm(a);
    }
}

/* o destino (modo fisico ou logico) inclui esta CPU? */
static bool dest_match(const lapic *a, uint8_t dest, bool logical)
{
    if (!logical)
        return dest == 0xff || dest == a->id;
    uint8_t ldr = (uint8_t)(a->ldr >> 24);
    if ((a->dfr >> 28) == 0xf) /* modelo flat: um bit por CPU */
        return (dest & ldr) != 0;
    /* modelo cluster: nibble alto = cluster (0xf = todos), nibble baixo = mascara */
    return ((dest >> 4) == 0xf || (dest >> 4) == (ldr >> 4)) && (dest & ldr & 0x0f) != 0;
}

static void send_ipi(lapic *a)
{
    uint32_t lo = a->icr_lo;
    int mode = (lo >> 8) & 7, shorthand = (lo >> 18) & 3;
    uint8_t dest = (uint8_t)(a->icr_hi >> 24);
    bool self;
    switch (shorthand) {
    case 1: case 2: self = true; break;   /* proprio / todos incluindo o proprio */
    case 3: self = false; break;          /* todos exceto o proprio: nao ha outras CPUs */
    default: self = dest_match(a, dest, (lo >> 11) & 1); break;
    }
    /* fixo (0) ou menor prioridade (1); INIT/SIPI/NMI/SMI so fariam sentido com outras CPUs */
    if (!self || (mode != 0 && mode != 1))
        return;
    accept_irq(a, (int)(lo & 0xff), false);
}

static uint32_t lapic_reg_read(lapic *a, unsigned reg)
{
    switch (reg) {
    case 0x02: return (uint32_t)a->id << 24;
    case 0x03: return 0x00050014; /* versao 0x14, 6 LVTs */
    case 0x08: return a->tpr;
    case 0x09: return 0;          /* APR */
    case 0x0a: return (uint32_t)ppr(a);
    case 0x0b: return 0;          /* EOI */
    case 0x0d: return a->ldr;
    case 0x0e: return a->dfr;
    case 0x0f: return a->svr;
    case 0x28: return a->esr;
    case 0x30: return a->icr_lo & ~(1u << 12);
    case 0x31: return a->icr_hi;
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        return a->lvt[reg - 0x32];
    case 0x38: return a->initial;
    case 0x39: return timer_current(a);
    case 0x3e: return a->divide_cfg;
    default:
        if (reg >= 0x10 && reg < 0x18) return a->isr[reg - 0x10];
        if (reg >= 0x18 && reg < 0x20) return a->tmr[reg - 0x18];
        if (reg >= 0x20 && reg < 0x28) return a->irr[reg - 0x20];
        return 0;
    }
}

static void lapic_reg_write(lapic *a, unsigned reg, uint32_t v)
{
    switch (reg) {
    case 0x02: a->id = (uint8_t)(v >> 24); break;
    case 0x08: a->tpr = (uint8_t)v; lapic_update(a); break;
    case 0x0b: do_eoi(a); break;
    case 0x0d: a->ldr = v & 0xff000000u; break;
    case 0x0e: a->dfr = v | 0x0fffffffu; break;
    case 0x0f:
        a->svr = v & 0x3ff;
        if (!(v & 0x100))
            for (int i = 0; i < LVT_N; i++)
                a->lvt[i] |= LVT_MASKED;
        lapic_update(a);
        break;
    case 0x28: a->esr = 0; break;
    case 0x30: a->icr_lo = v; send_ipi(a); break;
    case 0x31: a->icr_hi = v; break;
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
        int i = (int)reg - 0x32;
        if (!(a->svr & 0x100))
            v |= LVT_MASKED;
        static const uint32_t mask[LVT_N] = {0x710ff, 0x117ff, 0x117ff, 0x1f7ff, 0x1f7ff, 0x110ff};
        uint32_t old = a->lvt[LVT_TIMER];
        a->lvt[i] = v & mask[i];
        if (i == LVT_TIMER && ((old ^ a->lvt[i]) & (3u << 17)))
            timer_arm(a);
        lapic_update(a);
        break;
    }
    case 0x38:
        a->initial = v;
        a->start_ns = mvm_now(a->vm);
        timer_arm(a);
        break;
    case 0x3e: {
        uint32_t cur = timer_current(a);
        a->divide_cfg = v & 0xb;
        if (a->initial) { /* mantem a contagem atual com o novo divisor */
            a->start_ns = mvm_now(a->vm) - (int64_t)(a->initial - cur) * timer_div(a) * BUS_NS_PER_TICK;
            timer_arm(a);
        }
        break;
    }
    default: break;
    }
}

static uint64_t lapic_mmio_read(void *opaque, uint64_t off, unsigned size)
{
    lapic *a = opaque;
    uint32_t v = lapic_reg_read(a, (unsigned)(off >> 4) & 0xff);
    unsigned sh = 8 * (unsigned)(off & 3);
    v >>= sh;
    return size >= 4 ? v : v & ((1u << (8 * size)) - 1);
}

static void lapic_mmio_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    lapic *a = opaque;
    if (off & 0xf)
        return; /* apenas acessos alinhados de 32 bits tem efeito */
    (void)size;
    lapic_reg_write(a, (unsigned)(off >> 4) & 0xff, (uint32_t)v);
}

const mvm_io_ops lapic_mmio_ops = {lapic_mmio_read, lapic_mmio_write};

void lapic_reset(lapic *a)
{
    timer_del(a->vm, &a->timer);
    a->base = APIC_BASE_ADDR | MSR_ENABLE | MSR_BSP;
    a->id = 0;
    a->tpr = 0;
    a->ldr = 0;
    a->dfr = 0xffffffffu;
    a->svr = 0xff;
    a->esr = a->icr_lo = a->icr_hi = 0;
    for (int i = 0; i < LVT_N; i++)
        a->lvt[i] = LVT_MASKED;
    /* como no QEMU: LINT0 da BSP ja em ExtINT, para o 8259 funcionar com o APIC ligado */
    a->lvt[LVT_LINT0] = 0x700;
    memset(a->irr, 0, sizeof(a->irr));
    memset(a->isr, 0, sizeof(a->isr));
    memset(a->tmr, 0, sizeof(a->tmr));
    a->initial = a->divide_cfg = 0;
    lapic_update(a);
}

lapic *lapic_new(mvm_vm *vm, void *cpu, void (*set_intr)(void *cpu, int level), i8259 *pic)
{
    lapic *a = calloc(1, sizeof(*a));
    a->vm = vm;
    a->cpu = cpu;
    a->set_intr = set_intr;
    a->pic = pic;
    timer_init(&a->timer, timer_cb, a);
    lapic_reset(a);
    return a;
}

void lapic_free(lapic *a)
{
    if (!a)
        return;
    timer_del(a->vm, &a->timer);
    free(a);
}

void lapic_debug_dump(lapic *a)
{
    LOGI("lapic: base=%llx svr=%x tpr=%x ppr=%x irr=%d isr=%d lvt_t=%x lint0=%x init=%u cur=%u div=%x ativo=%d exp=%lld agora=%lld pic=%d",
         (unsigned long long)a->base, a->svr, a->tpr, ppr(a), highest(a->irr), highest(a->isr), a->lvt[LVT_TIMER],
         a->lvt[LVT_LINT0], a->initial, timer_current(a), a->divide_cfg, a->timer.active, (long long)a->timer.expire,
         (long long)mvm_now(a->vm), a->pic_level);
}

uint64_t lapic_get_base(void *opaque) { return ((lapic *)opaque)->base; }

void lapic_set_base(void *opaque, uint64_t v)
{
    lapic *a = opaque;
    /* a relocacao da janela MMIO nao e suportada: so os bits de controle mudam */
    a->base = (a->base & ~(MSR_ENABLE)) | (v & MSR_ENABLE);
    if (!(a->base & MSR_ENABLE)) {
        for (int i = 0; i < LVT_N; i++)
            a->lvt[i] |= LVT_MASKED;
        a->svr &= ~0x100u;
    }
    lapic_update(a);
}

uint8_t lapic_get_tpr(void *opaque) { return ((lapic *)opaque)->tpr; }

void lapic_set_tpr(void *opaque, uint8_t tpr)
{
    lapic *a = opaque;
    a->tpr = tpr;
    lapic_update(a);
}

void lapic_set_pic(lapic *a, i8259 *pic) { a->pic = pic; }

void lapic_set_eoi_cb(lapic *a, void (*cb)(void *opaque, int vector), void *opaque)
{
    a->eoi_cb = cb;
    a->eoi_opaque = opaque;
}

/* ------------------------------------------------------------ IOAPIC */

#define RED_MASKED (1ULL << 16)
#define RED_LEVEL (1ULL << 15)
#define RED_RIRR (1ULL << 14)

static void ioapic_service(ioapic *io, int pin)
{
    uint64_t r = io->redir[pin];
    if (r & RED_MASKED)
        return;
    bool level = r & RED_LEVEL;
    if (level) {
        if (!((io->level >> pin) & 1) || (r & RED_RIRR))
            return;
        io->redir[pin] |= RED_RIRR;
    }
    int mode = (int)((r >> 8) & 7);
    if (mode == 0 || mode == 1)
        lapic_deliver(io->lapic, (int)(r & 0xff), level);
    /* NMI/SMI/INIT/ExtINT pelo IOAPIC nao sao usados pelos sistemas alvo */
}

void ioapic_set_irq(void *opaque, int pin, int level)
{
    ioapic *io = opaque;
    if (pin < 0 || pin >= IOAPIC_PINS)
        return;
    uint32_t bit = 1u << pin;
    bool was = io->level & bit;
    if (level)
        io->level |= bit;
    else
        io->level &= ~bit;
    if (!level)
        return;
    if (io->redir[pin] & RED_LEVEL)
        ioapic_service(io, pin);
    else if (!was)
        ioapic_service(io, pin);
}

static void ioapic_eoi(void *opaque, int vector)
{
    ioapic *io = opaque;
    for (int i = 0; i < IOAPIC_PINS; i++) {
        uint64_t r = io->redir[i];
        if ((r & RED_LEVEL) && (r & RED_RIRR) && (int)(r & 0xff) == vector) {
            io->redir[i] &= ~RED_RIRR;
            ioapic_service(io, i);
        }
    }
}

static uint32_t ioapic_reg_read(ioapic *io)
{
    unsigned s = io->sel;
    if (s == 0) return (uint32_t)io->id << 24;
    if (s == 1) return 0x11 | ((IOAPIC_PINS - 1) << 16);
    if (s == 2) return (uint32_t)io->id << 24;
    if (s >= 0x10 && s < 0x10 + 2 * IOAPIC_PINS) {
        uint64_t r = io->redir[(s - 0x10) >> 1];
        return (s & 1) ? (uint32_t)(r >> 32) : (uint32_t)r;
    }
    return 0xffffffffu;
}

static void ioapic_reg_write(ioapic *io, uint32_t v)
{
    unsigned s = io->sel;
    if (s == 0) {
        io->id = (uint8_t)((v >> 24) & 0x0f);
    } else if (s >= 0x10 && s < 0x10 + 2 * IOAPIC_PINS) {
        int pin = (int)((s - 0x10) >> 1);
        uint64_t r = io->redir[pin];
        if (s & 1)
            r = (r & 0xffffffffULL) | ((uint64_t)v << 32);
        else /* status de entrega e remote IRR sao somente leitura */
            r = (r & ~0xffffffffULL) | (r & RED_RIRR) | (v & ~(uint32_t)(RED_RIRR | (1u << 12)));
        if (!(r & RED_LEVEL))
            r &= ~RED_RIRR;
        io->redir[pin] = r;
        ioapic_service(io, pin);
    }
}

static uint64_t ioapic_mmio_read(void *opaque, uint64_t off, unsigned size)
{
    ioapic *io = opaque;
    (void)size;
    if (off == 0x00) return io->sel;
    if (off == 0x10) return ioapic_reg_read(io);
    return 0;
}

static void ioapic_mmio_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    ioapic *io = opaque;
    (void)size;
    if (off == 0x00) io->sel = (uint8_t)v;
    else if (off == 0x10) ioapic_reg_write(io, (uint32_t)v);
}

const mvm_io_ops ioapic_mmio_ops = {ioapic_mmio_read, ioapic_mmio_write};

void ioapic_reset(ioapic *io)
{
    io->sel = 0;
    io->id = 0; /* o SeaBIOS atribui o ID (normalmente o numero de CPUs) */
    for (int i = 0; i < IOAPIC_PINS; i++)
        io->redir[i] = RED_MASKED;
}

ioapic *ioapic_new(mvm_vm *vm, lapic *a)
{
    ioapic *io = calloc(1, sizeof(*io));
    io->vm = vm;
    io->lapic = a;
    lapic_set_eoi_cb(a, ioapic_eoi, io);
    ioapic_reset(io);
    return io;
}

void ioapic_free(ioapic *io) { free(io); }
