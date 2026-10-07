/*
 * Audio entre as placas de som emuladas e o host.
 *
 * Saida: a placa (thread da VM) entrega PCM no formato do convidado; aqui ele e
 * reamostrado para 48 kHz estereo s16 e vai para um anel SPSC sem trava que o
 * backend do host (AAudio no Android) esvazia no callback de audio. O anel e
 * mantido curto (~20 ms): se o convidado adianta, amostras sao descartadas; se
 * atrasa, o backend recebe silencio. Assim a latencia nao acumula.
 *
 * Entrada (microfone): o backend escreve 48 kHz estereo no anel de entrada e a
 * placa le reamostrado para a taxa do convidado (silencio se faltar).
 */
#include "internal.h"

#include <stdlib.h>

#define RING_FRAMES 8192            /* potencia de 2 (~170 ms a 48 kHz) */
#define OUT_TARGET (MVM_AUDIO_RATE / 50)   /* 20 ms */
#define OUT_MAX (MVM_AUDIO_RATE * 3 / 50)  /* 60 ms: acima disso descarta */
#define IN_MAX (MVM_AUDIO_RATE * 3 / 50)

typedef struct {
    int16_t buf[RING_FRAMES * 2];
    _Atomic uint32_t w, r;          /* contadores de quadros (livres, mod 2^32) */
} ring;

struct mvm_audio {
    mvm_vm *vm;
    mvm_audio_config cfg;
    ring out, in;
    const mvm_audio_backend *be;
    void *be_opaque;
    bool out_on, in_on;
    _Atomic int muted;
    _Atomic uint64_t underruns, overruns;
};

static uint32_t ring_fill(ring *r) { return atomic_load(&r->w) - atomic_load(&r->r); }

mvm_audio *audio_new(mvm_vm *vm, const mvm_audio_config *cfg)
{
    mvm_audio *a = calloc(1, sizeof(*a));
    a->vm = vm;
    a->cfg = *cfg;
    return a;
}

void audio_free(mvm_audio *a)
{
    if (!a)
        return;
    if (a->be) {
        if (a->out_on && a->be->stop)
            a->be->stop(a->be_opaque, false);
        if (a->in_on && a->be->stop)
            a->be->stop(a->be_opaque, true);
    }
    free(a);
}

/* liga/desliga os streams do host conforme a placa toca ou grava */
void audio_out_active(mvm_audio *a, bool on)
{
    if (!a || a->out_on == on)
        return;
    a->out_on = on;
    if (on) {
        atomic_store(&a->out.r, atomic_load(&a->out.w));
    }
    if (a->be && (on ? a->be->start : a->be->stop))
        (on ? a->be->start : a->be->stop)(a->be_opaque, false);
}

void audio_in_active(mvm_audio *a, bool on)
{
    if (!a || !a->cfg.mic || a->in_on == on)
        return;
    a->in_on = on;
    if (on)
        atomic_store(&a->in.r, atomic_load(&a->in.w));
    if (a->be && (on ? a->be->start : a->be->stop))
        (on ? a->be->start : a->be->stop)(a->be_opaque, true);
}

bool audio_has_mic(mvm_audio *a) { return a && a->cfg.mic; }

/* amostra i (canal ch) do PCM do convidado como s16 */
static inline int16_t sample_at(const uint8_t *pcm, size_t i, int ch, int nch, int bits)
{
    if (nch == 1)
        ch = 0;
    if (bits == 8)
        return (int16_t)(((int)pcm[i * (size_t)nch + (size_t)ch] - 128) << 8);
    const uint8_t *p = pcm + (i * (size_t)nch + (size_t)ch) * 2;
    return (int16_t)(p[0] | p[1] << 8);
}

static void push_frame(mvm_audio *a, int16_t l, int16_t r)
{
    ring *o = &a->out;
    uint32_t w = atomic_load_explicit(&o->w, memory_order_relaxed);
    o->buf[(w % RING_FRAMES) * 2] = l;
    o->buf[(w % RING_FRAMES) * 2 + 1] = r;
    atomic_store_explicit(&o->w, w + 1, memory_order_release);
}

void audio_out_write(mvm_audio *a, audio_resampler *rs, const void *data, size_t frames, int nch, int bits,
                     uint32_t rate, int vol_l, int vol_r)
{
    if (!a || !frames || !rate)
        return;
    const uint8_t *pcm = data;
    /* volume em 1/256; o mute do app zera */
    if (atomic_load(&a->muted))
        vol_l = vol_r = 0;
    if (a->be && a->be->push) { /* backend sincrono (gravador WAV do CLI) */
        int16_t tmp[512 * 2];
        size_t k = 0;
        uint64_t step = ((uint64_t)rate << 32) / MVM_AUDIO_RATE;
        for (;;) {
            size_t idx = (size_t)(rs->pos >> 32);
            if (idx >= frames)
                break;
            tmp[k * 2] = (int16_t)(sample_at(pcm, idx, 0, nch, bits) * vol_l / 256);
            tmp[k * 2 + 1] = (int16_t)(sample_at(pcm, idx, 1, nch, bits) * vol_r / 256);
            rs->pos += step;
            if (++k == 512) {
                a->be->push(a->be_opaque, tmp, k);
                k = 0;
            }
        }
        if (k)
            a->be->push(a->be_opaque, tmp, k);
        rs->pos -= (uint64_t)frames << 32;
        return;
    }
    /* anel cheio demais: a latencia passaria de OUT_MAX; descarta este bloco ate voltar ao alvo */
    uint32_t fill = ring_fill(&a->out);
    if (fill > OUT_MAX) {
        atomic_fetch_add(&a->overruns, 1);
        uint32_t r = atomic_load(&a->out.r);
        (void)r;
        rs->pos = 0;
        return;
    }
    /* reamostragem linear (posicao em ponto fixo 32.32 sobre os quadros do convidado) */
    uint64_t step = ((uint64_t)rate << 32) / MVM_AUDIO_RATE;
    for (;;) {
        size_t idx = (size_t)(rs->pos >> 32);
        if (idx >= frames)
            break;
        uint32_t frac = (uint32_t)(rs->pos >> 16) & 0xffff;
        int16_t l0 = idx ? sample_at(pcm, idx - 1, 0, nch, bits) : rs->last[0];
        int16_t r0 = idx ? sample_at(pcm, idx - 1, 1, nch, bits) : rs->last[1];
        int16_t l1 = sample_at(pcm, idx, 0, nch, bits);
        int16_t r1 = sample_at(pcm, idx, 1, nch, bits);
        int l = l0 + (int)(((int64_t)(l1 - l0) * frac) >> 16);
        int r = r0 + (int)(((int64_t)(r1 - r0) * frac) >> 16);
        push_frame(a, (int16_t)(l * vol_l / 256), (int16_t)(r * vol_r / 256));
        rs->pos += step;
    }
    rs->last[0] = sample_at(pcm, frames - 1, 0, nch, bits);
    rs->last[1] = sample_at(pcm, frames - 1, 1, nch, bits);
    rs->pos -= (uint64_t)frames << 32;
}

size_t audio_in_read(mvm_audio *a, audio_resampler *rs, void *data, size_t frames, int nch, int bits, uint32_t rate)
{
    uint8_t *pcm = data;
    ring *in = &a->in;
    uint32_t fill = ring_fill(in);
    /* microfone acumulado demais: pula para o mais recente */
    if (fill > IN_MAX)
        atomic_store(&in->r, atomic_load(&in->w) - OUT_TARGET);
    uint64_t step = ((uint64_t)MVM_AUDIO_RATE << 32) / rate;
    for (size_t i = 0; i < frames; i++) {
        uint32_t r = atomic_load_explicit(&in->r, memory_order_relaxed);
        uint32_t w = atomic_load_explicit(&in->w, memory_order_acquire);
        int16_t l = 0, rr = 0;
        if (r != w && a->in_on) {
            l = in->buf[(r % RING_FRAMES) * 2];
            rr = in->buf[(r % RING_FRAMES) * 2 + 1];
            rs->pos += step;
            uint32_t adv = (uint32_t)(rs->pos >> 32);
            rs->pos &= 0xffffffffu;
            if (adv > w - r)
                adv = w - r;
            atomic_store_explicit(&in->r, r + adv, memory_order_release);
        }
        for (int c = 0; c < nch; c++) {
            int16_t v = c == 0 ? l : rr;
            if (bits == 8)
                pcm[i * (size_t)nch + (size_t)c] = (uint8_t)((v >> 8) + 128);
            else {
                uint8_t *p = pcm + (i * (size_t)nch + (size_t)c) * 2;
                p[0] = (uint8_t)v;
                p[1] = (uint8_t)((uint16_t)v >> 8);
            }
        }
    }
    return frames;
}

/* ---------------------------------------------------------------- API publica */

void mvm_audio_set_backend(mvm_vm *vm, const mvm_audio_backend *be, void *opaque)
{
    mvm_audio *a = vm->audio;
    if (!a)
        return;
    a->be = be;
    a->be_opaque = opaque;
    if (be && a->out_on && be->start)
        be->start(opaque, false);
    if (be && a->in_on && be->start)
        be->start(opaque, true);
}

bool mvm_audio_enabled(mvm_vm *vm) { return vm->audio != NULL; }
bool mvm_audio_mic_enabled(mvm_vm *vm) { return vm->audio && vm->audio->cfg.mic; }

size_t mvm_audio_read(mvm_vm *vm, int16_t *buf, size_t frames)
{
    mvm_audio *a = vm->audio;
    if (!a) {
        memset(buf, 0, frames * 4);
        return 0;
    }
    ring *o = &a->out;
    uint32_t r = atomic_load_explicit(&o->r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&o->w, memory_order_acquire);
    uint32_t avail = w - r;
    if (avail > RING_FRAMES) { /* o produtor deu a volta: recomeca do mais recente */
        r = w - OUT_TARGET;
        avail = OUT_TARGET;
    }
    size_t n = avail < frames ? avail : frames;
    for (size_t i = 0; i < n; i++) {
        buf[i * 2] = o->buf[((r + i) % RING_FRAMES) * 2];
        buf[i * 2 + 1] = o->buf[((r + i) % RING_FRAMES) * 2 + 1];
    }
    if (n < frames) {
        memset(buf + n * 2, 0, (frames - n) * 4);
        if (a->out_on)
            atomic_fetch_add(&a->underruns, 1);
    }
    atomic_store_explicit(&o->r, r + (uint32_t)n, memory_order_release);
    return n;
}

void mvm_audio_write_input(mvm_vm *vm, const int16_t *buf, size_t frames)
{
    mvm_audio *a = vm->audio;
    if (!a || !a->cfg.mic)
        return;
    ring *in = &a->in;
    for (size_t i = 0; i < frames; i++) {
        uint32_t w = atomic_load_explicit(&in->w, memory_order_relaxed);
        if (w - atomic_load_explicit(&in->r, memory_order_acquire) >= RING_FRAMES - 1)
            break; /* cheio: a placa nao esta lendo */
        in->buf[(w % RING_FRAMES) * 2] = buf[i * 2];
        in->buf[(w % RING_FRAMES) * 2 + 1] = buf[i * 2 + 1];
        atomic_store_explicit(&in->w, w + 1, memory_order_release);
    }
}

void mvm_audio_set_muted(mvm_vm *vm, bool muted)
{
    if (vm->audio)
        atomic_store(&vm->audio->muted, muted ? 1 : 0);
}

void mvm_audio_stats(mvm_vm *vm, uint64_t *underruns, uint64_t *overruns)
{
    *underruns = vm->audio ? atomic_load(&vm->audio->underruns) : 0;
    *overruns = vm->audio ? atomic_load(&vm->audio->overruns) : 0;
}
