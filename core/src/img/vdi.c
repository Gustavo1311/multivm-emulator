/*
 * VDI (VirtualBox), versao 1.1, imagens dinamicas e fixas, leitura e escrita.
 * Mapa de blocos (uint32) inteiro na RAM; blocos novos sao anexados no fim, na
 * ordem segura: dados -> entrada do mapa -> contador no cabecalho.
 * Recusado: imagens diferenciais/undo (o pai e achado por UUID no VirtualBox).
 */
#include "img.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VDI_SIGNATURE 0xbeda107fu
#define VDI_UNALLOCATED 0xffffffffu
#define VDI_ZERO 0xfffffffeu

typedef struct {
    int fd;
    bool ro;
    uint64_t vsize;
    uint32_t bsize, extra;
    uint32_t nblocks, nalloc;
    uint64_t map_off, data_off;
    uint32_t *map;
    uint8_t *tmp;
    pthread_mutex_t lock;
} vdi;

static uint64_t block_off(const vdi *v, uint32_t idx) { return v->data_off + (uint64_t)idx * (v->bsize + v->extra) + v->extra; }

static int vdi_read(void *st, uint64_t off, void *buf, size_t len)
{
    vdi *v = st;
    uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&v->lock);
    while (len) {
        uint64_t bi = off / v->bsize, in = off % v->bsize;
        size_t n = v->bsize - in < len ? (size_t)(v->bsize - in) : len;
        uint32_t e = bi < v->nblocks ? v->map[bi] : VDI_UNALLOCATED;
        if (off >= v->vsize || e >= VDI_ZERO)
            memset(p, 0, n);
        else if (img_pread(v->fd, p, n, block_off(v, e) + in) < 0) {
            r = -1;
            break;
        }
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&v->lock);
    return r;
}

static int write_in_block(vdi *v, uint32_t bi, uint64_t in, const uint8_t *buf, size_t len)
{
    uint32_t e = v->map[bi];
    if (e < VDI_ZERO)
        return img_pwrite(v->fd, buf, len, block_off(v, e) + in);
    if (img_all_zero(buf, len))
        return 0; /* bloco continua zerado */
    /* bloco novo no fim: dados extras (zerados) + bloco inteiro de uma vez */
    size_t total = v->extra + v->bsize;
    memset(v->tmp, 0, total);
    memcpy(v->tmp + v->extra + in, buf, len);
    uint32_t idx = v->nalloc;
    uint8_t b[4];
    if (img_pwrite(v->fd, v->tmp, total, block_off(v, idx) - v->extra) < 0)
        return -1;
    wr32le(b, idx);
    if (img_pwrite(v->fd, b, 4, v->map_off + 4ULL * bi) < 0)
        return -1;
    v->map[bi] = idx;
    v->nalloc++;
    wr32le(b, v->nalloc);
    return img_pwrite(v->fd, b, 4, 0x184);
}

static int vdi_write(void *st, uint64_t off, const void *buf, size_t len)
{
    vdi *v = st;
    const uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&v->lock);
    while (len) {
        uint64_t bi = off / v->bsize, in = off % v->bsize;
        size_t n = v->bsize - in < len ? (size_t)(v->bsize - in) : len;
        if (off >= v->vsize || bi >= v->nblocks) {
            errno = ENOSPC;
            r = -1;
            break;
        }
        if (write_in_block(v, (uint32_t)bi, in, p, n) < 0) {
            r = -1;
            break;
        }
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&v->lock);
    return r;
}

static uint64_t vdi_zero_span(void *st, uint64_t off)
{
    vdi *v = st;
    uint64_t bi = off / v->bsize;
    if (off >= v->vsize || bi >= v->nblocks || v->map[bi] < VDI_ZERO)
        return 0;
    return v->bsize - off % v->bsize;
}

static int vdi_flush(void *st)
{
    vdi *v = st;
    return v->ro ? 0 : fdatasync(v->fd);
}

static void vdi_close(void *st)
{
    vdi *v = st;
    if (!v)
        return;
    free(v->map);
    free(v->tmp);
    pthread_mutex_destroy(&v->lock);
    free(v);
}

static bool vdi_probe(const img_probe_info *p)
{
    return rd32le(p->head + 0x40) == VDI_SIGNATURE;
}

#define FAIL(...)                           \
    do {                                    \
        snprintf(err, errlen, __VA_ARGS__); \
        vdi_close(v);                       \
        return NULL;                        \
    } while (0)

static void *vdi_open(img_open_args *a, char *err, size_t errlen)
{
    uint8_t h[0x200];
    vdi *v = calloc(1, sizeof(*v));
    if (!v) {
        snprintf(err, errlen, "sem memoria");
        return NULL;
    }
    pthread_mutex_init(&v->lock, NULL);
    v->fd = a->fd;
    v->ro = a->readonly;
    if (img_pread(v->fd, h, sizeof(h), 0) < 0)
        FAIL("vdi: erro ao ler o cabecalho: %s", strerror(errno));
    if (rd32le(h + 0x44) != 0x00010001u)
        FAIL("vdi: versao 0x%08x nao suportada (so 1.1)", rd32le(h + 0x44));
    uint32_t type = rd32le(h + 0x4c);
    if (type == 3 || type == 4)
        FAIL("vdi: imagens diferenciais (snapshots do VirtualBox) nao sao suportadas; "
             "clone o disco no VirtualBox ou converta-o para uma imagem independente");
    if (type != 1 && type != 2)
        FAIL("vdi: tipo de imagem %u desconhecido", type);
    v->map_off = rd32le(h + 0x154);
    v->data_off = rd32le(h + 0x158);
    if (rd32le(h + 0x168) != 512)
        FAIL("vdi: setor de %u bytes nao suportado", rd32le(h + 0x168));
    v->vsize = rd64le(h + 0x170);
    v->bsize = rd32le(h + 0x178);
    v->extra = rd32le(h + 0x17c);
    v->nblocks = rd32le(h + 0x180);
    v->nalloc = rd32le(h + 0x184);
    if (v->bsize < 512 || (v->bsize & 511) || v->bsize > (64u << 20) || v->extra > (1u << 20) || (v->extra & 511))
        FAIL("vdi: tamanho de bloco invalido");
    if ((uint64_t)v->nblocks * v->bsize < v->vsize || v->nblocks > (256u << 20) / 4 || v->nalloc > v->nblocks)
        FAIL("vdi: mapa de blocos invalido");
    v->map = malloc((size_t)v->nblocks * 4 + 4);
    v->tmp = malloc((size_t)v->bsize + v->extra);
    if (!v->map || !v->tmp)
        FAIL("vdi: sem memoria");
    if (img_pread(v->fd, v->map, (size_t)v->nblocks * 4, v->map_off) < 0)
        FAIL("vdi: erro ao ler o mapa: %s", strerror(errno));
    for (uint32_t i = 0; i < v->nblocks; i++) {
        v->map[i] = rd32le((const uint8_t *)&v->map[i]);
        if (v->map[i] < VDI_ZERO && v->map[i] >= v->nalloc)
            FAIL("vdi: mapa de blocos corrompido (bloco %u)", i);
    }
    a->readonly = v->ro;
    a->vsize = v->vsize;
    return v;
}

const blk_driver img_vdi = {
    .name = "vdi",
    .probe = vdi_probe,
    .open = vdi_open,
    .read = vdi_read,
    .write = vdi_write,
    .flush = vdi_flush,
    .zero_span = vdi_zero_span,
    .close = vdi_close,
};
