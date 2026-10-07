/* Dispositivos legados do PC: PIC 8259, PIT 8254, RTC/CMOS, UART 16550A, i8042. */
#include "devices.h"

#include <stdlib.h>
#include <time.h>

/* ================================================================ 8259 */
static void pit_note(int64_t t, char what, int a, int b);

typedef struct {
    uint8_t irr, imr, isr, last_irr, elcr, elcr_mask;
    uint8_t priority_add, irq_base, read_reg_select, poll, special_mask;
    uint8_t init_state, auto_eoi, rotate_on_auto_eoi, sfnm, init4, single_mode;
} pic_chip;

struct i8259 {
    mvm_vm *vm;
    pic_chip p[2];
    void (*cpu_intr)(void *cpu, int level);
    void *cpu;
};

static int pic_priority(pic_chip *s, uint8_t mask)
{
    if (!mask)
        return 8;
    int pr = 0;
    while (!(mask & (1u << ((pr + s->priority_add) & 7))))
        pr++;
    return pr;
}

static int pic_get_irq(pic_chip *s, bool master)
{
    uint8_t mask = s->irr & ~s->imr;
    int pr = pic_priority(s, mask);
    if (pr == 8)
        return -1;
    mask = s->isr;
    if (s->special_mask)
        mask &= ~s->imr;
    if (s->sfnm && master)
        mask &= ~(1u << 2);
    int cur = pic_priority(s, mask);
    return pr < cur ? (pr + s->priority_add) & 7 : -1;
}

static void pic_set_level(pic_chip *s, int irq, int level)
{
    uint8_t mask = (uint8_t)(1u << irq);
    if (s->elcr & mask) {
        if (level) { s->irr |= mask; s->last_irr |= mask; }
        else { s->irr &= ~mask; s->last_irr &= ~mask; }
    } else {
        if (level) {
            if (!(s->last_irr & mask))
                s->irr |= mask;
            s->last_irr |= mask;
        } else {
            s->last_irr &= ~mask;
        }
    }
}

static void pic_update(i8259 *p)
{
    int irq2 = pic_get_irq(&p->p[1], false);
    pic_set_level(&p->p[0], 2, irq2 >= 0);
    int irq = pic_get_irq(&p->p[0], true);
    p->cpu_intr(p->cpu, irq >= 0);
}

void i8259_set_irq(void *opaque, int irq, int level)
{
    i8259 *p = opaque;
    if (irq < 0 || irq > 15)
        return;
    pic_set_level(&p->p[irq >> 3], irq & 7, level);
    pic_update(p);
}

static void pic_intack(pic_chip *s, int irq)
{
    if (s->auto_eoi) {
        if (s->rotate_on_auto_eoi)
            s->priority_add = (uint8_t)((irq + 1) & 7);
    } else {
        s->isr |= (uint8_t)(1u << irq);
    }
    if (!(s->elcr & (1u << irq)))
        s->irr &= (uint8_t)~(1u << irq);
}

int i8259_ack(i8259 *p)
{
    int irq = pic_get_irq(&p->p[0], true), intno;
    if (irq >= 0) {
        if (irq == 2) {
            int irq2 = pic_get_irq(&p->p[1], false);
            if (irq2 >= 0)
                pic_intack(&p->p[1], irq2);
            else
                irq2 = 7;
            intno = p->p[1].irq_base + irq2;
        } else {
            intno = p->p[0].irq_base + irq;
            if (irq == 0)
                pit_note(mvm_now(p->vm), 'A', intno, p->p[0].imr);
        }
        pic_intack(&p->p[0], irq);
    } else {
        intno = p->p[0].irq_base + 7;
    }
    pic_update(p);
    return intno;
}

void i8259_debug_dump(i8259 *p)
{
    LOGI("pic: m irr=%02x imr=%02x isr=%02x elcr=%02x base=%02x | s irr=%02x imr=%02x isr=%02x elcr=%02x", p->p[0].irr,
         p->p[0].imr, p->p[0].isr, p->p[0].elcr, p->p[0].irq_base, p->p[1].irr, p->p[1].imr, p->p[1].isr, p->p[1].elcr);
}

bool i8259_pending(i8259 *p) { return pic_get_irq(&p->p[0], true) >= 0; }

static void pic_chip_reset(pic_chip *s)
{
    uint8_t elcr = s->elcr, mask = s->elcr_mask;
    memset(s, 0, sizeof(*s));
    s->elcr = elcr;
    s->elcr_mask = mask;
}

void i8259_reset(i8259 *p)
{
    pic_chip_reset(&p->p[0]);
    pic_chip_reset(&p->p[1]);
    pic_update(p);
}

i8259 *i8259_new(mvm_vm *vm, void (*cpu_intr)(void *cpu, int level), void *cpu)
{
    i8259 *p = calloc(1, sizeof(*p));
    p->vm = vm;
    p->cpu_intr = cpu_intr;
    p->cpu = cpu;
    p->p[0].elcr_mask = 0xf8;
    p->p[1].elcr_mask = 0xde;
    return p;
}

static void pic_write(i8259 *p, int chip, uint64_t off, uint8_t val)
{
    pic_chip *s = &p->p[chip];
    if (off == 0) {
        if (val & 0x10) {
            pic_chip_reset(s);
            s->init_state = 1;
            s->init4 = val & 1;
            s->single_mode = (val >> 1) & 1;
        } else if (val & 0x08) {
            if (val & 0x04) s->poll = 1;
            if (val & 0x02) s->read_reg_select = val & 1;
            if (val & 0x40) s->special_mask = (val >> 5) & 1;
        } else {
            int cmd = val >> 5;
            switch (cmd) {
            case 0: case 4:
                s->rotate_on_auto_eoi = (uint8_t)(cmd >> 2);
                break;
            case 1: case 5: {
                int pr = pic_priority(s, s->isr);
                if (pr != 8) {
                    int irq = (pr + s->priority_add) & 7;
                    s->isr &= (uint8_t)~(1u << irq);
                    if (cmd == 5)
                        s->priority_add = (uint8_t)((irq + 1) & 7);
                }
                break;
            }
            case 3:
                s->isr &= (uint8_t)~(1u << (val & 7));
                break;
            case 6:
                s->priority_add = (uint8_t)((val + 1) & 7);
                break;
            case 7: {
                int irq = val & 7;
                s->isr &= (uint8_t)~(1u << irq);
                s->priority_add = (uint8_t)((irq + 1) & 7);
                break;
            }
            default:
                break;
            }
        }
    } else {
        switch (s->init_state) {
        case 0: s->imr = val; break;
        case 1:
            s->irq_base = val & 0xf8;
            s->init_state = s->single_mode ? (s->init4 ? 3 : 0) : 2;
            break;
        case 2: s->init_state = s->init4 ? 3 : 0; break;
        case 3:
            s->sfnm = (val >> 4) & 1;
            s->auto_eoi = (val >> 1) & 1;
            s->init_state = 0;
            break;
        }
    }
    pic_update(p);
}

static uint8_t pic_read(i8259 *p, int chip, uint64_t off)
{
    pic_chip *s = &p->p[chip];
    if (s->poll) {
        s->poll = 0;
        int irq = pic_get_irq(s, chip == 0);
        if (irq >= 0) {
            pic_intack(s, irq);
            pic_update(p);
            return (uint8_t)(irq | 0x80);
        }
        return 0;
    }
    if (off == 0)
        return s->read_reg_select ? s->isr : s->irr;
    return s->imr;
}

static uint64_t picm_r(void *o, uint64_t off, unsigned sz) { (void)sz; return pic_read(o, 0, off); }
static void picm_w(void *o, uint64_t off, uint64_t v, unsigned sz) { (void)sz; pic_write(o, 0, off, (uint8_t)v); }
static uint64_t pics_r(void *o, uint64_t off, unsigned sz) { (void)sz; return pic_read(o, 1, off); }
static void pics_w(void *o, uint64_t off, uint64_t v, unsigned sz) { (void)sz; pic_write(o, 1, off, (uint8_t)v); }
static uint64_t elcr_r(void *o, uint64_t off, unsigned sz)
{
    i8259 *p = o;
    (void)sz;
    return p->p[off & 1].elcr;
}
static void elcr_w(void *o, uint64_t off, uint64_t v, unsigned sz)
{
    i8259 *p = o;
    (void)sz;
    p->p[off & 1].elcr = (uint8_t)v & p->p[off & 1].elcr_mask;
    pic_update(p);
}

const mvm_io_ops i8259_master_ops = {picm_r, picm_w};
const mvm_io_ops i8259_slave_ops = {pics_r, pics_w};
const mvm_io_ops i8259_elcr_ops = {elcr_r, elcr_w};

/* ================================================================ 8254 */

/* depuracao: historico dos ultimos eventos do PIT/IRQ0 (MVM_PC_DEBUG) */
#define PIT_HIST 64
static struct { int64_t t; char what; int a, b; } pit_hist[PIT_HIST];
static unsigned pit_hist_pos;
static void pit_note(int64_t t, char what, int a, int b)
{
    unsigned i = pit_hist_pos++ % PIT_HIST;
    pit_hist[i].t = t; pit_hist[i].what = what; pit_hist[i].a = a; pit_hist[i].b = b;
}


#define PIT_FREQ 1193182LL

typedef struct {
    int count;          /* 1..65536 */
    uint16_t latched_count;
    uint8_t count_latched, status_latched, status, read_state, write_state, write_latch;
    uint8_t rw_mode, mode, bcd, gate;
    int64_t load_time;
    bool armed;         /* aguardando a carga do contador */
} pit_chan;

struct i8254 {
    mvm_vm *vm;
    pit_chan ch[3];
    irq_line irq0;
    mvm_timer t0;
    uint8_t port61;
    int64_t next_irq_tick;
};

static int64_t pit_ticks(i8254 *p, int64_t since)
{
    int64_t ns = mvm_now(p->vm) - since;
    if (ns < 0) ns = 0;
    return (int64_t)((__int128)ns * PIT_FREQ / 1000000000LL);
}

static int pit_get_count(i8254 *p, pit_chan *s)
{
    int64_t d = pit_ticks(p, s->load_time);
    int c;
    switch (s->mode) {
    case 0: case 1: case 4: case 5: c = (int)((s->count - d) & 0xffff); break;
    case 3: c = (int)(s->count - ((2 * d) % s->count)); break;
    default: c = (int)(s->count - (d % s->count)); break;
    }
    return c;
}

static int pit_get_out(i8254 *p, pit_chan *s)
{
    int64_t d = pit_ticks(p, s->load_time);
    switch (s->mode) {
    case 0: return d >= s->count;
    case 1: return d < s->count;
    case 2: return (d % s->count) != 0 || d == 0;
    case 3: return (d % s->count) < ((s->count + 1) >> 1);
    default: return d == s->count;
    }
}

static int64_t ticks_to_ns(int64_t t) { return (int64_t)((__int128)t * 1000000000LL / PIT_FREQ); }

static void pit_irq_timer(void *opaque);

/* fired: chamado apos o disparo (nos modos 0/4 nao ha proximo evento). Numa carga
 * nova o evento e sempre agendado, mesmo que o prazo ja tenha passado (a thread do
 * emulador pode ter sido preemptada): ele dispara na proxima passada dos timers. */
static void pit_schedule(i8254 *p, bool fired)
{
    pit_chan *s = &p->ch[0];
    timer_del(p->vm, &p->t0);
    if (s->armed)
        return;
    int64_t next;
    switch (s->mode) {
    case 2: case 3: {
        int64_t d = pit_ticks(p, s->load_time);
        next = (d / s->count + 1) * s->count;
        break;
    }
    case 0: case 4:
        if (fired)
            return;
        next = s->count;
        break;
    default:
        return;
    }
    timer_mod(p->vm, &p->t0, s->load_time + ticks_to_ns(next) + 1);
}

static void pit_irq_timer(void *opaque)
{
    i8254 *p = opaque;
    pit_note(mvm_now(p->vm), 'F', p->ch[0].mode, p->ch[0].count);
    irq_set(&p->irq0, 1);
    irq_set(&p->irq0, 0);
    pit_schedule(p, true);
}

static void pit_load(i8254 *p, int ch, int val)
{
    pit_chan *s = &p->ch[ch];
    if (!val)
        val = 0x10000;
    s->count = val;
    s->load_time = mvm_now(p->vm);
    s->armed = false;
    if (ch == 0) {
        pit_note(s->load_time, 'L', s->mode, val);
        pit_schedule(p, false);
    }
}

void i8254_reset(i8254 *p)
{
    for (int i = 0; i < 3; i++) {
        pit_chan *s = &p->ch[i];
        memset(s, 0, sizeof(*s));
        s->mode = 3;
        s->gate = i != 2;
        s->count = 0x10000;
        s->load_time = mvm_now(p->vm);
        s->armed = true;
    }
    timer_del(p->vm, &p->t0);
    p->port61 = 0;
}

i8254 *i8254_new(mvm_vm *vm, irq_line irq0)
{
    i8254 *p = calloc(1, sizeof(*p));
    p->vm = vm;
    p->irq0 = irq0;
    timer_init(&p->t0, pit_irq_timer, p);
    i8254_reset(p);
    return p;
}

static void pit_latch(i8254 *p, pit_chan *s)
{
    if (!s->count_latched) {
        s->latched_count = (uint16_t)pit_get_count(p, s);
        s->count_latched = s->rw_mode;
    }
}

static uint64_t pit_read(void *opaque, uint64_t off, unsigned size)
{
    i8254 *p = opaque;
    (void)size;
    if (off == 3)
        return 0xff;
    pit_chan *s = &p->ch[off];
    if (s->status_latched) {
        s->status_latched = 0;
        return s->status;
    }
    int ret;
    if (s->count_latched) {
        switch (s->count_latched) {
        case 1: ret = s->latched_count & 0xff; s->count_latched = 0; break;
        case 2: ret = s->latched_count >> 8; s->count_latched = 0; break;
        default: ret = s->latched_count & 0xff; s->count_latched = 2; break;
        }
        return (uint8_t)ret;
    }
    int count = pit_get_count(p, s);
    switch (s->read_state) {
    case 1: ret = count & 0xff; break;
    case 2: ret = (count >> 8) & 0xff; break;
    case 3: ret = count & 0xff; s->read_state = 4; break;
    default: ret = (count >> 8) & 0xff; s->read_state = 3; break;
    }
    return (uint8_t)ret;
}

static void pit_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    i8254 *p = opaque;
    uint8_t val = (uint8_t)v;
    (void)size;
    if (off == 3) {
        int ch = val >> 6;
        if (ch == 3) { /* read-back */
            for (int i = 0; i < 3; i++) {
                if (!(val & (2 << i))) continue;
                pit_chan *s = &p->ch[i];
                if (!(val & 0x20)) pit_latch(p, s);
                if (!(val & 0x10) && !s->status_latched) {
                    s->status = (uint8_t)((pit_get_out(p, s) << 7) | (s->rw_mode << 4) | (s->mode << 1) | s->bcd);
                    s->status_latched = 1;
                }
            }
            return;
        }
        pit_chan *s = &p->ch[ch];
        int access = (val >> 4) & 3;
        if (access == 0) {
            pit_latch(p, s);
        } else {
            s->rw_mode = (uint8_t)access;
            s->read_state = (uint8_t)access;
            s->write_state = (uint8_t)access;
            s->mode = (val >> 1) & 7;
            if (s->mode > 5) s->mode -= 4;
            s->bcd = val & 1;
            s->armed = true;
            if (ch == 0) {
                pit_note(mvm_now(p->vm), 'M', s->mode, val);
                timer_del(p->vm, &p->t0);
            }
        }
        return;
    }
    pit_chan *s = &p->ch[off];
    switch (s->write_state) {
    case 1: pit_load(p, (int)off, val); break;
    case 2: pit_load(p, (int)off, val << 8); break;
    case 3: s->write_latch = val; s->write_state = 4; break;
    default: pit_load(p, (int)off, s->write_latch | (val << 8)); s->write_state = 3; break;
    }
}

void i8254_debug_dump(i8254 *p)
{
    pit_chan *s = &p->ch[0];
    LOGI("pit0: modo=%d count=%d armed=%d decorrido=%lld timer=%d exp=%lld agora=%lld", s->mode, s->count, s->armed,
         (long long)pit_ticks(p, s->load_time), p->t0.active, (long long)p->t0.expire, (long long)mvm_now(p->vm));
    for (unsigned k = 0; k < PIT_HIST; k++) {
        unsigned i = (pit_hist_pos + k) % PIT_HIST;
        if (pit_hist[i].what)
            LOGI("  pit %lld %c %d %d", (long long)pit_hist[i].t, pit_hist[i].what, pit_hist[i].a, pit_hist[i].b);
    }
}

uint8_t i8254_port61_read(i8254 *p)
{
    pit_chan *s = &p->ch[2];
    int out = s->armed ? 0 : pit_get_out(p, s);
    int refresh = (int)((mvm_now(p->vm) / 15000) & 1);
    return (uint8_t)((p->port61 & 3) | (out << 5) | (refresh << 4));
}

void i8254_port61_write(i8254 *p, uint8_t v)
{
    pit_chan *s = &p->ch[2];
    int gate = v & 1;
    if (gate && !s->gate && (s->mode == 1 || s->mode == 5 || s->mode == 2 || s->mode == 3))
        s->load_time = mvm_now(p->vm);
    s->gate = (uint8_t)gate;
    p->port61 = v & 3;
}

const mvm_io_ops i8254_ops = {pit_read, pit_write};

/* ================================================================ CMOS */

/*
 * RTC MC146818 + CMOS. O relogio segue o relogio do convidado (mvm_now), com
 * um deslocamento para a hora de parede fixado na criacao: se o emulador frear
 * o tempo, o RTC fica coerente com TSC/PIT/APIC.
 * Registradores A (taxa, UIP), B (PIE/AIE/UIE, BCD, 24h) e C (IRQF/PF/AF/UF).
 */
#define RTC_A_UIP 0x80
#define RTC_B_SET 0x80
#define RTC_B_PIE 0x40
#define RTC_B_AIE 0x20
#define RTC_B_UIE 0x10
#define RTC_C_IRQF 0x80
#define RTC_C_PF 0x40
#define RTC_C_AF 0x20
#define RTC_C_UF 0x10
#define RTC_UIP_NS 244000LL

struct cmos {
    mvm_vm *vm;
    irq_line irq8;
    uint8_t idx;
    uint8_t ram[128];
    int64_t epoch_ns;      /* hora de parede (ns desde 1970) = mvm_now + epoch_ns */
    mvm_timer periodic, update;
    int64_t pf_checked;    /* ultimo instante em que PF foi avaliado (sem PIE) */
    int level;
};

static int64_t rtc_ns(cmos *c) { return mvm_now(c->vm) + c->epoch_ns; }

static int64_t rtc_period_ns(cmos *c)
{
    unsigned rate = c->ram[0x0a] & 0x0f;
    if (!rate || (c->ram[0x0a] & 0x70) != 0x20) /* divisor desligado */
        return 0;
    if (rate <= 2)
        rate += 7;
    return (int64_t)((1ULL << (rate - 1)) * 1000000000ULL / 32768);
}

static void rtc_update_irq(cmos *c)
{
    uint8_t flags = c->ram[0x0c] & c->ram[0x0b] & (RTC_C_PF | RTC_C_AF | RTC_C_UF);
    if (flags)
        c->ram[0x0c] |= RTC_C_IRQF;
    int level = (c->ram[0x0c] & RTC_C_IRQF) != 0;
    if (level != c->level) {
        c->level = level;
        irq_set(&c->irq8, level);
    }
}

static void rtc_arm_periodic(cmos *c)
{
    timer_del(c->vm, &c->periodic);
    int64_t per = rtc_period_ns(c);
    if (!per || !(c->ram[0x0b] & RTC_B_PIE))
        return;
    int64_t now = rtc_ns(c);
    int64_t next = (now / per + 1) * per; /* alinhado como o divisor do chip */
    timer_mod(c->vm, &c->periodic, next - c->epoch_ns);
}

static void rtc_periodic_cb(void *opaque)
{
    cmos *c = opaque;
    c->ram[0x0c] |= RTC_C_PF;
    rtc_update_irq(c);
    rtc_arm_periodic(c);
}

static bool rtc_alarm_match(cmos *c, const struct tm *tm);

static void rtc_arm_update(cmos *c)
{
    timer_del(c->vm, &c->update);
    if (!(c->ram[0x0b] & (RTC_B_UIE | RTC_B_AIE)) || (c->ram[0x0b] & RTC_B_SET))
        return;
    int64_t next = (rtc_ns(c) / 1000000000LL + 1) * 1000000000LL;
    timer_mod(c->vm, &c->update, next - c->epoch_ns);
}

static void rtc_now_tm(cmos *c, struct tm *tm)
{
    time_t t = (time_t)(rtc_ns(c) / 1000000000LL);
    gmtime_r(&t, tm);
}

static void rtc_update_cb(void *opaque)
{
    cmos *c = opaque;
    struct tm tm;
    rtc_now_tm(c, &tm);
    c->ram[0x0c] |= RTC_C_UF;
    if (rtc_alarm_match(c, &tm))
        c->ram[0x0c] |= RTC_C_AF;
    rtc_update_irq(c);
    rtc_arm_update(c);
}

cmos *cmos_new(mvm_vm *vm, irq_line irq8)
{
    cmos *c = calloc(1, sizeof(*c));
    c->vm = vm;
    c->irq8 = irq8;
    c->ram[0x0a] = 0x26;
    c->ram[0x0b] = 0x02;
    c->ram[0x0d] = 0x80;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (vm->cfg.rtc_base) { /* data fixa (ex.: software com data de validade) */
        ts.tv_sec = (time_t)vm->cfg.rtc_base;
        ts.tv_nsec = 0;
    }
    c->epoch_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec - mvm_now(vm);
    timer_init(&c->periodic, rtc_periodic_cb, c);
    timer_init(&c->update, rtc_update_cb, c);
    return c;
}

void cmos_set(cmos *c, int idx, uint8_t v)
{
    if (idx >= 0 && idx < 128)
        c->ram[idx] = v;
}

static uint8_t to_bcd(cmos *c, int v)
{
    if (c->ram[0x0b] & 4)
        return (uint8_t)v;
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int from_bcd(cmos *c, uint8_t v)
{
    if (c->ram[0x0b] & 4)
        return v;
    return (v >> 4) * 10 + (v & 15);
}

static uint8_t rtc_hour_reg(cmos *c, int h)
{
    if (!(c->ram[0x0b] & 2)) {
        int pm = h >= 12;
        h = h % 12;
        if (!h) h = 12;
        return (uint8_t)(to_bcd(c, h) | (pm ? 0x80 : 0));
    }
    return to_bcd(c, h);
}

static bool rtc_alarm_match(cmos *c, const struct tm *tm)
{
    /* valores 0xC0-0xFF nos registradores de alarme significam "qualquer" */
    uint8_t s = c->ram[0x01], m = c->ram[0x03], h = c->ram[0x05];
    if ((s & 0xc0) != 0xc0 && s != to_bcd(c, tm->tm_sec)) return false;
    if ((m & 0xc0) != 0xc0 && m != to_bcd(c, tm->tm_min)) return false;
    if ((h & 0xc0) != 0xc0 && h != rtc_hour_reg(c, tm->tm_hour)) return false;
    return true;
}

static uint64_t cmos_read(void *opaque, uint64_t off, unsigned size)
{
    cmos *c = opaque;
    (void)size;
    if (off == 0)
        return 0xff;
    struct tm tm;
    rtc_now_tm(c, &tm);
    switch (c->idx) {
    case 0x00: return to_bcd(c, tm.tm_sec);
    case 0x02: return to_bcd(c, tm.tm_min);
    case 0x04: return rtc_hour_reg(c, tm.tm_hour);
    case 0x06: return to_bcd(c, tm.tm_wday + 1);
    case 0x07: return to_bcd(c, tm.tm_mday);
    case 0x08: return to_bcd(c, tm.tm_mon + 1);
    case 0x09: return to_bcd(c, tm.tm_year % 100);
    case 0x32: return to_bcd(c, (tm.tm_year + 1900) / 100);
    case 0x0a: {
        uint8_t v = c->ram[0x0a] & 0x7f;
        /* UIP: 244 us antes de cada atualizacao do relogio */
        if (!(c->ram[0x0b] & RTC_B_SET) && (c->ram[0x0a] & 0x70) == 0x20 &&
            rtc_ns(c) % 1000000000LL >= 1000000000LL - RTC_UIP_NS)
            v |= RTC_A_UIP;
        return v;
    }
    case 0x0c: {
        /* sem PIE, PF e avaliado aqui (software que faz polling do flag) */
        int64_t per = rtc_period_ns(c);
        if (per && !(c->ram[0x0b] & RTC_B_PIE)) {
            int64_t now = rtc_ns(c);
            if (now / per != c->pf_checked / per)
                c->ram[0x0c] |= RTC_C_PF;
            c->pf_checked = now;
        }
        uint8_t v = c->ram[0x0c];
        c->ram[0x0c] = 0;
        rtc_update_irq(c);
        return v;
    }
    default: return c->ram[c->idx & 0x7f];
    }
}

static void cmos_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    cmos *c = opaque;
    (void)size;
    if (off == 0) {
        c->idx = (uint8_t)(v & 0x7f);
        return;
    }
    uint8_t val = (uint8_t)v;
    switch (c->idx) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x07: case 0x08: case 0x09: case 0x32: {
        /* ajusta o deslocamento do relogio, preservando a fracao de segundo */
        struct tm tm;
        rtc_now_tm(c, &tm);
        int n;
        if (c->idx == 0x04) {
            n = from_bcd(c, val & 0x7f);
            if (!(c->ram[0x0b] & 2)) {
                n %= 12;
                if (val & 0x80) n += 12;
            }
        } else {
            n = from_bcd(c, val);
        }
        if (c->idx == 0x00) tm.tm_sec = n;
        else if (c->idx == 0x02) tm.tm_min = n;
        else if (c->idx == 0x04) tm.tm_hour = n;
        else if (c->idx == 0x06) break; /* dia da semana e derivado da data */
        else if (c->idx == 0x07) tm.tm_mday = n;
        else if (c->idx == 0x08) tm.tm_mon = n - 1;
        else if (c->idx == 0x09) tm.tm_year = n + ((tm.tm_year + 1900) / 100) * 100 - 1900;
        else tm.tm_year = n * 100 + (tm.tm_year % 100) - 1900; /* seculo */
        int64_t frac = rtc_ns(c) % 1000000000LL;
        int64_t t = (int64_t)timegm(&tm);
        c->epoch_ns = t * 1000000000LL + frac - mvm_now(c->vm);
        rtc_arm_periodic(c);
        rtc_arm_update(c);
        break;
    }
    case 0x0a:
        c->ram[0x0a] = val & 0x7f;
        rtc_arm_periodic(c);
        break;
    case 0x0b:
        c->ram[0x0b] = val;
        if (val & RTC_B_SET)
            c->ram[0x0b] &= ~RTC_B_UIE; /* SET inibe as atualizacoes */
        rtc_update_irq(c);
        rtc_arm_periodic(c);
        rtc_arm_update(c);
        break;
    case 0x0c: case 0x0d: break;
    default: c->ram[c->idx] = val; break;
    }
}

void cmos_reset(cmos *c)
{
    timer_del(c->vm, &c->periodic);
    timer_del(c->vm, &c->update);
    c->ram[0x0b] &= ~(RTC_B_PIE | RTC_B_AIE | RTC_B_UIE | RTC_B_SET);
    c->ram[0x0c] = 0;
    rtc_update_irq(c);
}

const mvm_io_ops cmos_ops = {cmos_read, cmos_write};

/* ================================================================ 16550A */

#define UART_FIFO 16

struct uart16550 {
    mvm_vm *vm;
    mvm_chardev *chr;
    irq_line irq;
    uint16_t divider;
    uint8_t ier, iir, lcr, mcr, lsr, msr, scr, fcr;
    uint8_t fifo[UART_FIFO];
    unsigned rpos, count;
    bool thr_ipending;
};

static void uart_update(uart16550 *u)
{
    uint8_t iir = 1;
    if ((u->ier & 4) && (u->lsr & 0x1e)) iir = 6;
    else if ((u->ier & 1) && (u->lsr & 1)) iir = 4;
    else if ((u->ier & 2) && u->thr_ipending) iir = 2;
    else if ((u->ier & 8) && (u->msr & 0x0f)) iir = 0;
    u->iir = (uint8_t)(iir | (u->iir & 0xf0));
    irq_set(&u->irq, iir != 1);
}

#define UART_HIST 96
static struct { char rw; uint8_t off, val, ier; } uart_hist[UART_HIST];
static unsigned uart_hist_pos;
static void uart_note(char rw, unsigned off, uint8_t val, uint8_t ier)
{
    if (rw == 'W' && off == 0) { /* agrupa escritas no THR */
        unsigned last = (uart_hist_pos - 1) % UART_HIST;
        if (uart_hist_pos && uart_hist[last].rw == 'T') { uart_hist[last].val++; return; }
        rw = 'T'; val = 1;
    }
    unsigned i = uart_hist_pos++ % UART_HIST;
    uart_hist[i].rw = rw; uart_hist[i].off = (uint8_t)off; uart_hist[i].val = val; uart_hist[i].ier = ier;
}

void uart16550_debug_dump(uart16550 *u)
{
    char buf[UART_HIST * 12 + 1];
    int o = 0;
    for (unsigned k = 0; k < UART_HIST; k++) {
        unsigned i = (uart_hist_pos + k) % UART_HIST;
        if (uart_hist[i].rw)
            o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %c%u=%x", uart_hist[i].rw, uart_hist[i].off, uart_hist[i].val);
    }
    buf[o] = 0;
    LOGI("uart hist:%s", buf);
    LOGI("uart: ier=%x iir=%x lcr=%x mcr=%x lsr=%x msr=%x thr_pend=%d rx=%u", u->ier, u->iir, u->lcr, u->mcr, u->lsr,
         u->msr, u->thr_ipending, u->count);
}

static void uart_rx(uart16550 *u, uint8_t ch)
{
    if (u->count < UART_FIFO) {
        u->fifo[(u->rpos + u->count) % UART_FIFO] = ch;
        u->count++;
    } else {
        u->lsr |= 2; /* overrun */
    }
    u->lsr |= 1;
}

uart16550 *uart16550_new(mvm_vm *vm, mvm_chardev *chr, irq_line irq)
{
    uart16550 *u = calloc(1, sizeof(*u));
    u->vm = vm;
    u->chr = chr;
    u->irq = irq;
    u->lsr = 0x60;
    u->msr = 0xb0;
    u->iir = 1;
    u->divider = 12;
    return u;
}

void uart16550_poll(uart16550 *u)
{
    if (u->mcr & 0x10)
        return; /* loopback */
    bool got = false;
    while (u->count < UART_FIFO) {
        int ch = chr_read_byte(u->chr);
        if (ch < 0) break;
        uart_rx(u, (uint8_t)ch);
        got = true;
    }
    if (got)
        uart_update(u);
}

static uint64_t uart_read_(uart16550 *u, uint64_t off);
static uint64_t uart_read(void *opaque, uint64_t off, unsigned size)
{
    uart16550 *u = opaque;
    (void)size;
    uint64_t v = uart_read_(u, off);
    if (off != 5 || (v & 0x60) != 0x60) /* leituras de LSR ociosas poluem o historico */
        uart_note('R', (unsigned)off, (uint8_t)v, u->ier);
    return v;
}

static uint64_t uart_read_(uart16550 *u, uint64_t off)
{
    switch (off) {
    case 0:
        if (u->lcr & 0x80) return u->divider & 0xff;
        {
            uint8_t v = 0;
            if (u->count) {
                v = u->fifo[u->rpos];
                u->rpos = (u->rpos + 1) % UART_FIFO;
                u->count--;
            }
            if (!u->count) u->lsr &= ~1u;
            uart_update(u);
            return v;
        }
    case 1: return (u->lcr & 0x80) ? (uint64_t)(u->divider >> 8) : u->ier;
    case 2: {
        uint8_t v = u->iir;
        if ((v & 0x0f) == 2) {
            u->thr_ipending = false;
            uart_update(u);
        }
        return v;
    }
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5: {
        uint8_t v = u->lsr;
        u->lsr &= ~0x1eu;
        uart_update(u);
        return v;
    }
    case 6:
        if (u->mcr & 0x10) {
            return (uint64_t)(((u->mcr & 0x0c) << 4) | ((u->mcr & 0x02) << 3) | ((u->mcr & 0x01) << 5));
        } else {
            uint8_t v = u->msr;
            u->msr &= 0xf0;
            uart_update(u);
            return v;
        }
    default: return u->scr;
    }
}

static void uart_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    uart16550 *u = opaque;
    uint8_t val = (uint8_t)v;
    (void)size;
    uart_note('W', (unsigned)off, val, u->ier);
    switch (off) {
    case 0:
        if (u->lcr & 0x80) {
            u->divider = (uint16_t)((u->divider & 0xff00) | val);
            return;
        }
        if (u->mcr & 0x10) uart_rx(u, val);
        else chr_write(u->chr, &val, 1);
        u->lsr |= 0x60;
        u->thr_ipending = true;
        uart_update(u);
        return;
    case 1:
        if (u->lcr & 0x80) {
            u->divider = (uint16_t)((u->divider & 0xff) | (val << 8));
            return;
        }
        {
            uint8_t changed = (u->ier ^ val) & 0x0f;
            u->ier = val & 0x0f;
            if ((changed & 2) && (u->ier & 2) && (u->lsr & 0x20))
                u->thr_ipending = true;
            uart_update(u);
        }
        return;
    case 2:
        if (val & 2) {
            u->count = 0;
            u->rpos = 0;
            u->lsr &= ~1u;
        }
        u->fcr = val & 0xc9;
        if (val & 1) u->iir |= 0xc0;
        else u->iir &= 0x3f;
        uart_update(u);
        return;
    case 3: u->lcr = val; return;
    case 4: u->mcr = val & 0x1f; return;
    case 7: u->scr = val; return;
    default: return;
    }
}

const mvm_io_ops uart16550_ops = {uart_read, uart_write};

/* ================================================================ i8042 */

#define KBD_QUEUE 256

typedef struct {
    uint8_t data[KBD_QUEUE];
    unsigned rpos, count;
} kqueue;

struct i8042 {
    mvm_vm *vm;
    irq_line kirq, mirq;
    uint8_t status, mode, cmd, out_port;
    uint8_t outbuf;
    bool outbuf_aux, outbuf_full;
    kqueue kq, mq;
    /* teclado */
    uint8_t kbd_cmd;
    bool kbd_enabled;
    uint8_t scancode_set;
    /* mouse */
    uint8_t mouse_cmd;
    bool mouse_enabled;
    uint8_t mouse_rate, mouse_res, mouse_status;
    int mouse_wrap;
    uint32_t mouse_buttons;
};

static int i8042_dbg(void);

static void kq_push(kqueue *q, uint8_t v)
{
    if (q->count >= KBD_QUEUE) return;
    q->data[(q->rpos + q->count) % KBD_QUEUE] = v;
    q->count++;
}

static void i8042_update(i8042 *k)
{
    if (!k->outbuf_full) {
        if (k->kq.count && !(k->mode & 0x10)) {
            k->outbuf = k->kq.data[k->kq.rpos];
            k->kq.rpos = (k->kq.rpos + 1) % KBD_QUEUE;
            k->kq.count--;
            k->outbuf_full = true;
            k->outbuf_aux = false;
        } else if (k->mq.count && !(k->mode & 0x20)) {
            k->outbuf = k->mq.data[k->mq.rpos];
            k->mq.rpos = (k->mq.rpos + 1) % KBD_QUEUE;
            k->mq.count--;
            k->outbuf_full = true;
            k->outbuf_aux = true;
        }
    }
    k->status = (uint8_t)((k->status & ~0x21u) | (k->outbuf_full ? 1 : 0) | (k->outbuf_full && k->outbuf_aux ? 0x20 : 0));
    irq_set(&k->kirq, k->outbuf_full && !k->outbuf_aux && (k->mode & 1));
    irq_set(&k->mirq, k->outbuf_full && k->outbuf_aux && (k->mode & 2));
}

i8042 *i8042_new(mvm_vm *vm, irq_line kbd_irq, irq_line mouse_irq)
{
    i8042 *k = calloc(1, sizeof(*k));
    k->vm = vm;
    k->kirq = kbd_irq;
    k->mirq = mouse_irq;
    k->mode = 0x61; /* IRQ teclado, sistema, traducao */
    k->status = 0x18;
    k->out_port = 0x03;
    k->kbd_enabled = true;
    k->scancode_set = 2;
    k->mouse_rate = 100;
    k->mouse_res = 2;
    return k;
}

static void kbd_write(i8042 *k, uint8_t val)
{
    uint8_t cmd = k->kbd_cmd;
    k->kbd_cmd = 0;
    switch (cmd) {
    case 0xed: kq_push(&k->kq, 0xfa); return;              /* LEDs */
    case 0xf3: kq_push(&k->kq, 0xfa); return;              /* taxa de repeticao */
    case 0xf0:
        kq_push(&k->kq, 0xfa);
        if (val == 0) kq_push(&k->kq, k->scancode_set);
        else k->scancode_set = val;
        return;
    default: break;
    }
    switch (val) {
    case 0x00: kq_push(&k->kq, 0xfa); break;
    case 0x05: kq_push(&k->kq, 0xfe); break;
    case 0xed: case 0xf3: case 0xf0: kq_push(&k->kq, 0xfa); k->kbd_cmd = val; break;
    case 0xee: kq_push(&k->kq, 0xee); break;
    case 0xf2: kq_push(&k->kq, 0xfa); kq_push(&k->kq, 0xab); kq_push(&k->kq, 0x83); break;
    case 0xf4: k->kbd_enabled = true; kq_push(&k->kq, 0xfa); break;
    case 0xf5: k->kbd_enabled = false; kq_push(&k->kq, 0xfa); break;
    case 0xf6: k->kbd_enabled = true; kq_push(&k->kq, 0xfa); break;
    case 0xff: kq_push(&k->kq, 0xfa); kq_push(&k->kq, 0xaa); k->kbd_enabled = true; break;
    default: kq_push(&k->kq, 0xfa); break;
    }
}

static void mouse_write(i8042 *k, uint8_t val)
{
    uint8_t cmd = k->mouse_cmd;
    k->mouse_cmd = 0;
    if (cmd == 0xf3) { k->mouse_rate = val; kq_push(&k->mq, 0xfa); return; }
    if (cmd == 0xe8) { k->mouse_res = val; kq_push(&k->mq, 0xfa); return; }
    switch (val) {
    case 0xe6: case 0xe7: kq_push(&k->mq, 0xfa); break;
    case 0xe8: case 0xf3: kq_push(&k->mq, 0xfa); k->mouse_cmd = val; break;
    case 0xe9:
        kq_push(&k->mq, 0xfa);
        kq_push(&k->mq, k->mouse_enabled ? 0x20 : 0);
        kq_push(&k->mq, k->mouse_res);
        kq_push(&k->mq, k->mouse_rate);
        break;
    case 0xf2: kq_push(&k->mq, 0xfa); kq_push(&k->mq, 0x00); break;
    case 0xf4: k->mouse_enabled = true; kq_push(&k->mq, 0xfa); break;
    case 0xf5: k->mouse_enabled = false; kq_push(&k->mq, 0xfa); break;
    case 0xf6: kq_push(&k->mq, 0xfa); break;
    case 0xff:
        k->mouse_enabled = false;
        kq_push(&k->mq, 0xfa);
        kq_push(&k->mq, 0xaa);
        kq_push(&k->mq, 0x00);
        break;
    default: kq_push(&k->mq, 0xfa); break;
    }
}

static uint64_t i8042_read(void *opaque, uint64_t off, unsigned size)
{
    i8042 *k = opaque;
    (void)size;
    if (off == 4)
        return k->status;
    uint8_t v = k->outbuf;
    if (i8042_dbg() > 1)
        LOGI("i8042: leitura 60 = %02x (cheio=%d aux=%d)", v, k->outbuf_full, k->outbuf_aux);
    k->outbuf_full = false;
    /* a leitura derruba a IRQ; se houver outro byte na fila ela sobe de novo,
     * gerando uma nova borda para o 8259 */
    irq_set(&k->kirq, 0);
    irq_set(&k->mirq, 0);
    i8042_update(k);
    return v;
}

static void out_ctrl(i8042 *k, uint8_t v)
{
    k->outbuf = v;
    k->outbuf_full = true;
    k->outbuf_aux = false;
    i8042_update(k);
}

static int i8042_dbg(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MVM_I8042_DEBUG") ? (atoi(getenv("MVM_I8042_DEBUG")) > 0 ? atoi(getenv("MVM_I8042_DEBUG")) : 1) : 0;
    return on;
}

static void i8042_write(void *opaque, uint64_t off, uint64_t v, unsigned size)
{
    i8042 *k = opaque;
    uint8_t val = (uint8_t)v;
    (void)size;
    if (i8042_dbg())
        LOGI("i8042: %s %02x (pendente=%02x modo=%02x mouse=%d)", off == 4 ? "cmd" : "dado", val, k->cmd, k->mode,
             k->mouse_enabled);
    if (off == 4) {
        k->status |= 0x08; /* ultimo acesso: comando */
        switch (val) {
        case 0x20: out_ctrl(k, k->mode); return;
        case 0x60: case 0xd1: case 0xd2: case 0xd3: case 0xd4: k->cmd = val; return;
        case 0xa7: k->mode |= 0x20; i8042_update(k); return;
        case 0xa8: k->mode &= ~0x20u; i8042_update(k); return;
        case 0xa9: out_ctrl(k, 0x00); return;
        case 0xaa: k->status |= 0x04; out_ctrl(k, 0x55); return;
        case 0xab: out_ctrl(k, 0x00); return;
        case 0xad: k->mode |= 0x10; i8042_update(k); return;
        case 0xae: k->mode &= ~0x10u; i8042_update(k); return;
        case 0xc0: out_ctrl(k, 0x80); return;
        case 0xd0: out_ctrl(k, k->out_port); return;
        case 0xe0: out_ctrl(k, 0x00); return;
        default:
            if ((val & 0xf0) == 0xf0 && !(val & 1)) { /* pulso de reset */
                LOGI("i8042: reset do sistema");
                vm_request_guest_reset(k->vm);
            }
            return;
        }
    }
    k->status &= ~0x08u;
    uint8_t cmd = k->cmd;
    k->cmd = 0;
    switch (cmd) {
    case 0x60:
        k->mode = val;
        i8042_update(k);
        return;
    case 0xd1:
        k->out_port = val;
        if (!(val & 1)) vm_request_guest_reset(k->vm);
        return;
    case 0xd2: kq_push(&k->kq, val); i8042_update(k); return;
    case 0xd3: kq_push(&k->mq, val); i8042_update(k); return;
    case 0xd4:
        /* escrever no dispositivo auxiliar liga o clock da porta auxiliar (como nos
         * 8042 reais): drivers como o i8042prt do Windows/ReactOS detectam o mouse
         * com a porta desabilitada no byte de comando e esperam a resposta */
        k->mode &= ~0x20u;
        mouse_write(k, val);
        i8042_update(k);
        return;
    default:
        kbd_write(k, val);
        i8042_update(k);
        return;
    }
}

const mvm_io_ops i8042_ops = {i8042_read, i8042_write};

/* codigos evdev -> scancode set 1 (com prefixo 0xE0 marcado em 0x100) */
static uint16_t evdev_to_set1(uint16_t code)
{
    if (code >= 1 && code <= 88 && code != 84 && code != 85 && code != 86)
        return code;
    switch (code) {
    case 86: return 0x56;
    case 96: return 0x11c;  /* KP Enter */
    case 97: return 0x11d;  /* Ctrl dir */
    case 98: return 0x135;  /* KP / */
    case 99: return 0x137;  /* SysRq */
    case 100: return 0x138; /* Alt dir */
    case 102: return 0x147; /* Home */
    case 103: return 0x148; /* Cima */
    case 104: return 0x149; /* PgUp */
    case 105: return 0x14b; /* Esquerda */
    case 106: return 0x14d; /* Direita */
    case 107: return 0x14f; /* End */
    case 108: return 0x150; /* Baixo */
    case 109: return 0x151; /* PgDn */
    case 110: return 0x152; /* Insert */
    case 111: return 0x153; /* Delete */
    case 119: return 0x145; /* Pause (simplificado) */
    case 125: return 0x15b; /* Meta esq */
    case 126: return 0x15c; /* Meta dir */
    case 127: return 0x15d; /* Menu */
    default: return 0;
    }
}

void i8042_key(i8042 *k, uint16_t code, bool pressed)
{
    if (i8042_dbg())
        LOGI("i8042: tecla %u %s (habilitado=%d modo=%02x fila=%u status=%02x outbuf=%02x aux=%d)", code,
             pressed ? "down" : "up", k->kbd_enabled, k->mode, k->kq.count, k->status, k->outbuf, k->outbuf_aux);
    if (!k->kbd_enabled)
        return;
    uint16_t sc = evdev_to_set1(code);
    if (!sc)
        return;
    if (sc & 0x100)
        kq_push(&k->kq, 0xe0);
    kq_push(&k->kq, (uint8_t)((sc & 0x7f) | (pressed ? 0 : 0x80)));
    i8042_update(k);
}

void i8042_mouse(i8042 *k, int dx, int dy, int dz, uint32_t buttons)
{
    (void)dz;
    if (i8042_dbg())
        LOGI("i8042: evento de mouse %d,%d b=%u (habilitado=%d modo=%02x fila=%u status=%02x)", dx, dy, buttons,
             k->mouse_enabled, k->mode, k->mq.count, k->status);
    if (!k->mouse_enabled)
        return;
    while (dx || dy || buttons != k->mouse_buttons) {
        int x = dx > 127 ? 127 : dx < -127 ? -127 : dx;
        int y = dy > 127 ? 127 : dy < -127 ? -127 : dy;
        int py = -y; /* PS/2: Y positivo para cima */
        uint8_t b0 = (uint8_t)(0x08 | (buttons & 7) | (x < 0 ? 0x10 : 0) | (py < 0 ? 0x20 : 0));
        kq_push(&k->mq, b0);
        kq_push(&k->mq, (uint8_t)x);
        kq_push(&k->mq, (uint8_t)py);
        dx -= x;
        dy -= y;
        k->mouse_buttons = buttons;
    }
    i8042_update(k);
}
