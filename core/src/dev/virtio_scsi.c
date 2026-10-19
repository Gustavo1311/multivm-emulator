/*
 * virtio-scsi (placa ARM "virt"): CD-ROMs com troca de midia e discos que entram e saem com a
 * VM ligada (eventos TRANSPORT_RESET na fila de eventos, VIRTIO_SCSI_F_HOTPLUG).
 *
 * Um alvo por drive, sempre no LUN 0 (max_lun = 0). Alvo vazio responde BAD_TARGET, como no QEMU,
 * para o Linux e o edk2 pularem a varredura. Os comandos sao atendidos na hora, na thread da VM.
 * Disco: blocos de 512 bytes (SBC); CD: 2048 (MMC, os mesmos comandos do ATAPI do IDE).
 */
#include "devices.h"

#include <stdlib.h>

#define VIRTIO_F_VERSION_1 32
#define VIRTIO_SCSI_F_HOTPLUG 1

#define Q_CTRL 0
#define Q_EVENT 1
#define Q_REQ 2

#define SCSI_MAX_TARGETS 8
#define MAX_SECTORS 2048 /* por comando, em blocos de 512: 1 MiB */
#define NEVENTS 16

/* respostas do transporte */
#define VS_OK 0
#define VS_OVERRUN 1
#define VS_BAD_TARGET 3
#define VS_FAILURE 9

/* eventos */
#define EVT_TRANSPORT_RESET 1
#define EVT_MISSED 0x80000000u
#define RESET_RESCAN 1
#define RESET_REMOVED 2

/* status SCSI e chaves de sense */
#define ST_GOOD 0
#define ST_CHECK 2
#define K_NOT_READY 2
#define K_MEDIUM 3
#define K_ILLEGAL 5
#define K_UNIT_ATTN 6
#define K_PROTECT 7

typedef struct {
    bool present;          /* o alvo existe (CD sempre; disco quando conectado) */
    bool cd;
    mvm_blk *blk;          /* NULL = CD sem midia */
    bool unit_attention;   /* proximo comando recebe UNIT ATTENTION */
    uint8_t ua_asc;
    bool media_event;      /* para GET EVENT STATUS NOTIFICATION */
    uint8_t key, asc, ascq;/* sense guardado para REQUEST SENSE */
} scsi_tgt;

typedef struct {
    uint32_t event, reason;
    uint8_t target;
} scsi_evt;

typedef struct {
    scsi_tgt t[SCSI_MAX_TARGETS];
    uint32_t cdb_size, sense_size;
    uint8_t *buf;
    size_t cap;
    scsi_evt ev[NEVENTS];
    int nev;
    bool missed;
} vscsi;

static inline uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static inline void wbe16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline void wbe64(uint8_t *p, uint64_t v) { wbe32(p, (uint32_t)(v >> 32)); wbe32(p + 4, (uint32_t)v); }

static uint8_t *buf_need(vscsi *s, size_t n)
{
    if (n > s->cap) {
        uint8_t *nb = realloc(s->buf, n);
        if (!nb)
            return NULL;
        s->buf = nb;
        s->cap = n;
    }
    return s->buf;
}

/* ------------------------------------------------------------ comandos SCSI */

typedef struct {
    vscsi *s;
    scsi_tgt *t;
    const uint8_t *cdb;
    virtio_dev *d;
    const virtq_elem *e;
    size_t dout_off, dout_len; /* dados do host para o alvo (dentro da cadeia) */
    size_t din_max;            /* espaco para dados do alvo para o host */
    size_t din_len;            /* quanto foi entregue (em s->buf) */
    uint8_t sense[18];
    bool has_sense;
} scsi_req;

static int check(scsi_req *r, uint8_t key, uint8_t asc, uint8_t ascq)
{
    memset(r->sense, 0, sizeof(r->sense));
    r->sense[0] = 0x70;
    r->sense[2] = key;
    r->sense[7] = 10;
    r->sense[12] = asc;
    r->sense[13] = ascq;
    r->has_sense = true;
    r->t->key = key;
    r->t->asc = asc;
    r->t->ascq = ascq;
    r->din_len = 0;
    return ST_CHECK;
}

/* resposta curta: copia ate 'alloc' bytes (e ate o espaco do host) */
static int reply(scsi_req *r, const void *data, size_t len, size_t alloc)
{
    if (len > alloc) len = alloc;
    if (len > r->din_max) len = r->din_max;
    uint8_t *b = buf_need(r->s, len ? len : 1);
    if (!b)
        return check(r, K_MEDIUM, 0x11, 0);
    memcpy(b, data, len);
    r->din_len = len;
    return ST_GOOD;
}

static uint32_t bsize(const scsi_tgt *t) { return t->cd ? 2048 : 512; }
static uint64_t nblocks(const scsi_tgt *t) { return t->blk ? t->blk->size / bsize(t) : 0; }

static int do_read(scsi_req *r, uint64_t lba, uint32_t count)
{
    scsi_tgt *t = r->t;
    if (!t->blk)
        return check(r, K_NOT_READY, 0x3a, 0);
    if (lba + count > nblocks(t) || lba + count < lba)
        return check(r, K_ILLEGAL, 0x21, 0);
    size_t n = (size_t)count * bsize(t);
    if (n > r->din_max)
        n = r->din_max - r->din_max % bsize(t);
    uint8_t *b = buf_need(r->s, n ? n : 1);
    if (!b)
        return check(r, K_MEDIUM, 0x11, 0);
    if (n && blk_read(t->blk, lba * bsize(t), b, n) < 0)
        return check(r, K_MEDIUM, 0x11, 0);
    r->din_len = n;
    return ST_GOOD;
}

static int do_write(scsi_req *r, uint64_t lba, uint32_t count)
{
    scsi_tgt *t = r->t;
    if (t->cd)
        return check(r, K_ILLEGAL, 0x20, 0);
    if (!t->blk)
        return check(r, K_NOT_READY, 0x3a, 0);
    if (t->blk->readonly)
        return check(r, K_PROTECT, 0x27, 0);
    if (lba + count > nblocks(t) || lba + count < lba)
        return check(r, K_ILLEGAL, 0x21, 0);
    size_t n = (size_t)count * 512;
    if (n > r->dout_len)
        return check(r, K_ILLEGAL, 0x24, 0);
    uint8_t *b = buf_need(r->s, n ? n : 1);
    if (!b)
        return check(r, K_MEDIUM, 0x0c, 0);
    virtq_read(r->d, r->e, r->dout_off, b, n);
    if (n && blk_write(t->blk, lba * 512, b, n) < 0)
        return check(r, K_MEDIUM, 0x0c, 0);
    return ST_GOOD;
}

static int do_unmap(scsi_req *r)
{
    scsi_tgt *t = r->t;
    if (t->cd || !t->blk->can_discard || t->blk->readonly)
        return check(r, K_ILLEGAL, 0x20, 0);
    size_t n = r->dout_len < 8 + 16 * 64 ? r->dout_len : 8 + 16 * 64;
    uint8_t p[8 + 16 * 64];
    virtq_read(r->d, r->e, r->dout_off, p, n);
    if (n < 8)
        return ST_GOOD;
    size_t dl = be16(p + 2);
    if (dl > n - 8) dl = n - 8;
    for (size_t i = 0; i + 16 <= dl; i += 16) {
        uint64_t lba = be64(p + 8 + i);
        uint32_t cnt = be32(p + 8 + i + 8);
        if (lba + cnt > nblocks(t))
            return check(r, K_ILLEGAL, 0x21, 0);
        if (cnt && blk_discard(t->blk, lba * 512, (uint64_t)cnt * 512) < 0)
            return check(r, K_MEDIUM, 0x0c, 0);
    }
    return ST_GOOD;
}

static int inquiry(scsi_req *r)
{
    scsi_tgt *t = r->t;
    const uint8_t *cdb = r->cdb;
    uint32_t alloc = be16(cdb + 3);
    uint8_t p[96];
    memset(p, 0, sizeof(p));
    uint8_t type = t->cd ? 0x05 : 0x00;
    if (!(cdb[1] & 1)) {
        if (cdb[2])
            return check(r, K_ILLEGAL, 0x24, 0);
        p[0] = type;
        p[1] = t->cd ? 0x80 : 0x00; /* removivel */
        p[2] = 0x05;                 /* SPC-3 */
        p[3] = 0x12;                 /* HiSup, formato 2 */
        p[4] = 31;
        p[7] = 0x02;                 /* CmdQue */
        memcpy(p + 8, "MULTIVM ", 8);
        memcpy(p + 16, t->cd ? "CD-ROM          " : "HARDDISK        ", 16);
        memcpy(p + 32, "1.0 ", 4);
        return reply(r, p, 36, alloc);
    }
    p[0] = type;
    p[1] = cdb[2];
    unsigned n = 4;
    int tidx = (int)(t - r->s->t);
    switch (cdb[2]) {
    case 0x00: /* paginas suportadas */
        p[n++] = 0x00; p[n++] = 0x80; p[n++] = 0x83;
        if (!t->cd) {
            p[n++] = 0xb0; p[n++] = 0xb1; p[n++] = 0xb2;
        }
        break;
    case 0x80: /* numero de serie */
        n += (unsigned)snprintf((char *)p + 4, 20, "MVM%04d", tidx);
        break;
    case 0x83: { /* identificacao: designador T10 */
        uint8_t *dsc = p + 4;
        dsc[0] = 0x02; dsc[1] = 0x01; /* ASCII, T10 vendor id, associado ao LUN */
        int len = snprintf((char *)dsc + 4, 40, "MULTIVM %s%d", t->cd ? "CD" : "HD", tidx);
        dsc[3] = (uint8_t)len;
        n += 4 + (unsigned)len;
        break;
    }
    case 0xb0: /* limites de bloco */
        if (t->cd) return check(r, K_ILLEGAL, 0x24, 0);
        wbe32(p + 8, MAX_SECTORS);            /* tamanho maximo de transferencia */
        wbe32(p + 12, 8);                     /* otimo: 4 KiB */
        if (t->blk && t->blk->can_discard) {
            wbe32(p + 20, 1u << 22);          /* blocos por UNMAP */
            wbe32(p + 24, 64);                /* descritores por UNMAP */
            wbe32(p + 28, 8);                 /* granularidade de 4 KiB */
        }
        n = 64;
        break;
    case 0xb1: /* caracteristicas: sem rotacao (SSD) */
        if (t->cd) return check(r, K_ILLEGAL, 0x24, 0);
        wbe16(p + 4, 1);
        n = 64;
        break;
    case 0xb2: /* provisionamento: UNMAP se o formato descarta */
        if (t->cd) return check(r, K_ILLEGAL, 0x24, 0);
        p[5] = t->blk && t->blk->can_discard ? 0x84 : 0x00; /* LBPU, LBPRZ */
        n = 8;
        break;
    default:
        return check(r, K_ILLEGAL, 0x24, 0);
    }
    wbe16(p + 2, n - 4);
    return reply(r, p, n, alloc);
}

static int mode_sense(scsi_req *r)
{
    scsi_tgt *t = r->t;
    const uint8_t *cdb = r->cdb;
    bool ten = cdb[0] == 0x5a;
    uint32_t alloc = ten ? be16(cdb + 7) : cdb[4];
    unsigned page = cdb[2] & 0x3f, hl = ten ? 8 : 4, n = hl;
    uint8_t p[128];
    memset(p, 0, sizeof(p));
    if (t->cd) {
        if (page == 0x01 || page == 0x3f) {
            p[n] = 0x01; p[n + 1] = 0x06; p[n + 3] = 5;
            n += 8;
        }
        if (page == 0x2a || page == 0x3f) { /* capacidades do drive (MMC) */
            uint8_t *q = p + n;
            q[0] = 0x2a; q[1] = 0x12;
            q[2] = 0x3b; q[3] = 0x00; q[4] = 0x71; q[5] = 0x60; q[6] = 0x29; q[7] = 0x00;
            wbe16(q + 8, 5540); wbe16(q + 10, 2); wbe16(q + 12, 512); wbe16(q + 14, 5540);
            n += 20;
        }
    } else {
        if (page == 0x08 || page == 0x3f) { /* cache: escrita com cache ligada */
            p[n] = 0x08; p[n + 1] = 0x12; p[n + 2] = 0x04;
            n += 20;
        }
        if (page == 0x0a || page == 0x3f) { /* controle */
            p[n] = 0x0a; p[n + 1] = 0x0a;
            n += 12;
        }
    }
    if (n == hl && page != 0x3f)
        return check(r, K_ILLEGAL, 0x24, 0);
    uint8_t dsp = !t->cd && t->blk && t->blk->readonly ? 0x80 : 0x00; /* WP */
    if (ten) {
        wbe16(p, n - 2);
        p[3] = dsp;
    } else {
        p[0] = (uint8_t)(n - 1);
        p[2] = dsp;
    }
    return reply(r, p, n, alloc);
}

static void lba_to_msf(uint8_t *p, uint32_t lba)
{
    lba += 150;
    p[0] = 0;
    p[1] = (uint8_t)(lba / (60 * 75));
    p[2] = (uint8_t)((lba / 75) % 60);
    p[3] = (uint8_t)(lba % 75);
}

/* comandos so de CD (MMC); -1 = nao e um deles */
static int cd_command(scsi_req *r)
{
    scsi_tgt *t = r->t;
    const uint8_t *cdb = r->cdb;
    bool media = t->blk != NULL;
    uint32_t last = media ? (uint32_t)(nblocks(t) - 1) : 0;
    uint8_t p[128];
    memset(p, 0, sizeof(p));
    switch (cdb[0]) {
    case 0xbe: /* READ CD: so dados de usuario */
        if ((cdb[9] & 0xf8) != 0x10)
            return check(r, K_ILLEGAL, 0x24, 0);
        return do_read(r, be32(cdb + 2), ((uint32_t)cdb[6] << 16) | be16(cdb + 7));
    case 0x43: { /* READ TOC */
        if (!media) return check(r, K_NOT_READY, 0x3a, 0);
        bool msf = cdb[1] & 2;
        unsigned fmt = cdb[2] & 0xf;
        if (!fmt) fmt = cdb[9] >> 6;
        uint32_t alloc = be16(cdb + 7);
        unsigned n;
        if (fmt == 0) {
            p[2] = 1; p[3] = 1;
            n = 4;
            if (cdb[6] <= 1) {
                p[n + 1] = 0x14; p[n + 2] = 1;
                if (msf) lba_to_msf(p + n + 4, 0); else wbe32(p + n + 4, 0);
                n += 8;
            }
            p[n + 1] = 0x16; p[n + 2] = 0xaa;
            if (msf) lba_to_msf(p + n + 4, last + 1); else wbe32(p + n + 4, last + 1);
            n += 8;
        } else if (fmt == 1) {
            p[2] = 1; p[3] = 1;
            p[5] = 0x14; p[6] = 1;
            n = 12;
        } else {
            return check(r, K_ILLEGAL, 0x24, 0);
        }
        wbe16(p, n - 2);
        return reply(r, p, n, alloc);
    }
    case 0x42: /* READ SUB-CHANNEL */
        p[1] = 0x15;
        return reply(r, p, 4, be16(cdb + 7));
    case 0x46: { /* GET CONFIGURATION */
        uint16_t prof = !media ? 0 : (t->blk->size > 700ull * 1024 * 1024 ? 0x10 : 0x08); /* DVD : CD */
        wbe16(p + 6, prof);
        uint8_t *f = p + 8;
        f[2] = 0x03; f[3] = 8;
        wbe16(f + 4, 0x10); f[6] = prof == 0x10;
        wbe16(f + 8, 0x08); f[10] = prof == 0x08;
        wbe32(p, 8 + 12 - 4);
        return reply(r, p, 20, be16(cdb + 7));
    }
    case 0x4a: { /* GET EVENT STATUS NOTIFICATION */
        if (!(cdb[1] & 1))
            return check(r, K_ILLEGAL, 0x24, 0);
        wbe16(p, 6);
        p[2] = 4;
        p[3] = 0x10;
        if (t->media_event) { /* 2 = midia nova, 3 = removida */
            p[4] = media ? 2 : 3;
            t->media_event = false;
        }
        p[5] = media ? 2 : 0;
        return reply(r, p, 8, be16(cdb + 7));
    }
    case 0x51: /* READ DISC INFORMATION */
        if (!media) return check(r, K_NOT_READY, 0x3a, 0);
        wbe16(p, 32);
        p[2] = 0x0e;
        p[3] = 1; p[4] = 1; p[5] = 1; p[6] = 1;
        return reply(r, p, 34, be16(cdb + 7));
    case 0x52: { /* READ TRACK INFORMATION: uma trilha de dados */
        if (!media) return check(r, K_NOT_READY, 0x3a, 0);
        wbe16(p, 34);
        p[2] = 1; p[3] = 1;
        p[5] = 0x04;
        p[6] = 0x01;
        wbe32(p + 24, last + 1);
        return reply(r, p, 36, be16(cdb + 7));
    }
    case 0xbd: /* MECHANISM STATUS */
        wbe16(p + 6, 8);
        return reply(r, p, 8, be16(cdb + 8));
    case 0xbb: case 0x2b: /* SET SPEED, SEEK */
        return ST_GOOD;
    default:
        return -1;
    }
}

static int scsi_exec(scsi_req *r)
{
    scsi_tgt *t = r->t;
    const uint8_t *cdb = r->cdb;
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    r->din_len = 0;

    /* midia trocada / disco novo: avisa no primeiro comando que nao seja de estado */
    if (t->unit_attention && cdb[0] != 0x12 && cdb[0] != 0x03 && cdb[0] != 0xa0 && cdb[0] != 0x4a) {
        t->unit_attention = false;
        return check(r, K_UNIT_ATTN, t->ua_asc, 0);
    }

    switch (cdb[0]) {
    case 0x00: /* TEST UNIT READY */
        return t->blk ? ST_GOOD : check(r, K_NOT_READY, 0x3a, 0);
    case 0x03: /* REQUEST SENSE */
        p[0] = 0x70;
        p[2] = t->key;
        p[7] = 10;
        p[12] = t->asc;
        p[13] = t->ascq;
        if (!t->blk && t->cd && !t->key) { /* sem midia */
            p[2] = K_NOT_READY;
            p[12] = 0x3a;
        }
        t->key = t->asc = t->ascq = 0;
        return reply(r, p, 18, cdb[4]);
    case 0x12:
        return inquiry(r);
    case 0xa0: /* REPORT LUNS: so o LUN 0 */
        wbe32(p, 8);
        return reply(r, p, 16, be32(cdb + 6));
    case 0x1a: case 0x5a:
        return mode_sense(r);
    case 0x15: case 0x55: /* MODE SELECT: aceita e ignora */
    case 0x1b:            /* START STOP UNIT */
    case 0x1e:            /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2f: case 0x8f: /* VERIFY */
        return ST_GOOD;
    case 0x35: case 0x91: /* SYNCHRONIZE CACHE */
        if (!t->cd && t->blk && !t->blk->readonly && blk_flush(t->blk) < 0)
            return check(r, K_MEDIUM, 0x0c, 0);
        return ST_GOOD;
    case 0x25: { /* READ CAPACITY(10) */
        if (!t->blk) return check(r, K_NOT_READY, 0x3a, 0);
        uint64_t last = nblocks(t) - 1;
        wbe32(p, last > 0xffffffffu ? 0xffffffffu : (uint32_t)last);
        wbe32(p + 4, bsize(t));
        return reply(r, p, 8, 8);
    }
    case 0x9e: /* SERVICE ACTION IN(16) */
        if ((cdb[1] & 0x1f) != 0x10)
            return check(r, K_ILLEGAL, 0x24, 0);
        if (!t->blk) return check(r, K_NOT_READY, 0x3a, 0);
        wbe64(p, nblocks(t) - 1);
        wbe32(p + 8, bsize(t));
        if (!t->cd && t->blk->can_discard)
            p[14] = 0xc0; /* LBPME, LBPRZ */
        return reply(r, p, 32, be32(cdb + 10));
    case 0x08: /* READ(6) */
        return do_read(r, ((uint32_t)(cdb[1] & 0x1f) << 16) | be16(cdb + 2), cdb[4] ? cdb[4] : 256);
    case 0x28: return do_read(r, be32(cdb + 2), be16(cdb + 7));
    case 0xa8: return do_read(r, be32(cdb + 2), be32(cdb + 6));
    case 0x88: return do_read(r, be64(cdb + 2), be32(cdb + 10));
    case 0x0a: /* WRITE(6) */
        return do_write(r, ((uint32_t)(cdb[1] & 0x1f) << 16) | be16(cdb + 2), cdb[4] ? cdb[4] : 256);
    case 0x2a: case 0x2e: return do_write(r, be32(cdb + 2), be16(cdb + 7));
    case 0xaa: return do_write(r, be32(cdb + 2), be32(cdb + 6));
    case 0x8a: case 0x8e: return do_write(r, be64(cdb + 2), be32(cdb + 10));
    default:
        break;
    }
    if (t->cd) {
        int st = cd_command(r);
        if (st >= 0)
            return st;
    } else if (cdb[0] == 0x42) { /* UNMAP */
        if (!t->blk) return check(r, K_NOT_READY, 0x3a, 0);
        return do_unmap(r);
    }
    LOGD("virtio-scsi: comando 0x%02x nao suportado", cdb[0]);
    return check(r, K_ILLEGAL, 0x20, 0);
}

/* ------------------------------------------------------------ filas */

/* alvo do endereco LUN do virtio (1, alvo, 0x40|lun>>8, lun); NULL = nao existe */
static scsi_tgt *lun_target(vscsi *s, const uint8_t *lun)
{
    if (lun[0] != 1 || lun[1] >= SCSI_MAX_TARGETS)
        return NULL;
    unsigned l = ((lun[2] & 0x3f) << 8) | lun[3];
    scsi_tgt *t = &s->t[lun[1]];
    if (!t->present || l != 0)
        return NULL;
    return t;
}

static void req_notify(virtio_dev *d, int qi)
{
    vscsi *s = d->priv;
    virtq_elem *e = malloc(sizeof(*e));
    if (!e)
        return;
    bool any = false;
    size_t req_len = 19 + s->cdb_size, resp_len = 12 + s->sense_size;
    while (virtq_pop(d, qi, e)) {
        size_t out_len = 0, in_len = 0;
        for (int i = 0; i < e->nseg; i++) {
            if (e->seg[i].write) in_len += e->seg[i].len;
            else out_len += e->seg[i].len;
        }
        uint8_t hdr[19 + 64];
        uint8_t resp[12 + 256];
        memset(resp, 0, sizeof(resp));
        uint32_t written = 0;
        if (out_len < req_len || in_len < resp_len || req_len > sizeof(hdr) || resp_len > sizeof(resp)) {
            LOGW("virtio-scsi: pedido malformado (out=%zu in=%zu)", out_len, in_len);
            if (in_len)
                virtq_write(d, e, 0, resp, in_len < resp_len ? in_len : resp_len);
            virtq_push(d, qi, e, 0);
            any = true;
            continue;
        }
        virtq_read(d, e, 0, hdr, req_len);
        scsi_tgt *t = lun_target(s, hdr);
        if (!t) {
            resp[11] = VS_BAD_TARGET;
            wbe32(resp + 4, 0);
        } else {
            scsi_req r = {.s = s, .t = t, .cdb = hdr + 19, .d = d, .e = e, .dout_off = req_len,
                          .dout_len = out_len - req_len, .din_max = in_len - resp_len};
            int st = scsi_exec(&r);
            resp[10] = (uint8_t)st;
            resp[11] = VS_OK;
            if (r.has_sense) {
                size_t sl = sizeof(r.sense) < s->sense_size ? sizeof(r.sense) : s->sense_size;
                uint32_t sl32 = (uint32_t)sl;
                memcpy(resp, &sl32, 4);
                memcpy(resp + 12, r.sense, sl);
            }
            /* resid: o que faltou transferir */
            uint32_t resid = r.din_max > r.din_len ? (uint32_t)(r.din_max - r.din_len) : 0;
            memcpy(resp + 4, &resid, 4);
            if (r.din_len) {
                virtq_write(d, e, resp_len, s->buf, r.din_len);
                written = (uint32_t)r.din_len;
            }
        }
        virtq_write(d, e, 0, resp, resp_len);
        virtq_push(d, qi, e, (uint32_t)resp_len + written);
        any = true;
    }
    free(e);
    if (any)
        virtio_notify_irq(d);
}

static void ctrl_notify(virtio_dev *d, int qi)
{
    virtq_elem *e = malloc(sizeof(*e));
    if (!e)
        return;
    bool any = false;
    while (virtq_pop(d, qi, e)) {
        uint8_t h[4] = {0};
        virtq_read(d, e, 0, h, 4);
        uint32_t type = (uint32_t)ld_le(h, 4);
        uint8_t resp[5] = {0}; /* TMF: response (1 byte); AN: event_actual (4) + response */
        uint32_t n = type == 0 ? 1 : 5;
        virtq_write(d, e, 0, resp, n);
        virtq_push(d, qi, e, n);
        any = true;
    }
    free(e);
    if (any)
        virtio_notify_irq(d);
}

/* entrega os eventos pendentes nos buffers que o convidado deixou na fila de eventos */
static void flush_events(virtio_dev *d)
{
    vscsi *s = d->priv;
    if (!s->nev || !(d->status & 4)) /* so com o driver pronto */
        return;
    virtq_elem *e = malloc(sizeof(*e));
    if (!e)
        return;
    bool any = false;
    while (s->nev && virtq_pop(d, Q_EVENT, e)) {
        scsi_evt *v = &s->ev[0];
        uint8_t b[16] = {0};
        uint32_t ev = v->event | (s->missed ? EVT_MISSED : 0);
        memcpy(b, &ev, 4);
        b[4] = 1;
        b[5] = v->target; /* LUN 0: bytes 2-3 zerados (o Linux usa o valor como numero do LUN) */
        memcpy(b + 12, &v->reason, 4);
        virtq_write(d, e, 0, b, 16);
        virtq_push(d, Q_EVENT, e, 16);
        s->missed = false;
        memmove(s->ev, s->ev + 1, (size_t)(--s->nev) * sizeof(*s->ev));
        any = true;
    }
    free(e);
    if (any)
        virtio_notify_irq(d);
}

static void post_event(virtio_dev *d, uint32_t event, uint32_t reason, int target)
{
    vscsi *s = d->priv;
    LOGI("virtio-scsi: alvo %d %s", target, reason == RESET_RESCAN ? "conectado" : "removido");
    if (!(d->guest_features & (1ULL << VIRTIO_SCSI_F_HOTPLUG))) {
        LOGI("virtio-scsi: driver sem hotplug (features %llx)", (unsigned long long)d->guest_features);
        return;
    } /* driver sem hotplug (o edk2): ele varre os alvos de novo quando precisar */
    if (s->nev == NEVENTS) {
        s->missed = true;
        return;
    }
    s->ev[s->nev++] = (scsi_evt){event, reason, (uint8_t)target};
    flush_events(d);
}

static void scsi_notify(virtio_dev *d, int qi)
{
    if (qi == Q_CTRL)
        ctrl_notify(d, qi);
    else if (qi == Q_EVENT)
        flush_events(d);
    else
        req_notify(d, qi);
}

static uint32_t scsi_cfg_read(virtio_dev *d, uint32_t off, unsigned size)
{
    vscsi *s = d->priv;
    uint8_t cfg[36] = {0};
    uint32_t v[7] = {1, VIRTQ_MAX_SEGS - 2, MAX_SECTORS, 128, 16, s->sense_size, s->cdb_size};
    memcpy(cfg, v, sizeof(v));
    uint16_t max_channel = 0, max_target = SCSI_MAX_TARGETS - 1;
    uint32_t max_lun = 0;
    memcpy(cfg + 28, &max_channel, 2);
    memcpy(cfg + 30, &max_target, 2);
    memcpy(cfg + 32, &max_lun, 4);
    if (off + size > sizeof(cfg))
        return 0;
    return (uint32_t)ld_le(cfg + off, size);
}

static void scsi_cfg_write(virtio_dev *d, uint32_t off, uint32_t val, unsigned size)
{
    vscsi *s = d->priv;
    (void)size;
    if (off == 20 && val >= 18 && val <= 256)
        s->sense_size = val;
    else if (off == 24 && val >= 6 && val <= 64)
        s->cdb_size = val;
}

static void scsi_reset(virtio_dev *d)
{
    vscsi *s = d->priv;
    s->sense_size = 96;
    s->cdb_size = 32;
    s->nev = 0;
    s->missed = false;
}

static void scsi_destroy(virtio_dev *d)
{
    vscsi *s = d->priv;
    free(s->buf);
}

/* ------------------------------------------------------------ API */

virtio_dev *virtio_scsi_new(mvm_vm *vm)
{
    virtio_dev *d = calloc(1, sizeof(*d));
    vscsi *s = calloc(1, sizeof(*s));
    if (!d || !s) {
        free(d);
        free(s);
        return NULL;
    }
    d->vm = vm;
    d->priv = s;
    d->device_id = 8;
    d->host_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_SCSI_F_HOTPLUG);
    d->nvq = 3;
    for (int i = 0; i < 3; i++)
        d->vq[i].num_max = i == Q_REQ ? 256 : 64;
    d->cfg_read = scsi_cfg_read;
    d->cfg_write = scsi_cfg_write;
    d->notify = scsi_notify;
    d->reset = scsi_reset;
    d->destroy = scsi_destroy;
    scsi_reset(d);
    return d;
}

int virtio_scsi_free_target(virtio_dev *d)
{
    vscsi *s = d->priv;
    for (int i = 0; i < SCSI_MAX_TARGETS; i++)
        if (!s->t[i].present)
            return i;
    return -1;
}

int virtio_scsi_attach(virtio_dev *d, mvm_blk *blk, bool cd)
{
    vscsi *s = d->priv;
    int i = virtio_scsi_free_target(d);
    if (i < 0)
        return -1;
    s->t[i] = (scsi_tgt){.present = true, .cd = cd, .blk = blk};
    return i;
}

static bool scsi_change_locked(virtio_dev *d, int target, mvm_blk *blk, bool cd);

/* com a bql (troca de midia): a trava do dispositivo protege os alvos da fila de pedidos */
bool virtio_scsi_change(virtio_dev *d, int target, mvm_blk *blk, bool cd)
{
    if (d->own_lock)
        pthread_mutex_lock(&d->mu);
    bool r = scsi_change_locked(d, target, blk, cd);
    if (d->own_lock)
        pthread_mutex_unlock(&d->mu);
    return r;
}

static bool scsi_change_locked(virtio_dev *d, int target, mvm_blk *blk, bool cd)
{
    vscsi *s = d->priv;
    if (target < 0 || target >= SCSI_MAX_TARGETS)
        return false;
    scsi_tgt *t = &s->t[target];
    if (cd) { /* CD: troca a midia; o convidado ve UNIT ATTENTION e o evento de midia */
        if (!t->present || !t->cd)
            return false;
        t->blk = blk;
        t->unit_attention = true;
        t->ua_asc = 0x28;
        t->media_event = true;
        return true;
    }
    if (blk) { /* disco novo: alvo aparece e o convidado varre */
        *t = (scsi_tgt){.present = true, .cd = false, .blk = blk};
        post_event(d, EVT_TRANSPORT_RESET, RESET_RESCAN, target);
    } else {
        if (!t->present || t->cd)
            return false;
        *t = (scsi_tgt){0};
        post_event(d, EVT_TRANSPORT_RESET, RESET_REMOVED, target);
    }
    return true;
}
