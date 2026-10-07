/* Nucleo virtio (virtqueues split), transporte MMIO (v2) e virtio-blk. */
#include "devices.h"

#include <stdlib.h>

static inline uint16_t rd16(virtio_dev *d, uint64_t a) { return (uint16_t)space_read(&d->vm->mem, a, 2); }
static inline uint32_t rd32(virtio_dev *d, uint64_t a) { return (uint32_t)space_read(&d->vm->mem, a, 4); }
static inline uint64_t rd64(virtio_dev *d, uint64_t a) { return space_read(&d->vm->mem, a, 8); }

void virtio_reset(virtio_dev *d)
{
    d->status = 0;
    d->isr = 0;
    d->guest_features = 0;
    for (int i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        virtq *q = &d->vq[i];
        q->num = 0;
        q->ready = false;
        q->desc = q->avail = q->used = 0;
        q->last_avail = 0;
    }
    if (d->reset)
        d->reset(d);
    virtio_update_irq(d);
}

void virtio_update_irq(virtio_dev *d) { irq_set(&d->irq, d->isr ? 1 : 0); }

void virtio_notify_irq(virtio_dev *d)
{
    d->isr |= 1;
    virtio_update_irq(d);
}

bool virtq_pop(virtio_dev *d, int qi, virtq_elem *e)
{
    virtq *q = &d->vq[qi];
    if (!q->ready || !q->num)
        return false;
    uint16_t avail_idx = rd16(d, q->avail + 2);
    if (avail_idx == q->last_avail)
        return false;
    uint16_t head = rd16(d, q->avail + 4 + 2 * (q->last_avail % q->num));
    q->last_avail++;
    e->head = head;
    e->nseg = 0;
    uint64_t table = q->desc;
    uint32_t tnum = q->num;
    uint16_t i = head;
    for (unsigned guard = 0; guard < 1024; guard++) {
        if (i >= tnum) {
            LOGW("virtio: descritor invalido %u", i);
            break;
        }
        uint64_t da = table + 16ULL * i;
        uint64_t addr = rd64(d, da);
        uint32_t len = rd32(d, da + 8);
        uint16_t flags = rd16(d, da + 12);
        uint16_t next = rd16(d, da + 14);
        if (flags & VIRTQ_DESC_F_INDIRECT) {
            table = addr;
            tnum = len / 16;
            i = 0;
            continue;
        }
        if (e->nseg < VIRTQ_MAX_SEGS) {
            e->seg[e->nseg].addr = addr;
            e->seg[e->nseg].len = len;
            e->seg[e->nseg].write = flags & VIRTQ_DESC_F_WRITE;
            e->nseg++;
        }
        if (!(flags & VIRTQ_DESC_F_NEXT))
            break;
        i = next;
    }
    return true;
}

void virtq_push(virtio_dev *d, int qi, const virtq_elem *e, uint32_t len)
{
    virtq *q = &d->vq[qi];
    uint16_t idx = rd16(d, q->used + 2);
    uint64_t ue = q->used + 4 + 8ULL * (idx % q->num);
    space_write(&d->vm->mem, ue, e->head, 4);
    space_write(&d->vm->mem, ue + 4, len, 4);
    space_write(&d->vm->mem, q->used + 2, (uint16_t)(idx + 1), 2);
}

size_t virtq_read(virtio_dev *d, const virtq_elem *e, size_t off, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t done = 0;
    for (int i = 0; i < e->nseg && done < len; i++) {
        const virtq_seg *s = &e->seg[i];
        if (s->write)
            continue;
        if (off >= s->len) {
            off -= s->len;
            continue;
        }
        size_t n = s->len - off;
        if (n > len - done)
            n = len - done;
        space_memread(&d->vm->mem, s->addr + off, p + done, n);
        done += n;
        off = 0;
    }
    return done;
}

size_t virtq_write(virtio_dev *d, const virtq_elem *e, size_t off, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t done = 0;
    for (int i = 0; i < e->nseg && done < len; i++) {
        const virtq_seg *s = &e->seg[i];
        if (!s->write)
            continue;
        if (off >= s->len) {
            off -= s->len;
            continue;
        }
        size_t n = s->len - off;
        if (n > len - done)
            n = len - done;
        space_memwrite(&d->vm->mem, s->addr + off, p + done, n);
        done += n;
        off = 0;
    }
    return done;
}

void virtio_dev_free(virtio_dev *d)
{
    if (!d)
        return;
    if (d->destroy)
        d->destroy(d);
    free(d->priv);
    free(d);
}

/* ------------------------------------------------------ transporte MMIO */

/* Estado especifico do transporte MMIO: guardado junto ao dispositivo. */
typedef struct {
    virtio_dev *d;
    uint32_t dev_feat_sel, drv_feat_sel, queue_sel;
} vmmio;

static uint64_t vmmio_read(void *opaque, uint64_t off, unsigned size)
{
    vmmio *m = opaque;
    virtio_dev *d = m->d;
    if (off >= 0x100)
        return d->cfg_read ? d->cfg_read(d, (uint32_t)(off - 0x100), size) : 0;
    virtq *q = m->queue_sel < VIRTIO_MAX_QUEUES ? &d->vq[m->queue_sel] : NULL;
    switch (off) {
    case 0x000: return 0x74726976;
    case 0x004: return 2;
    case 0x008: return d->device_id;
    case 0x00c: return 0x4d564d4d;
    case 0x010: return m->dev_feat_sel < 2 ? (uint32_t)(d->host_features >> (32 * m->dev_feat_sel)) : 0;
    case 0x034: return (q && (int)m->queue_sel < d->nvq) ? q->num_max : 0;
    case 0x044: return q ? q->ready : 0;
    case 0x060: return d->isr;
    case 0x070: return d->status;
    case 0x0fc: return d->cfg_gen;
    default: return 0;
    }
}

static void vmmio_write(void *opaque, uint64_t off, uint64_t val, unsigned size)
{
    vmmio *m = opaque;
    virtio_dev *d = m->d;
    uint32_t v = (uint32_t)val;
    if (off >= 0x100) {
        if (d->cfg_write)
            d->cfg_write(d, (uint32_t)(off - 0x100), v, size);
        return;
    }
    virtq *q = m->queue_sel < VIRTIO_MAX_QUEUES ? &d->vq[m->queue_sel] : NULL;
    switch (off) {
    case 0x014: m->dev_feat_sel = v; break;
    case 0x020:
        if (m->drv_feat_sel < 2) {
            uint64_t mask = 0xffffffffULL << (32 * m->drv_feat_sel);
            d->guest_features = (d->guest_features & ~mask) | (((uint64_t)v << (32 * m->drv_feat_sel)) & mask);
        }
        break;
    case 0x024: m->drv_feat_sel = v; break;
    case 0x030: m->queue_sel = v; break;
    case 0x038: if (q) q->num = v <= q->num_max ? v : q->num_max; break;
    case 0x044: if (q) q->ready = v & 1; break;
    case 0x050:
        if (v < (uint32_t)d->nvq && d->notify)
            d->notify(d, (int)v);
        break;
    case 0x064:
        d->isr &= ~v;
        virtio_update_irq(d);
        break;
    case 0x070:
        if (v == 0) {
            virtio_reset(d);
            m->dev_feat_sel = m->drv_feat_sel = m->queue_sel = 0;
        } else {
            d->status = v;
        }
        break;
    case 0x080: if (q) q->desc = (q->desc & ~0xffffffffULL) | v; break;
    case 0x084: if (q) q->desc = (q->desc & 0xffffffffULL) | ((uint64_t)v << 32); break;
    case 0x090: if (q) q->avail = (q->avail & ~0xffffffffULL) | v; break;
    case 0x094: if (q) q->avail = (q->avail & 0xffffffffULL) | ((uint64_t)v << 32); break;
    case 0x0a0: if (q) q->used = (q->used & ~0xffffffffULL) | v; break;
    case 0x0a4: if (q) q->used = (q->used & 0xffffffffULL) | ((uint64_t)v << 32); break;
    default: break;
    }
}

/* A maquina cria o estado do transporte com virtio_mmio_wrap(). */
const mvm_io_ops virtio_mmio_ops = {vmmio_read, vmmio_write};

void *virtio_mmio_wrap(virtio_dev *d)
{
    vmmio *m = calloc(1, sizeof(*m));
    m->d = d;
    return m;
}

void virtio_mmio_unwrap(void *opaque) { free(opaque); }

/* ------------------------------------------------------------ virtio-blk */

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14
#define DISCARD_MAX_SECTORS (1u << 22) /* 2 GiB por segmento */
#define DISCARD_MAX_SEG 64
#define VIRTIO_F_VERSION_1 32

typedef struct {
    mvm_blk *blk;
    uint8_t *buf;
    size_t buf_cap;
} vblk;

static uint32_t blk_cfg_read(virtio_dev *d, uint32_t off, unsigned size)
{
    vblk *b = d->priv;
    uint8_t cfg[64] = {0};
    uint64_t sectors = b->blk->size / 512;
    memcpy(cfg + 0, &sectors, 8);
    uint32_t size_max = 1u << 20, seg_max = VIRTQ_MAX_SEGS - 2, blk_size = 512;
    memcpy(cfg + 8, &size_max, 4);
    memcpy(cfg + 12, &seg_max, 4);
    memcpy(cfg + 20, &blk_size, 4);
    uint32_t dmax = DISCARD_MAX_SECTORS, dseg = DISCARD_MAX_SEG, dalign = 8; /* 4 KiB */
    memcpy(cfg + 36, &dmax, 4);
    memcpy(cfg + 40, &dseg, 4);
    memcpy(cfg + 44, &dalign, 4);
    memcpy(cfg + 48, &dmax, 4);
    memcpy(cfg + 52, &dseg, 4);
    cfg[56] = 1; /* write_zeroes_may_unmap */
    if (off + size > sizeof(cfg))
        return 0;
    return (uint32_t)ld_le(cfg + off, size);
}

static void blk_notify(virtio_dev *d, int qi)
{
    vblk *b = d->priv;
    virtq_elem *e = malloc(sizeof(*e));
    bool any = false;
    while (virtq_pop(d, qi, e)) {
        uint8_t hdr[16];
        uint8_t status = 0;
        uint32_t written = 0;
        if (virtq_read(d, e, 0, hdr, 16) < 16) {
            LOGW("virtio-blk: cabecalho curto");
            status = 1;
        } else {
            uint32_t type = (uint32_t)ld_le(hdr, 4);
            uint64_t sector = ld_le(hdr + 8, 8);
            /* tamanho dos dados: segmentos exceto cabecalho e byte de status */
            size_t in_len = 0, out_len = 0;
            for (int i = 0; i < e->nseg; i++) {
                if (e->seg[i].write) in_len += e->seg[i].len;
                else out_len += e->seg[i].len;
            }
            uint64_t off = sector * 512;
            LOGD("virtio-blk: req tipo %u setor %llu in=%zu out=%zu", type, (unsigned long long)sector, in_len, out_len);
            switch (type) {
            case 0: { /* IN (leitura) */
                size_t n = in_len ? in_len - 1 : 0;
                if (n > b->buf_cap) {
                    b->buf = realloc(b->buf, n);
                    b->buf_cap = n;
                }
                if (off + n > b->blk->size || blk_read(b->blk, off, b->buf, n) < 0) {
                    status = 1;
                } else {
                    virtq_write(d, e, 0, b->buf, n);
                    written = (uint32_t)n;
                }
                break;
            }
            case 1: { /* OUT (escrita) */
                size_t n = out_len - 16;
                if (n > b->buf_cap) {
                    b->buf = realloc(b->buf, n);
                    b->buf_cap = n;
                }
                virtq_read(d, e, 16, b->buf, n);
                if (b->blk->readonly || off + n > b->blk->size || blk_write(b->blk, off, b->buf, n) < 0)
                    status = 1;
                break;
            }
            case 11:   /* DISCARD */
            case 13: { /* WRITE_ZEROES: o discard dos formatos sempre deixa zeros */
                size_t n = out_len - 16;
                if (b->blk->readonly || n % 16 || n / 16 > DISCARD_MAX_SEG) {
                    status = 1;
                    break;
                }
                uint8_t seg[16 * DISCARD_MAX_SEG];
                virtq_read(d, e, 16, seg, n);
                for (size_t i = 0; i < n && status == 0; i += 16) {
                    uint64_t s0 = ld_le(seg + i, 8);
                    uint32_t cnt = (uint32_t)ld_le(seg + i + 8, 4);
                    if (cnt > DISCARD_MAX_SECTORS || s0 + cnt > b->blk->size / 512)
                        status = 1;
                    else if (blk_discard(b->blk, s0 * 512, (uint64_t)cnt * 512) < 0)
                        status = 1;
                }
                break;
            }
            case 4: /* FLUSH */
                status = blk_flush(b->blk) < 0 ? 1 : 0;
                break;
            case 8: { /* GET_ID */
                char id[20] = "multivm-disk";
                size_t n = in_len ? in_len - 1 : 0;
                virtq_write(d, e, 0, id, n < 20 ? n : 20);
                written = n < 20 ? (uint32_t)n : 20;
                break;
            }
            default:
                status = 2;
                break;
            }
        }
        /* status: ultimo byte gravavel */
        size_t in_total = 0;
        for (int i = 0; i < e->nseg; i++)
            if (e->seg[i].write) in_total += e->seg[i].len;
        if (in_total)
            virtq_write(d, e, in_total - 1, &status, 1);
        virtq_push(d, qi, e, written + 1);
        any = true;
    }
    free(e);
    if (any)
        virtio_notify_irq(d);
}

static void blk_destroy(virtio_dev *d)
{
    vblk *b = d->priv;
    free(b->buf);
}

virtio_dev *virtio_blk_new(mvm_vm *vm, mvm_blk *blk)
{
    virtio_dev *d = calloc(1, sizeof(*d));
    vblk *b = calloc(1, sizeof(*b));
    b->blk = blk;
    d->vm = vm;
    d->priv = b;
    d->device_id = 2;
    d->host_features = (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                       (1ULL << VIRTIO_BLK_F_BLK_SIZE) | (1ULL << VIRTIO_BLK_F_FLUSH) |
                       (1ULL << VIRTIO_BLK_F_SIZE_MAX);
    if (blk->readonly)
        d->host_features |= 1ULL << VIRTIO_BLK_F_RO;
    else
        d->host_features |= 1ULL << VIRTIO_BLK_F_WRITE_ZEROES;
    if (blk->can_discard)
        d->host_features |= 1ULL << VIRTIO_BLK_F_DISCARD;
    d->nvq = 1;
    d->vq[0].num_max = 256;
    d->cfg_read = blk_cfg_read;
    d->notify = blk_notify;
    d->destroy = blk_destroy;
    return d;
}
