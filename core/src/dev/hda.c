/*
 * Intel HD Audio (controlador ICH6, PCI 8086:2668) com um codec simples de duas
 * vias (como o "hda-duplex" do QEMU): DAC -> pino de saida de linha e pino de
 * microfone -> ADC. Driver nativo no Windows Vista/7/10/Server e no Linux
 * (snd-hda-intel).
 *
 * O driver fala com o codec pelos aneis CORB/RIRB (ou pelo comando imediato) e
 * move o audio por streams com lista de descritores (BDL) na RAM: 4 de entrada
 * e 4 de saida. O DMA anda no relogio virtual, como no AC'97.
 */
#include "devices.h"

#include <math.h>
#include <stdlib.h>

#define TICK_NS 5000000LL
#define NSTREAMS 8 /* 0-3 entrada, 4-7 saida */
#define ISS 4

/* registradores globais */
#define GCAP 0x00
#define VMIN 0x02
#define VMAJ 0x03
#define OUTPAY 0x04
#define INPAY 0x06
#define GCTL 0x08
#define WAKEEN 0x0c
#define STATESTS 0x0e
#define GSTS 0x10
#define INTCTL 0x20
#define INTSTS 0x24
#define WALCLK 0x30
#define SSYNC 0x38
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4a
#define CORBCTL 0x4c
#define CORBSTS 0x4d
#define CORBSIZE 0x4e
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5a
#define RIRBCTL 0x5c
#define RIRBSTS 0x5d
#define RIRBSIZE 0x5e
#define ICOI 0x60
#define ICII 0x64
#define ICIS 0x68
#define DPLBASE 0x70
#define DPUBASE 0x74
#define SD_BASE 0x80
#define SD_SIZE 0x20

/* registradores do stream (deslocamento dentro do descritor) */
#define SD_CTL 0x00
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0c
#define SD_FIFOS 0x10
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1c

#define CTL_SRST 0x01
#define CTL_RUN 0x02
#define CTL_IOCE 0x04
#define STS_BCIS 0x04
#define STS_FIFORDY 0x20

/* nos do codec */
enum { NID_ROOT, NID_AFG, NID_DAC, NID_OUT, NID_ADC, NID_MIC, NID_COUNT };

typedef struct {
    uint32_t ctl;     /* 24 bits */
    uint8_t sts;
    uint32_t lpib, cbl;
    uint16_t lvi, fmt;
    uint32_t bdpl, bdpu;
    /* posicao no BDL */
    int bd_index;
    uint32_t bd_off;
    audio_resampler rs;
    int64_t frac;
} hda_stream;

struct hda {
    mvm_vm *vm;
    mvm_audio *audio;
    irq_line irq;
    uint32_t gctl, intctl;
    uint16_t wakeen, statests;
    uint32_t corb_lo, corb_hi, rirb_lo, rirb_hi;
    uint16_t corbwp, corbrp, rirbwp, rintcnt;
    uint8_t corbctl, corbsts, rirbctl, rirbsts;
    uint32_t icoi, icii;
    uint16_t icis;
    uint32_t dpl_lo, dpl_hi;
    hda_stream sd[NSTREAMS];
    /* estado do codec */
    uint8_t dac_stream, adc_stream; /* stream (tag) / canal: bits 7:4 */
    uint16_t dac_fmt, adc_fmt;
    uint8_t dac_gain[2], adc_gain[2];
    bool dac_mute[2], adc_mute[2];
    uint8_t pin_ctl[NID_COUNT];
    uint8_t power[NID_COUNT];
    uint8_t unsol[NID_COUNT];
    mvm_timer timer;
    bool timer_on;
    int64_t last;
    uint8_t buf[16384];
    int16_t pcm[8192];
};

#define AMP_STEPS 0x4a

static uint64_t corb_base(hda *h) { return ((uint64_t)h->corb_hi << 32) | (h->corb_lo & ~0x7fu); }
static uint64_t rirb_base(hda *h) { return ((uint64_t)h->rirb_hi << 32) | (h->rirb_lo & ~0x7fu); }

/* ---------------------------------------------------------------- interrupcoes */

static uint32_t intsts(hda *h)
{
    uint32_t v = 0;
    for (int i = 0; i < NSTREAMS; i++)
        if ((h->sd[i].sts & STS_BCIS) && (h->sd[i].ctl & CTL_IOCE))
            v |= 1u << i;
    if ((h->rirbsts & 0x01) && (h->rirbctl & 0x01))
        v |= 1u << 30;
    if (h->statests & h->wakeen)
        v |= 1u << 30;
    if (v)
        v |= 1u << 31;
    return v;
}

static void update_irq(hda *h)
{
    uint32_t s = intsts(h);
    bool lvl = false;
    if (h->intctl & (1u << 31)) {
        if ((s & h->intctl & 0xff))
            lvl = true;
        if ((h->intctl & (1u << 30)) && (s & (1u << 30)))
            lvl = true;
    }
    irq_set(&h->irq, lvl);
}

/* ---------------------------------------------------------------- codec */

static uint32_t param(hda *h, int nid, int p)
{
    (void)h;
    switch (nid) {
    case NID_ROOT:
        if (p == 0x00) return 0x1af40022; /* id do fabricante/dispositivo */
        if (p == 0x02) return 0x00100101; /* revisao */
        if (p == 0x04) return (NID_AFG << 16) | 1;
        return 0;
    case NID_AFG:
        switch (p) {
        case 0x04: return (NID_DAC << 16) | (NID_COUNT - NID_DAC);
        case 0x05: return 0x00000001; /* grupo de funcao de audio */
        case 0x08: return 0x00000000;
        case 0x0a: return 0x00020060; /* 16 bits, 44,1 e 48 kHz */
        case 0x0b: return 0x00000001; /* PCM */
        case 0x0f: return 0x00000009; /* D0, D3 */
        case 0x11: return 0;
        case 0x12: case 0x0d: return (1u << 31) | (0x03u << 16) | (AMP_STEPS << 8) | AMP_STEPS;
        default: return 0;
        }
    case NID_DAC:
        switch (p) {
        case 0x09: return (0u << 20) | (1u << 10) | (1u << 4) | (1u << 3) | (1u << 2) | 1u; /* saida, estereo, amp de saida */
        case 0x0a: return 0x00020060;
        case 0x0b: return 0x00000001;
        case 0x12: return (1u << 31) | (0x03u << 16) | (AMP_STEPS << 8) | AMP_STEPS;
        case 0x0f: return 0x00000009;
        default: return 0;
        }
    case NID_ADC:
        switch (p) {
        case 0x09: return (1u << 20) | (1u << 10) | (1u << 8) | (1u << 4) | (1u << 3) | (1u << 1) | 1u; /* entrada, conexoes, amp de entrada */
        case 0x0a: return 0x00020060;
        case 0x0b: return 0x00000001;
        case 0x0d: return (1u << 31) | (0x03u << 16) | (AMP_STEPS << 8) | AMP_STEPS;
        case 0x0e: return 1; /* uma conexao */
        case 0x0f: return 0x00000009;
        default: return 0;
        }
    case NID_OUT:
        switch (p) {
        case 0x09: return (4u << 20) | (1u << 10) | (1u << 8) | 1u; /* pino, conexoes */
        case 0x0c: return (1u << 4) | (1u << 2); /* saida, detecao de presenca */
        case 0x0e: return 1;
        case 0x0f: return 0x00000009;
        default: return 0;
        }
    case NID_MIC:
        switch (p) {
        case 0x09: return (4u << 20) | (1u << 10) | 1u;
        case 0x0c: return (1u << 5) | (1u << 2); /* entrada, detecao de presenca */
        case 0x0f: return 0x00000009;
        default: return 0;
        }
    }
    return 0;
}

static uint32_t codec_verb(hda *h, uint32_t cmd)
{
    unsigned cad = cmd >> 28, nid = (cmd >> 20) & 0x7f;
    uint32_t verb = (cmd >> 8) & 0xfff, payload = cmd & 0xff;
    if (cad != 0 || nid >= NID_COUNT)
        return 0;
    /* verbos de 4 bits com carga de 16 */
    unsigned v4 = (cmd >> 16) & 0xf;
    uint16_t p16 = (uint16_t)cmd;
    if (v4 == 0x2) { /* SET_CONVERTER_FORMAT */
        if (nid == NID_DAC) h->dac_fmt = p16;
        if (nid == NID_ADC) h->adc_fmt = p16;
        return 0;
    }
    if (v4 == 0xa) /* GET_CONVERTER_FORMAT */
        return nid == NID_DAC ? h->dac_fmt : nid == NID_ADC ? h->adc_fmt : 0;
    if (v4 == 0x3) { /* SET_AMP_GAIN_MUTE */
        bool out = p16 & 0x8000, in = p16 & 0x4000, l = p16 & 0x2000, r = p16 & 0x1000;
        uint8_t g = p16 & 0x7f;
        bool mute = p16 & 0x80;
        if (nid == NID_DAC && out) {
            if (l) { h->dac_gain[0] = g; h->dac_mute[0] = mute; }
            if (r) { h->dac_gain[1] = g; h->dac_mute[1] = mute; }
        }
        if (nid == NID_ADC && in) {
            if (l) { h->adc_gain[0] = g; h->adc_mute[0] = mute; }
            if (r) { h->adc_gain[1] = g; h->adc_mute[1] = mute; }
        }
        return 0;
    }
    if (v4 == 0xb) { /* GET_AMP_GAIN_MUTE */
        int ch = (p16 & 0x2000) ? 0 : 1;
        if (nid == NID_DAC && (p16 & 0x8000))
            return h->dac_gain[ch] | (h->dac_mute[ch] ? 0x80 : 0);
        if (nid == NID_ADC && !(p16 & 0x8000))
            return h->adc_gain[ch] | (h->adc_mute[ch] ? 0x80 : 0);
        return 0;
    }
    switch (verb) {
    case 0xf00: return param(h, (int)nid, (int)payload);
    case 0xf02: /* lista de conexoes */
        if (nid == NID_OUT) return NID_DAC;
        if (nid == NID_ADC) return NID_MIC;
        return 0;
    case 0xf01: return 0;
    case 0x701: return 0;
    case 0xf05: return (uint32_t)(h->power[nid] << 4) | h->power[nid];
    case 0x705: h->power[nid] = (uint8_t)(payload & 0xf); return 0;
    case 0xf06:
        return nid == NID_DAC ? h->dac_stream : nid == NID_ADC ? h->adc_stream : 0;
    case 0x706:
        if (nid == NID_DAC) h->dac_stream = (uint8_t)payload;
        if (nid == NID_ADC) h->adc_stream = (uint8_t)payload;
        return 0;
    case 0xf07: return h->pin_ctl[nid];
    case 0x707: h->pin_ctl[nid] = (uint8_t)payload; return 0;
    case 0xf08: return h->unsol[nid];
    case 0x708: h->unsol[nid] = (uint8_t)payload; return 0;
    case 0xf09: return (nid == NID_OUT || nid == NID_MIC) ? 0x80000000u : 0; /* algo conectado */
    case 0xf1c: /* configuracao padrao do pino */
        if (nid == NID_OUT) return 0x01014010; /* saida de linha, traseira, verde */
        if (nid == NID_MIC) return 0x01a19020; /* microfone, traseira, rosa */
        return 0;
    case 0xf20: return 0x1af41100; /* id do subsistema */
    case 0xf0c: case 0xf0d: case 0xf15: case 0xf16: case 0xf17: case 0xf19: return 0;
    case 0x7ff: /* reset do grupo de funcao */
        h->dac_stream = h->adc_stream = 0;
        return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- CORB/RIRB */

static void rirb_push(hda *h, uint32_t resp, uint32_t ext)
{
    h->rirbwp = (uint16_t)((h->rirbwp + 1) & 0xff);
    uint64_t a = rirb_base(h) + 8ULL * h->rirbwp;
    space_write(&h->vm->mem, a, resp, 4);
    space_write(&h->vm->mem, a + 4, ext, 4);
}

static void corb_run(hda *h)
{
    if (!(h->corbctl & 0x02) || !(h->rirbctl & 0x02))
        return;
    int n = 0;
    while ((h->corbrp & 0xff) != (h->corbwp & 0xff)) {
        h->corbrp = (uint16_t)((h->corbrp + 1) & 0xff);
        uint32_t cmd = (uint32_t)space_read(&h->vm->mem, corb_base(h) + 4ULL * h->corbrp, 4);
        rirb_push(h, codec_verb(h, cmd), cmd >> 28);
        n++;
    }
    if (n) {
        h->rirbsts |= 0x01; /* RINTFL */
        update_irq(h);
    }
}

/* ---------------------------------------------------------------- streams */

static void stream_format(uint16_t fmt, uint32_t *rate, int *bits, int *nch)
{
    uint32_t base = (fmt & 0x4000) ? 44100 : 48000;
    uint32_t mult = ((fmt >> 11) & 7) + 1, div = ((fmt >> 8) & 7) + 1;
    *rate = base * mult / div;
    static const int b[] = {8, 16, 20, 24, 32, 16, 16, 16};
    *bits = b[(fmt >> 4) & 7];
    *nch = (fmt & 0xf) + 1;
}

static int amp_gain(uint8_t g, bool mute)
{
    if (mute)
        return 0;
    if (g > AMP_STEPS)
        g = AMP_STEPS;
    return (int)(256.0 * pow(10.0, -0.75 * (AMP_STEPS - g) / 20.0));
}

/* move 'bytes' bytes do stream entre a RAM e buf (saida: le; entrada: escreve) */
static void stream_dma(hda *h, int i, uint8_t *buf, size_t bytes, bool out)
{
    hda_stream *s = &h->sd[i];
    uint64_t bdl = ((uint64_t)s->bdpu << 32) | (s->bdpl & ~0x7fu);
    size_t done = 0;
    for (int guard = 0; done < bytes && guard < 512; guard++) {
        uint64_t e = bdl + 16ULL * (uint64_t)s->bd_index;
        uint64_t addr = space_read(&h->vm->mem, e, 8);
        uint32_t len = (uint32_t)space_read(&h->vm->mem, e + 8, 4);
        uint32_t ioc = (uint32_t)space_read(&h->vm->mem, e + 12, 4) & 1;
        if (!len) { /* descritor vazio: pula */
            s->bd_index = s->bd_index >= s->lvi ? 0 : s->bd_index + 1;
            s->bd_off = 0;
            continue;
        }
        size_t n = len - s->bd_off;
        if (n > bytes - done)
            n = bytes - done;
        if (out)
            space_memread(&h->vm->mem, addr + s->bd_off, buf + done, n);
        else
            space_memwrite(&h->vm->mem, addr + s->bd_off, buf + done, n);
        done += n;
        s->bd_off += (uint32_t)n;
        s->lpib += (uint32_t)n;
        if (s->cbl && s->lpib >= s->cbl)
            s->lpib -= s->cbl;
        if (s->bd_off >= len) {
            if (ioc)
                s->sts |= STS_BCIS;
            s->bd_off = 0;
            s->bd_index = s->bd_index >= s->lvi ? 0 : s->bd_index + 1;
        }
    }
}

/* PCM do convidado (8/16/20/24/32 bits) para s16 intercalado */
static size_t to_s16(const uint8_t *src, size_t frames, int bits, int nch, int16_t *dst)
{
    int bps = bits == 8 ? 1 : bits == 16 ? 2 : 4;
    for (size_t f = 0; f < frames; f++)
        for (int c = 0; c < nch; c++) {
            const uint8_t *p = src + (f * (size_t)nch + (size_t)c) * (size_t)bps;
            int16_t v;
            if (bps == 1)
                v = (int16_t)(((int)p[0] - 128) << 8);
            else if (bps == 2)
                v = (int16_t)(p[0] | p[1] << 8);
            else /* 20/24/32 bits num container de 32: alinhados a esquerda no HDA */
                v = (int16_t)(p[2] | p[3] << 8);
            dst[f * (size_t)nch + (size_t)c] = v;
        }
    return frames;
}

static void run_stream(hda *h, int i, int64_t dt)
{
    hda_stream *s = &h->sd[i];
    bool out = i >= ISS;
    uint32_t rate;
    int bits, nch;
    stream_format(s->fmt, &rate, &bits, &nch);
    int bps = bits == 8 ? 1 : bits == 16 ? 2 : 4;
    size_t fbytes = (size_t)(bps * nch);
    s->frac += dt * (int64_t)rate;
    size_t frames = (size_t)(s->frac / 1000000000LL);
    s->frac -= (int64_t)frames * 1000000000LL;
    unsigned tag = (s->ctl >> 20) & 0xf;
    while (frames) {
        size_t n = frames;
        if (n * fbytes > sizeof(h->buf))
            n = sizeof(h->buf) / fbytes;
        if (n * (size_t)nch > ARRAY_SIZE(h->pcm))
            n = ARRAY_SIZE(h->pcm) / (size_t)nch;
        if (out) {
            stream_dma(h, i, h->buf, n * fbytes, true);
            if (tag && (h->dac_stream >> 4) == tag && nch <= 2) {
                to_s16(h->buf, n, bits, nch, h->pcm);
                int gl = amp_gain(h->dac_gain[0], h->dac_mute[0]);
                int gr = amp_gain(h->dac_gain[1], h->dac_mute[1]);
                audio_out_write(h->audio, &s->rs, h->pcm, n, nch, 16, rate, gl, gr);
            }
        } else {
            memset(h->buf, 0, n * fbytes);
            if (tag && (h->adc_stream >> 4) == tag && bps == 2 && nch <= 2)
                audio_in_read(h->audio, &s->rs, h->buf, n, nch, 16, rate);
            stream_dma(h, i, h->buf, n * fbytes, false);
        }
        frames -= n;
    }
}

static void update_dpl(hda *h)
{
    if (!(h->dpl_lo & 1))
        return;
    uint64_t base = ((uint64_t)h->dpl_hi << 32) | (h->dpl_lo & ~0x7fu);
    for (int i = 0; i < NSTREAMS; i++)
        space_write(&h->vm->mem, base + 8ULL * (uint64_t)i, h->sd[i].lpib, 4);
}

static void streams_changed(hda *h);

static void tick(void *opaque)
{
    hda *h = opaque;
    int64_t now = mvm_now(h->vm);
    int64_t dt = now - h->last;
    if (dt > 100000000LL)
        dt = TICK_NS;
    h->last = now;
    for (int i = 0; i < NSTREAMS; i++)
        if (h->sd[i].ctl & CTL_RUN)
            run_stream(h, i, dt);
    update_dpl(h);
    update_irq(h);
    streams_changed(h);
}

static void streams_changed(hda *h)
{
    bool out = false, in = false, any = false;
    for (int i = 0; i < NSTREAMS; i++) {
        if (!(h->sd[i].ctl & CTL_RUN))
            continue;
        any = true;
        if (i >= ISS) out = true;
        else in = true;
    }
    audio_out_active(h->audio, out);
    audio_in_active(h->audio, in);
    if (any) {
        if (!h->timer_on)
            h->last = mvm_now(h->vm);
        h->timer_on = true;
        timer_mod(h->vm, &h->timer, mvm_now(h->vm) + TICK_NS);
    } else if (h->timer_on) {
        h->timer_on = false;
        timer_del(h->vm, &h->timer);
    }
}

static void stream_reset(hda_stream *s)
{
    memset(s, 0, sizeof(*s));
    s->sts = STS_FIFORDY;
}

/* ---------------------------------------------------------------- registradores */

/* leitura por dword alinhado (os registradores de 8/16 bits sao recortados em mmio_read) */
static uint32_t sd_read(hda *h, int i, unsigned off)
{
    hda_stream *s = &h->sd[i];
    switch (off) {
    case 0x00: return s->ctl | ((uint32_t)s->sts << 24);
    case 0x04: return s->lpib;
    case 0x08: return s->cbl;
    case 0x0c: return s->lvi | (0x04u << 16); /* FIFOW */
    case 0x10: return 0xff | ((uint32_t)s->fmt << 16);
    case 0x18: return s->bdpl;
    case 0x1c: return s->bdpu;
    default: return 0;
    }
}

static void sd_write(hda *h, int i, unsigned off, uint32_t v, unsigned size)
{
    hda_stream *s = &h->sd[i];
    switch (off) {
    case SD_CTL: {
        uint32_t nv = size >= 3 ? (v & 0xffffff) : size == 2 ? ((s->ctl & 0xff0000) | (v & 0xffff)) : ((s->ctl & 0xffff00) | (v & 0xff));
        if (size == 4)
            s->sts &= (uint8_t)~((v >> 24) & 0x1c);
        if (nv & CTL_SRST) {
            stream_reset(s);
            s->ctl = CTL_SRST;
            break;
        }
        bool was = s->ctl & CTL_RUN;
        s->ctl = nv;
        if ((nv & CTL_RUN) && !was) {
            s->frac = 0;
        }
        break;
    }
    case 0x02: /* byte 2 de CTL (numero do stream) */
        s->ctl = (s->ctl & 0x00ffff) | ((v & 0xff) << 16);
        break;
    case SD_STS:
        s->sts &= (uint8_t)~(v & 0x1c);
        break;
    case SD_CBL: s->cbl = v; break;
    case SD_LVI: s->lvi = (uint16_t)(v & 0xff); break;
    case SD_FMT: s->fmt = (uint16_t)v; break;
    case SD_BDPL: s->bdpl = v & ~0x7fu; s->bd_index = 0; s->bd_off = 0; break;
    case SD_BDPU: s->bdpu = v; break;
    default: break;
    }
    update_irq(h);
    streams_changed(h);
}

static uint32_t reg_read(hda *h, unsigned off)
{
    if (off >= SD_BASE && off < SD_BASE + SD_SIZE * NSTREAMS)
        return sd_read(h, (int)(off - SD_BASE) / SD_SIZE, (off - SD_BASE) % SD_SIZE);
    switch (off) {
    case 0x00: return 0x4401 | (0x00u << 16) | (0x01u << 24); /* GCAP: 4 saidas, 4 entradas, 64 bits; HDA 1.0 */
    case 0x04: return 0x3c | (0x1du << 16);                   /* OUTPAY, INPAY */
    case GCTL: return h->gctl;
    case WAKEEN: return h->wakeen | ((uint32_t)h->statests << 16);
    case GSTS: return 0;
    case INTCTL: return h->intctl;
    case INTSTS: return intsts(h);
    case WALCLK: return (uint32_t)(mvm_now(h->vm) * 24 / 1000);
    case CORBLBASE: return h->corb_lo;
    case CORBUBASE: return h->corb_hi;
    case CORBWP: return h->corbwp | ((uint32_t)h->corbrp << 16);
    case CORBCTL: return h->corbctl | ((uint32_t)h->corbsts << 8) | (0x42u << 16);
    case RIRBLBASE: return h->rirb_lo;
    case RIRBUBASE: return h->rirb_hi;
    case RIRBWP: return h->rirbwp | ((uint32_t)h->rintcnt << 16);
    case RIRBCTL: return h->rirbctl | ((uint32_t)h->rirbsts << 8) | (0x42u << 16);
    case ICOI: return h->icoi;
    case ICII: return h->icii;
    case ICIS: return h->icis;
    case DPLBASE: return h->dpl_lo;
    case DPUBASE: return h->dpl_hi;
    default: return 0;
    }
}

static void reg_write(hda *h, unsigned off, uint32_t v, unsigned size)
{
    if (off >= SD_BASE && off < SD_BASE + SD_SIZE * NSTREAMS) {
        sd_write(h, (int)(off - SD_BASE) / SD_SIZE, (off - SD_BASE) % SD_SIZE, v, size);
        return;
    }
    switch (off) {
    case GCTL:
        if (!(v & 1)) { /* controlador em reset */
            h->gctl = 0;
            h->corbctl = h->rirbctl = 0;
            h->corbwp = h->corbrp = h->rirbwp = 0;
            h->intctl = 0;
            for (int i = 0; i < NSTREAMS; i++)
                stream_reset(&h->sd[i]);
            streams_changed(h);
        } else {
            if (!(h->gctl & 1))
                h->statests |= 1; /* codec 0 presente */
            h->gctl = v & 0x103;
        }
        break;
    case WAKEEN:
        h->wakeen = (uint16_t)v;
        if (size == 4)
            h->statests &= (uint16_t)~(v >> 16);
        break;
    case STATESTS: h->statests &= (uint16_t)~v; break;
    case INTCTL: h->intctl = v; break;
    case INTSTS: /* escrever 1 limpa os bits dos streams */
        for (int i = 0; i < NSTREAMS; i++)
            if (v & (1u << i))
                h->sd[i].sts &= (uint8_t)~STS_BCIS;
        break;
    case CORBLBASE: h->corb_lo = v; break;
    case CORBUBASE: h->corb_hi = v; break;
    case CORBWP:
        h->corbwp = (uint16_t)(v & 0xff);
        corb_run(h);
        break;
    case CORBRP:
        if (v & 0x8000)
            h->corbrp = 0x8000; /* reset: o driver espera ler o bit ligado */
        else
            h->corbrp = 0;
        break;
    case CORBCTL:
        h->corbctl = (uint8_t)v;
        if (size >= 2)
            h->corbsts &= (uint8_t)~(v >> 8);
        corb_run(h);
        break;
    case CORBSTS: h->corbsts &= (uint8_t)~v; break;
    case RIRBLBASE: h->rirb_lo = v; break;
    case RIRBUBASE: h->rirb_hi = v; break;
    case RIRBWP:
        if (v & 0x8000)
            h->rirbwp = 0;
        if (size == 4)
            h->rintcnt = (uint16_t)(v >> 16);
        break;
    case RINTCNT: h->rintcnt = (uint16_t)v; break;
    case RIRBCTL:
        h->rirbctl = (uint8_t)v;
        if (size >= 2)
            h->rirbsts &= (uint8_t)~(v >> 8);
        corb_run(h);
        break;
    case RIRBSTS: h->rirbsts &= (uint8_t)~v; break;
    case ICOI: h->icoi = v; break;
    case ICIS:
        if (v & 1) { /* comando imediato */
            h->icii = codec_verb(h, h->icoi);
            h->icis = 0x2; /* resposta valida */
        } else if (v & 2) {
            h->icis &= (uint16_t)~2u;
        }
        break;
    case DPLBASE: h->dpl_lo = v; break;
    case DPUBASE: h->dpl_hi = v; break;
    default: break;
    }
    update_irq(h);
}

static uint64_t mmio_read(void *opaque, uint64_t off, unsigned size)
{
    hda *h = opaque;
    /* registradores com largura variada: le o dword alinhado e recorta */
    uint32_t v = reg_read(h, (unsigned)off & ~3u) >> (8 * (off & 3));
    if (size == 1)
        return v & 0xff;
    if (size == 2)
        return v & 0xffff;
    return v;
}

static void mmio_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    reg_write(opaque, (unsigned)off, (uint32_t)val, size);
}

const mvm_io_ops hda_ops = {mmio_read, mmio_write};

void hda_reset(hda *h)
{
    h->gctl = 0;
    h->intctl = 0;
    h->wakeen = 0;
    h->statests = 0;
    h->corbctl = h->rirbctl = h->corbsts = h->rirbsts = 0;
    h->corbwp = h->corbrp = h->rirbwp = h->rintcnt = 0;
    h->icis = 0;
    h->dpl_lo = h->dpl_hi = 0;
    for (int i = 0; i < NSTREAMS; i++)
        stream_reset(&h->sd[i]);
    h->dac_stream = h->adc_stream = 0;
    h->dac_gain[0] = h->dac_gain[1] = AMP_STEPS;
    h->adc_gain[0] = h->adc_gain[1] = AMP_STEPS;
    memset(h->dac_mute, 0, sizeof(h->dac_mute));
    memset(h->adc_mute, 0, sizeof(h->adc_mute));
    streams_changed(h);
    update_irq(h);
}

hda *hda_new(mvm_vm *vm, irq_line irq)
{
    hda *h = calloc(1, sizeof(*h));
    h->vm = vm;
    h->audio = vm->audio;
    h->irq = irq;
    timer_init(&h->timer, tick, h);
    hda_reset(h);
    return h;
}

void hda_free(hda *h)
{
    if (!h)
        return;
    timer_del(h->vm, &h->timer);
    free(h);
}
