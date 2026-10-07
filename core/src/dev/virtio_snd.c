/*
 * virtio-sound (device id 25): um stream de saida e um de entrada (microfone),
 * PCM s16 de 8 a 48 kHz, 1 ou 2 canais. Usado na maquina ARM (virtio-mmio), com
 * o driver virtio_snd do Linux.
 *
 * Filas: 0 controle, 1 eventos (nao usada), 2 transmissao, 3 recepcao. Cada
 * buffer de PCM fica retido e so e devolvido quando o tempo de toca-lo (ou de
 * grava-lo) passou no relogio virtual; e isso que da o ritmo ao driver.
 */
#include "devices.h"

#include <stdlib.h>

#define TICK_NS 5000000LL
#define MAX_PENDING 64

#define S_OK 0x8000
#define S_BAD_MSG 0x8001
#define S_NOT_SUPP 0x8002

#define R_JACK_INFO 1
#define R_PCM_INFO 0x0100
#define R_PCM_SET_PARAMS 0x0101
#define R_PCM_PREPARE 0x0102
#define R_PCM_RELEASE 0x0103
#define R_PCM_START 0x0104
#define R_PCM_STOP 0x0105
#define R_CHMAP_INFO 0x0200

#define FMT_S16 5
static const uint32_t rate_table[] = {5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000};

typedef struct {
    virtq_elem *e;
    uint32_t done; /* bytes ja tocados/gravados */
    uint32_t len;  /* bytes de PCM */
} pending;

typedef struct {
    bool running;
    uint32_t rate;
    int nch;
    pending q[MAX_PENDING];
    int head, count;
    int64_t frac;
    audio_resampler rs;
} snd_stream;

typedef struct {
    virtio_dev *d;
    mvm_audio *audio;
    snd_stream st[2]; /* 0 = saida, 1 = entrada */
    mvm_timer timer;
    bool timer_on;
    int64_t last;
    uint8_t buf[16384];
} vsnd;

static uint32_t snd_cfg_read(virtio_dev *d, uint32_t off, unsigned size)
{
    (void)d;
    (void)size;
    switch (off) {
    case 0: return 0; /* jacks */
    case 4: return 2; /* streams */
    case 8: return 0; /* chmaps */
    default: return 0;
    }
}

static void streams_changed(vsnd *s);

/* bytes de PCM do pedido (sem o cabecalho e sem o status) */
static uint32_t pcm_len(const virtq_elem *e, bool tx)
{
    uint32_t r = 0, w = 0;
    for (int i = 0; i < e->nseg; i++) {
        if (e->seg[i].write) w += e->seg[i].len;
        else r += e->seg[i].len;
    }
    if (tx)
        return r > 4 ? r - 4 : 0; /* cabecalho xfer (4) | PCM ; status escrito */
    return w > 8 ? w - 8 : 0;     /* PCM | status (8) */
}

static void complete(vsnd *s, int q, pending *p, uint32_t status)
{
    virtio_dev *d = s->d;
    uint8_t st[8] = {(uint8_t)status, (uint8_t)(status >> 8), 0, 0, 0, 0, 0, 0};
    if (q == 2) {
        virtq_write(d, p->e, 0, st, 8);
        virtq_push(d, 2, p->e, 8);
    } else {
        virtq_write(d, p->e, p->len, st, 8);
        virtq_push(d, 3, p->e, p->len + 8);
    }
    free(p->e);
    p->e = NULL;
}

static void flush_stream(vsnd *s, int i)
{
    snd_stream *t = &s->st[i];
    while (t->count) {
        complete(s, i ? 3 : 2, &t->q[t->head], S_OK);
        t->head = (t->head + 1) % MAX_PENDING;
        t->count--;
    }
    virtio_notify_irq(s->d);
}

/* retira os pedidos de PCM de uma fila e guarda ate a hora de completa-los */
static void take_pcm(vsnd *s, int q)
{
    virtio_dev *d = s->d;
    snd_stream *t = &s->st[q == 2 ? 0 : 1];
    for (;;) {
        if (t->count == MAX_PENDING)
            return;
        virtq_elem *e = malloc(sizeof(*e));
        if (!virtq_pop(d, q, e)) {
            free(e);
            return;
        }
        pending *p = &t->q[(t->head + t->count) % MAX_PENDING];
        p->e = e;
        p->done = 0;
        p->len = pcm_len(e, q == 2);
        t->count++; /* fica retido ate ser tocado/gravado (o driver enche a fila antes do START) */
    }
}

static void run_stream(vsnd *s, int i, int64_t dt)
{
    snd_stream *t = &s->st[i];
    size_t fbytes = (size_t)t->nch * 2;
    t->frac += dt * (int64_t)t->rate;
    size_t frames = (size_t)(t->frac / 1000000000LL);
    t->frac -= (int64_t)frames * 1000000000LL;
    bool any = false;
    while (frames && t->count) {
        pending *p = &t->q[t->head];
        size_t avail = (p->len - p->done) / fbytes;
        size_t n = frames < avail ? frames : avail;
        if (n * fbytes > sizeof(s->buf))
            n = sizeof(s->buf) / fbytes;
        if (n) {
            size_t bytes = n * fbytes;
            if (i == 0) {
                virtq_read(s->d, p->e, 4 + p->done, s->buf, bytes);
                audio_out_write(s->audio, &t->rs, s->buf, n, t->nch, 16, t->rate, 256, 256);
            } else {
                audio_in_read(s->audio, &t->rs, s->buf, n, t->nch, 16, t->rate);
                virtq_write(s->d, p->e, p->done, s->buf, bytes);
            }
            p->done += (uint32_t)bytes;
            frames -= n;
        }
        if (p->done + fbytes > p->len) { /* buffer inteiro: devolve */
            complete(s, i ? 3 : 2, p, S_OK);
            t->head = (t->head + 1) % MAX_PENDING;
            t->count--;
            any = true;
        }
    }
    if (any)
        virtio_notify_irq(s->d);
}

static void tick(void *opaque)
{
    vsnd *s = opaque;
    int64_t now = mvm_now(s->d->vm);
    int64_t dt = now - s->last;
    if (dt > 100000000LL)
        dt = TICK_NS;
    s->last = now;
    take_pcm(s, 2);
    take_pcm(s, 3);
    for (int i = 0; i < 2; i++)
        if (s->st[i].running)
            run_stream(s, i, dt);
    streams_changed(s);
}

static void streams_changed(vsnd *s)
{
    audio_out_active(s->audio, s->st[0].running);
    audio_in_active(s->audio, s->st[1].running);
    bool any = s->st[0].running || s->st[1].running;
    if (any) {
        if (!s->timer_on)
            s->last = mvm_now(s->d->vm);
        s->timer_on = true;
        timer_mod(s->d->vm, &s->timer, mvm_now(s->d->vm) + TICK_NS);
    } else if (s->timer_on) {
        s->timer_on = false;
        timer_del(s->d->vm, &s->timer);
    }
}

/* ---------------------------------------------------------------- controle */

static uint32_t control(vsnd *s, const uint8_t *req, size_t len, uint8_t *resp, size_t *rlen)
{
    *rlen = 0;
    if (len < 4)
        return S_BAD_MSG;
    uint32_t code = (uint32_t)ld_le(req, 4);
    switch (code) {
    case R_PCM_INFO: {
        if (len < 16)
            return S_BAD_MSG;
        uint32_t start = (uint32_t)ld_le(req + 4, 4), count = (uint32_t)ld_le(req + 8, 4);
        uint32_t size = (uint32_t)ld_le(req + 12, 4);
        if (start + count > 2 || size < 32)
            return S_BAD_MSG;
        for (uint32_t i = 0; i < count; i++) {
            uint8_t *o = resp + i * size;
            memset(o, 0, size);
            uint64_t formats = 1ULL << FMT_S16;
            uint64_t rates = 0;
            for (int r = 1; r <= 7; r++) /* 8 kHz a 48 kHz */
                rates |= 1ULL << r;
            st_le(o + 8, formats, 8);
            st_le(o + 16, rates, 8);
            o[24] = (uint8_t)(start + i == 0 ? 0 : 1); /* direcao */
            o[25] = 1;                                  /* canais min */
            o[26] = 2;                                  /* canais max */
        }
        *rlen = count * size;
        return S_OK;
    }
    case R_PCM_SET_PARAMS: {
        if (len < 24)
            return S_BAD_MSG;
        uint32_t id = (uint32_t)ld_le(req + 4, 4);
        uint8_t ch = req[20], fmt = req[21], rate = req[22];
        if (id > 1 || fmt != FMT_S16 || ch < 1 || ch > 2 || rate >= ARRAY_SIZE(rate_table))
            return S_NOT_SUPP;
        s->st[id].nch = ch;
        s->st[id].rate = rate_table[rate];
        return S_OK;
    }
    case R_PCM_PREPARE:
    case R_PCM_RELEASE:
    case R_PCM_START:
    case R_PCM_STOP: {
        if (len < 8)
            return S_BAD_MSG;
        uint32_t id = (uint32_t)ld_le(req + 4, 4);
        if (id > 1)
            return S_BAD_MSG;
        snd_stream *t = &s->st[id];
        if (code == R_PCM_START) {
            t->running = true;
            t->frac = 0;
        } else if (code == R_PCM_STOP) {
            t->running = false;
        } else if (code == R_PCM_RELEASE) {
            t->running = false;
            flush_stream(s, (int)id);
        }
        streams_changed(s);
        return S_OK;
    }
    case R_JACK_INFO:
    case R_CHMAP_INFO:
        return S_BAD_MSG; /* nao ha jacks nem mapas de canais */
    default:
        return S_NOT_SUPP;
    }
}

static void snd_notify(virtio_dev *d, int q)
{
    vsnd *s = d->priv;
    if (q == 0) {
        virtq_elem e;
        bool any = false;
        while (virtq_pop(d, 0, &e)) {
            uint8_t req[64] = {0}, resp[256];
            size_t len = virtq_read(d, &e, 0, req, sizeof(req));
            size_t rlen = 0;
            uint32_t st = control(s, req, len, resp, &rlen);
            uint8_t hdr[4];
            st_le(hdr, st, 4);
            virtq_write(d, &e, 0, hdr, 4);
            if (rlen)
                virtq_write(d, &e, 4, resp, rlen);
            virtq_push(d, 0, &e, (uint32_t)(4 + rlen));
            any = true;
        }
        if (any)
            virtio_notify_irq(d);
    } else if (q == 2 || q == 3) {
        take_pcm(s, q);
    }
    /* fila de eventos (1): os buffers ficam com o dispositivo */
}

static void snd_reset(virtio_dev *d)
{
    vsnd *s = d->priv;
    for (int i = 0; i < 2; i++) {
        snd_stream *t = &s->st[i];
        while (t->count) {
            free(t->q[t->head].e);
            t->q[t->head].e = NULL;
            t->head = (t->head + 1) % MAX_PENDING;
            t->count--;
        }
        t->running = false;
        t->rate = 48000;
        t->nch = 2;
    }
    streams_changed(s);
}

static void snd_destroy(virtio_dev *d)
{
    vsnd *s = d->priv;
    snd_reset(d);
    timer_del(d->vm, &s->timer);
}

virtio_dev *virtio_snd_new(mvm_vm *vm)
{
    virtio_dev *d = calloc(1, sizeof(*d));
    vsnd *s = calloc(1, sizeof(*s));
    s->d = d;
    s->audio = vm->audio;
    d->vm = vm;
    d->priv = s;
    d->device_id = 25;
    d->host_features = 1ULL << 32; /* VERSION_1 */
    d->nvq = 4;
    for (int i = 0; i < 4; i++)
        d->vq[i].num_max = 256;
    d->cfg_read = snd_cfg_read;
    d->notify = snd_notify;
    d->reset = snd_reset;
    d->destroy = snd_destroy;
    timer_init(&s->timer, tick, s);
    for (int i = 0; i < 2; i++) {
        s->st[i].rate = 48000;
        s->st[i].nch = 2;
    }
    return d;
}
