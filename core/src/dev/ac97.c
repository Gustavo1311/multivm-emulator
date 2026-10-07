/*
 * Intel 82801AA AC'97 (PCI 8086:2415) com codec SigmaTel STAC9700: placa de som
 * com driver nativo no Windows XP, no ReactOS e no Linux (snd-intel8x0).
 *
 * BAR0 (NAM): registradores do mixer/codec. BAR1 (NABM): bus master com tres
 * canais (PCM in, PCM out e microfone), cada um com uma lista de 32 descritores
 * (BDL) na RAM. O DMA anda no ritmo do relogio virtual (um temporizador a cada
 * TICK_NS) e o audio vai/vem da camada de audio do host (audio.c).
 */
#include "devices.h"

#include <math.h>
#include <stdlib.h>

#define TICK_NS 5000000LL /* 5 ms */

enum { CH_PI, CH_PO, CH_MC, NCH };

/* registradores de cada canal (NABM) */
#define BDBAR 0x00
#define CIV 0x04
#define LVI 0x05
#define SR 0x06
#define PICB 0x08
#define PIV 0x0a
#define CR 0x0b

#define SR_DCH 0x01
#define SR_CELV 0x02
#define SR_LVBCI 0x04
#define SR_BCIS 0x08
#define SR_FIFOE 0x10
#define SR_WCLEAR (SR_LVBCI | SR_BCIS | SR_FIFOE)

#define CR_RPBM 0x01
#define CR_RR 0x02
#define CR_LVBIE 0x04
#define CR_IOCE 0x10
#define CR_FEIE 0x08

#define GLOB_CNT 0x2c
#define GLOB_STA 0x30
#define CAS 0x34

#define GS_PIINT (1u << 5)
#define GS_POINT (1u << 6)
#define GS_MINT (1u << 7)
#define GS_PCR (1u << 8)

#define BD_IOC 0x8000

typedef struct {
    uint32_t bdbar;
    uint8_t civ, lvi, piv, cr;
    uint16_t sr, picb;
    uint32_t addr; /* endereco atual dentro do buffer */
    bool fed;      /* descritor atual carregado */
    audio_resampler rs;
    int64_t frac;  /* quadros fracionarios acumulados (x 1e9) */
} ac97_ch;

struct ac97 {
    mvm_vm *vm;
    mvm_audio *audio;
    irq_line irq;
    uint16_t mixer[64];
    ac97_ch ch[NCH];
    uint32_t glob_cnt;
    uint8_t cas;
    mvm_timer timer;
    bool timer_on;
    int64_t last;
    uint8_t buf[8192];
};

static const uint16_t mixer_default[64] = {
    [0x00 / 2] = 0x0000, [0x02 / 2] = 0x8000, [0x04 / 2] = 0x8000, [0x06 / 2] = 0x8000,
    [0x0a / 2] = 0x0000, [0x0c / 2] = 0x8008, [0x0e / 2] = 0x8008, [0x10 / 2] = 0x8808,
    [0x12 / 2] = 0x8808, [0x14 / 2] = 0x8808, [0x16 / 2] = 0x8808, [0x18 / 2] = 0x8808,
    [0x1a / 2] = 0x0000, [0x1c / 2] = 0x8000, [0x1e / 2] = 0x8000, [0x20 / 2] = 0x0000,
    [0x22 / 2] = 0x0000, [0x26 / 2] = 0x000f, [0x28 / 2] = 0x0001, /* VRA */
    [0x2a / 2] = 0x0000, [0x2c / 2] = 48000, [0x2e / 2] = 48000, [0x30 / 2] = 48000,
    [0x32 / 2] = 48000, [0x34 / 2] = 48000,
    [0x7c / 2] = 0x8384, [0x7e / 2] = 0x7600, /* SigmaTel STAC9700 */
};

/* MVM_SND_DEBUG=1: registra o bus master e as interrupcoes */
static int snd_debug = -1, snd_debug_n;
#define SNDDBG(...) do { \
    if (snd_debug < 0) { const char *e_ = getenv("MVM_SND_DEBUG"); snd_debug = e_ && atoi(e_) > 0; } \
    if (snd_debug && snd_debug_n++ < 20000) LOGI("ac97: " __VA_ARGS__); } while (0)

static void update_irq(ac97 *a)
{
    bool lvl = false;
    for (int i = 0; i < NCH; i++) {
        ac97_ch *c = &a->ch[i];
        if (((c->sr & SR_BCIS) && (c->cr & CR_IOCE)) || ((c->sr & SR_LVBCI) && (c->cr & CR_LVBIE)) ||
            ((c->sr & SR_FIFOE) && (c->cr & CR_FEIE)))
            lvl = true;
    }
    static bool last;
    if (lvl != last)
        SNDDBG("irq %d", lvl);
    last = lvl;
    irq_set(&a->irq, lvl);
}

static uint32_t glob_sta(ac97 *a)
{
    uint32_t v = GS_PCR;
    static const uint32_t bit[NCH] = {GS_PIINT, GS_POINT, GS_MINT};
    for (int i = 0; i < NCH; i++)
        if (a->ch[i].sr & (SR_BCIS | SR_LVBCI | SR_FIFOE))
            v |= bit[i];
    return v;
}

/* ganho em 1/256: atenuacao de 1,5 dB por passo; bit 15 = mudo */
static int gain(uint16_t reg, bool left, int bits)
{
    if (reg & 0x8000)
        return 0;
    int att = left ? (reg >> 8) & ((1 << bits) - 1) : reg & ((1 << bits) - 1);
    return (int)(256.0 * pow(10.0, -1.5 * att / 20.0));
}

static void reset_ch(ac97 *a, int i)
{
    ac97_ch *c = &a->ch[i];
    memset(c, 0, sizeof(*c));
    c->sr = SR_DCH;
    (void)a;
}

static void fetch_bd(ac97 *a, ac97_ch *c)
{
    uint64_t e = (c->bdbar & ~7u) + 8u * (c->civ & 31);
    c->addr = (uint32_t)space_read(&a->vm->mem, e, 4) & ~1u;
    c->picb = (uint16_t)space_read(&a->vm->mem, e + 4, 2);
    c->piv = (uint8_t)((c->civ + 1) & 31);
    c->fed = true;
}

static uint16_t bd_flags(ac97 *a, ac97_ch *c)
{
    uint64_t e = (c->bdbar & ~7u) + 8u * (c->civ & 31);
    return (uint16_t)space_read(&a->vm->mem, e + 6, 2);
}

static void channels_changed(ac97 *a);

/* fim do buffer atual: interrupcao e proximo descritor (ou para no ultimo valido) */
static void buffer_done(ac97 *a, ac97_ch *c)
{
    SNDDBG("fim do buffer civ=%u lvi=%u ioc=%d", c->civ, c->lvi, !!(bd_flags(a, c) & BD_IOC));
    if (bd_flags(a, c) & BD_IOC)
        c->sr |= SR_BCIS;
    if (c->civ == c->lvi) {
        c->sr |= SR_LVBCI | SR_DCH | SR_CELV;
        c->fed = false;
        return;
    }
    c->civ = (uint8_t)((c->civ + 1) & 31);
    fetch_bd(a, c);
}

/* move ate 'frames' quadros no canal; devolve quantos */
static void run_channel(ac97 *a, int i, size_t frames)
{
    ac97_ch *c = &a->ch[i];
    int nch = i == CH_MC ? 1 : 2;
    size_t fbytes = (size_t)nch * 2;
    uint32_t rate = i == CH_PO ? a->mixer[0x2c / 2] : i == CH_PI ? a->mixer[0x32 / 2] : a->mixer[0x34 / 2];
    if (!(a->mixer[0x2a / 2] & 1))
        rate = 48000; /* sem VRA */
    while (frames && !(c->sr & SR_DCH)) {
        if (!c->fed)
            fetch_bd(a, c);
        if (!c->picb) { /* descritor vazio */
            buffer_done(a, c);
            continue;
        }
        size_t avail = (size_t)c->picb * 2 / fbytes; /* PICB conta amostras de 16 bits */
        size_t n = frames < avail ? frames : avail;
        if (n * fbytes > sizeof(a->buf))
            n = sizeof(a->buf) / fbytes;
        if (!n) {
            c->picb = 0;
            continue;
        }
        size_t bytes = n * fbytes;
        if (i == CH_PO) {
            space_memread(&a->vm->mem, c->addr, a->buf, bytes);
            uint16_t master = a->mixer[0x02 / 2], pcm = a->mixer[0x18 / 2];
            int gl = gain(master, true, 6) * gain(pcm, true, 5) / 256;
            int gr = gain(master, false, 6) * gain(pcm, false, 5) / 256;
            audio_out_write(a->audio, &c->rs, a->buf, n, 2, 16, rate, gl, gr);
        } else {
            audio_in_read(a->audio, &c->rs, a->buf, n, nch, 16, rate);
            space_memwrite(&a->vm->mem, c->addr, a->buf, bytes);
        }
        c->addr += (uint32_t)bytes;
        c->picb = (uint16_t)(c->picb - bytes / 2);
        frames -= n;
        if (!c->picb)
            buffer_done(a, c);
    }
}

static void tick(void *opaque)
{
    ac97 *a = opaque;
    int64_t now = mvm_now(a->vm);
    int64_t dt = now - a->last;
    if (dt > 100000000LL)
        dt = TICK_NS; /* pausa longa: nao tenta recuperar */
    a->last = now;
    for (int i = 0; i < NCH; i++) {
        ac97_ch *c = &a->ch[i];
        if (!(c->cr & CR_RPBM) || (c->sr & SR_DCH))
            continue;
        uint32_t rate = i == CH_PO ? a->mixer[0x2c / 2] : i == CH_PI ? a->mixer[0x32 / 2] : a->mixer[0x34 / 2];
        if (!(a->mixer[0x2a / 2] & 1) || !rate)
            rate = 48000;
        c->frac += dt * (int64_t)rate;
        size_t frames = (size_t)(c->frac / 1000000000LL);
        c->frac -= (int64_t)frames * 1000000000LL;
        run_channel(a, i, frames);
    }
    update_irq(a);
    channels_changed(a);
}

/* liga/desliga o temporizador e os streams do host conforme os canais rodam */
static void channels_changed(ac97 *a)
{
    bool po = (a->ch[CH_PO].cr & CR_RPBM) && !(a->ch[CH_PO].sr & SR_DCH);
    bool in = ((a->ch[CH_PI].cr & CR_RPBM) && !(a->ch[CH_PI].sr & SR_DCH)) ||
              ((a->ch[CH_MC].cr & CR_RPBM) && !(a->ch[CH_MC].sr & SR_DCH));
    audio_out_active(a->audio, po);
    audio_in_active(a->audio, in);
    if (po || in) {
        if (!a->timer_on)
            a->last = mvm_now(a->vm);
        a->timer_on = true;
        timer_mod(a->vm, &a->timer, mvm_now(a->vm) + TICK_NS);
    } else if (a->timer_on) {
        a->timer_on = false;
        timer_del(a->vm, &a->timer);
    }
}

/* ---------------------------------------------------------------- NAM (mixer) */

static uint64_t nam_read(void *opaque, uint64_t off, unsigned size)
{
    ac97 *a = opaque;
    a->cas = 0; /* o acesso ao codec termina na hora: libera o semaforo */
    if (off >= 128)
        return 0;
    uint16_t v = a->mixer[off / 2];
    if (size == 1)
        return (off & 1) ? v >> 8 : v & 0xff;
    if (size == 4)
        return v | ((uint32_t)a->mixer[(off / 2 + 1) & 63] << 16);
    return v;
}

static void nam_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    ac97 *a = opaque;
    a->cas = 0;
    if (off >= 128)
        return;
    unsigned r = (unsigned)off / 2;
    uint16_t v = (uint16_t)val;
    if (size == 1) {
        v = (off & 1) ? (uint16_t)((a->mixer[r] & 0xff) | (val << 8)) : (uint16_t)((a->mixer[r] & 0xff00) | (val & 0xff));
    }
    switch (r * 2) {
    case 0x00: /* reset do codec */
        memcpy(a->mixer, mixer_default, sizeof(a->mixer));
        return;
    case 0x26: /* powerdown: os bits de "pronto" ficam ligados */
        a->mixer[r] = (uint16_t)((v & 0xff00) | 0x000f);
        return;
    case 0x28: case 0x7c: case 0x7e: /* so leitura */
        return;
    case 0x2a:
        a->mixer[r] = v & 0x0001; /* so VRA */
        if (!(v & 1)) {
            a->mixer[0x2c / 2] = a->mixer[0x32 / 2] = a->mixer[0x34 / 2] = 48000;
        }
        return;
    case 0x2c: case 0x2e: case 0x30: case 0x32: case 0x34:
        if (a->mixer[0x2a / 2] & 1)
            a->mixer[r] = v < 8000 ? 8000 : v;
        return;
    default:
        a->mixer[r] = v;
        return;
    }
}

const mvm_io_ops ac97_nam_ops = {nam_read, nam_write};

/* ---------------------------------------------------------------- NABM (bus master) */

static uint64_t nabm_read_(void *opaque, uint64_t off, unsigned size);
static uint64_t nabm_read(void *opaque, uint64_t off, unsigned size)
{
    uint64_t v = nabm_read_(opaque, off, size);
    SNDDBG("le %02llx/%u = %llx", (unsigned long long)off, size, (unsigned long long)v);
    return v;
}

static uint64_t nabm_read_(void *opaque, uint64_t off, unsigned size)
{
    ac97 *a = opaque;
    if (off == GLOB_CNT)
        return a->glob_cnt;
    if (off == GLOB_STA)
        return glob_sta(a);
    if (off == CAS) {
        uint8_t v = a->cas;
        a->cas = 1;
        return v;
    }
    if (off >= 0x30)
        return 0;
    ac97_ch *c = &a->ch[off / 16];
    unsigned r = off % 16;
    uint64_t v;
    switch (r) {
    case BDBAR: v = c->bdbar; break;
    case CIV: v = c->civ | (uint32_t)c->lvi << 8 | (uint32_t)c->sr << 16; break; /* leitura de 32 bits: CIV,LVI,SR */
    case LVI: v = c->lvi; break;
    case SR: v = c->sr; break;
    case PICB: v = c->picb | (uint32_t)c->piv << 16 | (uint32_t)c->cr << 24; break;
    case PIV: v = c->piv; break;
    case CR: v = c->cr; break;
    default: v = 0; break;
    }
    if (size == 1)
        v &= 0xff;
    else if (size == 2)
        v &= 0xffff;
    return v;
}

static void write_cr(ac97 *a, int i, uint8_t v)
{
    ac97_ch *c = &a->ch[i];
    if (v & CR_RR) {
        reset_ch(a, i);
        update_irq(a);
        channels_changed(a);
        return;
    }
    bool was = c->cr & CR_RPBM;
    c->cr = v & (CR_RPBM | CR_LVBIE | CR_IOCE | CR_FEIE);
    if ((c->cr & CR_RPBM) && !was) {
        c->sr &= (uint16_t)~(SR_DCH | SR_CELV);
        c->fed = false;
        c->frac = 0;
    } else if (!(c->cr & CR_RPBM)) {
        c->sr |= SR_DCH;
    }
    update_irq(a);
    channels_changed(a);
}

static void nabm_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    ac97 *a = opaque;
    SNDDBG("escreve %02llx/%u = %llx", (unsigned long long)off, size, (unsigned long long)val);
    if (off == GLOB_CNT) {
        a->glob_cnt = (uint32_t)val;
        if (!(val & 2)) { /* reset frio */
            for (int i = 0; i < NCH; i++)
                reset_ch(a, i);
            memcpy(a->mixer, mixer_default, sizeof(a->mixer));
            update_irq(a);
            channels_changed(a);
        }
        return;
    }
    if (off == GLOB_STA || off == CAS) {
        if (off == CAS)
            a->cas = (uint8_t)val;
        return;
    }
    if (off >= 0x30)
        return;
    int i = (int)off / 16;
    ac97_ch *c = &a->ch[i];
    unsigned r = off % 16;
    switch (r) {
    case BDBAR:
        c->bdbar = (uint32_t)val & ~7u;
        break;
    case LVI:
        c->lvi = (uint8_t)(val & 31);
        /* buffer novo depois do ultimo: o canal parado no fim volta a andar */
        if ((c->cr & CR_RPBM) && (c->sr & SR_DCH) && (c->sr & SR_CELV) && c->civ != c->lvi) {
            c->civ = (uint8_t)((c->civ + 1) & 31);
            c->sr &= (uint16_t)~(SR_DCH | SR_CELV);
            fetch_bd(a, c);
            channels_changed(a);
        }
        break;
    case SR:
        c->sr &= (uint16_t)~(val & SR_WCLEAR);
        update_irq(a);
        break;
    case CR:
        write_cr(a, i, (uint8_t)val);
        break;
    case CIV: /* CIV e so leitura; escritas largas alcancam LVI (e SR) */
        if (size >= 2)
            nabm_write(a, off + 1, (val >> 8) & 0xff, 1);
        if (size == 4)
            nabm_write(a, off + 2, val >> 16, 2);
        break;
    case PICB: /* so leitura; escrita de 32 bits alcanca CR */
        if (size == 4)
            write_cr(a, i, (uint8_t)(val >> 24));
        break;
    default:
        break;
    }
}

const mvm_io_ops ac97_nabm_ops = {nabm_read, nabm_write};

/* ---------------------------------------------------------------- ciclo de vida */

void ac97_reset(ac97 *a)
{
    for (int i = 0; i < NCH; i++)
        reset_ch(a, i);
    memcpy(a->mixer, mixer_default, sizeof(a->mixer));
    a->glob_cnt = 0;
    a->cas = 0;
    update_irq(a);
    channels_changed(a);
}

ac97 *ac97_new(mvm_vm *vm, irq_line irq)
{
    ac97 *a = calloc(1, sizeof(*a));
    a->vm = vm;
    a->audio = vm->audio;
    a->irq = irq;
    timer_init(&a->timer, tick, a);
    ac97_reset(a);
    return a;
}

void ac97_free(ac97 *a)
{
    if (!a)
        return;
    timer_del(a->vm, &a->timer);
    free(a);
}
