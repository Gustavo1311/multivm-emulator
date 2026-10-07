/*
 * Imagens qcow2 (versoes 2 e 3), leitura e escrita, sem dependencias externas.
 *
 * Suportado: clusters de 512 B a 2 MiB, larguras de refcount de 1 a 64 bits,
 * clusters comprimidos (deflate; leitura e copia na escrita), clusters "zero" (v3),
 * snapshots internos (o estado ativo e usado; clusters compartilhados sao copiados
 * antes de escritos), arquivo base (backing file, de qualquer formato suportado).
 * Recusado com mensagem: criptografia, arquivo de dados externo, L2 estendido,
 * compressao zstd e imagens marcadas como corrompidas.
 *
 * Metadados sao gravados na hora (sem cache sujo), na ordem segura do QEMU sem
 * lazy refcounts: dados -> refcount -> L2 -> L1. Uma queda no meio pode vazar
 * clusters, mas nao corromper a imagem. Clusters novos sao sempre alocados no fim
 * do arquivo (clusters liberados nao sao reaproveitados).
 */
#include "img.h"
#include "../inflate.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define L2_CACHE 32
#define RB_CACHE 8

#define OFLAG_COPIED (1ULL << 63)
#define OFLAG_COMPRESSED (1ULL << 62)
#define OFLAG_ZERO 1ULL
#define OFFSET_MASK 0x00fffffffffffe00ULL

typedef struct {
    uint64_t off;   /* offset da tabela no arquivo (0 = livre) */
    uint64_t *e;    /* entradas em ordem do host */
    uint32_t lru;
} l2slot;

typedef struct {
    uint64_t off;
    uint8_t *d;     /* bloco cru (big-endian / bits empacotados) */
    uint32_t lru;
} rbslot;

struct qcow2 {
    int fd;
    bool ro;
    int version;
    unsigned cluster_bits;
    uint64_t csize;
    uint64_t vsize;
    uint64_t l2_entries;
    uint32_t l1_size;
    uint64_t l1_off;
    uint64_t *l1;
    uint64_t rt_off;
    uint64_t rt_entries;
    uint64_t *rt;
    unsigned rc_order;      /* refcount de 2^rc_order bits */
    uint64_t rc_per_block;
    uint64_t file_end;      /* proximo cluster livre (fim do arquivo, alinhado) */
    l2slot l2c[L2_CACHE];
    rbslot rbc[RB_CACHE];
    uint32_t clock;
    uint64_t zc_guest;      /* cluster do convidado descomprimido em zbuf (~0 = nenhum) */
    uint8_t *zbuf, *zin, *tmp;
    mvm_blk *backing;       /* arquivo base (clusters nao alocados vem dele) */
    pthread_mutex_t lock;
};

#define be64 rd64be
#define be32 rd32be
#define wbe64 wr64be
#define pread_full img_pread
#define pwrite_full img_pwrite

static int write_be64(struct qcow2 *q, uint64_t off, uint64_t v)
{
    uint8_t b[8];
    wbe64(b, v);
    return pwrite_full(q->fd, b, 8, off);
}

/* ------------------------------------------------------------ refcounts */

static uint8_t *rb_get(struct qcow2 *q, uint64_t off)
{
    rbslot *victim = &q->rbc[0];
    for (int i = 0; i < RB_CACHE; i++) {
        if (q->rbc[i].off == off) {
            q->rbc[i].lru = ++q->clock;
            return q->rbc[i].d;
        }
        if (q->rbc[i].lru < victim->lru)
            victim = &q->rbc[i];
    }
    if (!victim->d && !(victim->d = malloc(q->csize)))
        return NULL;
    if (pread_full(q->fd, victim->d, q->csize, off) < 0) {
        victim->off = 0;
        return NULL;
    }
    victim->off = off;
    victim->lru = ++q->clock;
    return victim->d;
}

static uint64_t rc_load(struct qcow2 *q, const uint8_t *d, uint64_t i)
{
    switch (q->rc_order) {
    case 0: return (d[i / 8] >> (i % 8)) & 1;
    case 1: return (d[i / 4] >> (2 * (i % 4))) & 3;
    case 2: return (d[i / 2] >> (4 * (i % 2))) & 15;
    case 3: return d[i];
    case 4: return ((uint64_t)d[2 * i] << 8) | d[2 * i + 1];
    case 5: return be32(d + 4 * i);
    default: return be64(d + 8 * i);
    }
}

/* grava o valor e devolve o trecho de bytes alterado (para gravar no arquivo) */
static void rc_store(struct qcow2 *q, uint8_t *d, uint64_t i, uint64_t v, uint64_t *byte, unsigned *len)
{
    switch (q->rc_order) {
    case 0: d[i / 8] = (uint8_t)((d[i / 8] & ~(1u << (i % 8))) | ((v & 1) << (i % 8))); *byte = i / 8; *len = 1; break;
    case 1: d[i / 4] = (uint8_t)((d[i / 4] & ~(3u << (2 * (i % 4)))) | ((v & 3) << (2 * (i % 4)))); *byte = i / 4; *len = 1; break;
    case 2: d[i / 2] = (uint8_t)((d[i / 2] & ~(15u << (4 * (i % 2)))) | ((v & 15) << (4 * (i % 2)))); *byte = i / 2; *len = 1; break;
    case 3: d[i] = (uint8_t)v; *byte = i; *len = 1; break;
    case 4: d[2 * i] = (uint8_t)(v >> 8); d[2 * i + 1] = (uint8_t)v; *byte = 2 * i; *len = 2; break;
    case 5: for (int k = 0; k < 4; k++) d[4 * i + (uint64_t)k] = (uint8_t)(v >> (24 - 8 * k)); *byte = 4 * i; *len = 4; break;
    default: wbe64(d + 8 * i, v); *byte = 8 * i; *len = 8; break;
    }
}

static int get_refcount(struct qcow2 *q, uint64_t host_off, uint64_t *out)
{
    uint64_t cl = host_off >> q->cluster_bits, bi = cl / q->rc_per_block;
    if (bi >= q->rt_entries || !(q->rt[bi] & OFFSET_MASK)) {
        *out = 0;
        return 0;
    }
    uint8_t *d = rb_get(q, q->rt[bi] & OFFSET_MASK);
    if (!d)
        return -1;
    *out = rc_load(q, d, cl % q->rc_per_block);
    return 0;
}

static int set_refcount(struct qcow2 *q, uint64_t host_off, uint64_t v);
static int add_refcount(struct qcow2 *q, uint64_t host_off, int delta);

/* novo cluster no fim do arquivo, zerado se zero = true, com refcount 1 */
static int alloc_cluster(struct qcow2 *q, bool zero, uint64_t *out)
{
    uint64_t off = q->file_end;
    q->file_end += q->csize;
    if (zero) {
        memset(q->tmp, 0, q->csize);
        if (pwrite_full(q->fd, q->tmp, q->csize, off) < 0)
            return -1;
    }
    if (set_refcount(q, off, 1) < 0)
        return -1;
    *out = off;
    return 0;
}

/*
 * Aumenta a tabela de refcount para cobrir o bloco 'need': grava uma tabela nova
 * (maior, com folga) no fim do arquivo, conta os clusters dela, troca o cabecalho
 * e so entao solta a tabela antiga. Uma queda antes da troca so vaza clusters.
 */
static int grow_refcount_table(struct qcow2 *q, uint64_t need)
{
    uint64_t per_cluster = q->csize / 8;
    uint64_t entries = (need + 1) * 2 + 16;
    uint64_t nclusters = (entries + per_cluster - 1) / per_cluster;
    for (;;) { /* a propria tabela e os blocos de refcount dela tambem precisam caber */
        uint64_t end = q->file_end + nclusters * q->csize;
        uint64_t blocks_needed = ((end >> q->cluster_bits) / q->rc_per_block) + 2;
        if (nclusters * per_cluster >= blocks_needed + nclusters + 1 && nclusters * per_cluster > need)
            break;
        nclusters++;
    }
    if (nclusters > 0xffffffffULL || nclusters * q->csize > (256ULL << 20)) {
        errno = EFBIG;
        return -1;
    }
    uint64_t new_entries = nclusters * per_cluster;
    uint64_t *nrt = calloc((size_t)new_entries + 1, 8);
    uint8_t *raw = calloc((size_t)(nclusters * q->csize), 1);
    if (!nrt || !raw) {
        free(nrt);
        free(raw);
        errno = ENOMEM;
        return -1;
    }
    memcpy(nrt, q->rt, (size_t)q->rt_entries * 8);
    for (uint64_t i = 0; i < q->rt_entries; i++)
        wbe64(raw + 8 * i, q->rt[i]);
    uint64_t noff = q->file_end;
    q->file_end += nclusters * q->csize;
    if (pwrite_full(q->fd, raw, (size_t)(nclusters * q->csize), noff) < 0) {
        free(nrt);
        free(raw);
        return -1;
    }
    free(raw);
    uint64_t old_off = q->rt_off, old_entries = q->rt_entries;
    uint64_t old_clusters = (old_entries * 8 + q->csize - 1) / q->csize;
    free(q->rt);
    q->rt = nrt;
    q->rt_entries = new_entries;
    q->rt_off = noff;
    for (uint64_t c = 0; c < nclusters; c++) /* blocos novos sao anotados na tabela nova */
        if (set_refcount(q, noff + c * q->csize, 1) < 0)
            return -1;
    uint8_t hdr[12];
    wbe64(hdr, noff);
    hdr[8] = (uint8_t)(nclusters >> 24);
    hdr[9] = (uint8_t)(nclusters >> 16);
    hdr[10] = (uint8_t)(nclusters >> 8);
    hdr[11] = (uint8_t)nclusters;
    if (fdatasync(q->fd) < 0 || pwrite_full(q->fd, hdr, 12, 48) < 0 || fdatasync(q->fd) < 0)
        return -1;
    for (uint64_t c = 0; c < old_clusters; c++)
        if (add_refcount(q, old_off + c * q->csize, -1) < 0)
            return -1;
    return 0;
}

static int set_refcount(struct qcow2 *q, uint64_t host_off, uint64_t v)
{
    uint64_t cl = host_off >> q->cluster_bits, bi = cl / q->rc_per_block;
    if (bi >= q->rt_entries && grow_refcount_table(q, bi) < 0)
        return -1;
    if (!(q->rt[bi] & OFFSET_MASK)) { /* novo bloco de refcount */
        uint64_t nb = q->file_end;
        q->file_end += q->csize;
        memset(q->tmp, 0, q->csize);
        if (pwrite_full(q->fd, q->tmp, q->csize, nb) < 0)
            return -1;
        q->rt[bi] = nb;
        if (write_be64(q, q->rt_off + 8 * bi, nb) < 0)
            return -1;
        if (set_refcount(q, nb, 1) < 0) /* o proprio bloco tambem e contado */
            return -1;
    }
    uint64_t boff = q->rt[bi] & OFFSET_MASK;
    uint8_t *d = rb_get(q, boff);
    if (!d)
        return -1;
    if (q->rc_order < 6 && (v >> (1u << q->rc_order))) {
        errno = EOVERFLOW;
        return -1;
    }
    uint64_t byte;
    unsigned len;
    rc_store(q, d, cl % q->rc_per_block, v, &byte, &len);
    return pwrite_full(q->fd, d + byte, len, boff + byte);
}

static int add_refcount(struct qcow2 *q, uint64_t host_off, int delta)
{
    uint64_t v;
    if (get_refcount(q, host_off, &v) < 0)
        return -1;
    if (delta < 0 && v == 0)
        return 0; /* inconsistente; nao piora */
    return set_refcount(q, host_off, delta < 0 ? v - 1 : v + 1);
}

/* ------------------------------------------------------------ tabelas L2 */

static uint64_t *l2_get(struct qcow2 *q, uint64_t off)
{
    l2slot *victim = &q->l2c[0];
    for (int i = 0; i < L2_CACHE; i++) {
        if (q->l2c[i].off == off) {
            q->l2c[i].lru = ++q->clock;
            return q->l2c[i].e;
        }
        if (q->l2c[i].lru < victim->lru)
            victim = &q->l2c[i];
    }
    if (!victim->e && !(victim->e = malloc(q->csize)))
        return NULL;
    if (pread_full(q->fd, victim->e, q->csize, off) < 0) {
        victim->off = 0;
        return NULL;
    }
    for (uint64_t i = 0; i < q->l2_entries; i++)
        victim->e[i] = be64((const uint8_t *)&victim->e[i]);
    victim->off = off;
    victim->lru = ++q->clock;
    return victim->e;
}

/* entrada L2 do cluster do convidado (0 se nao alocado) */
static int l2_lookup(struct qcow2 *q, uint64_t gcl, uint64_t *entry)
{
    uint64_t l1i = gcl / q->l2_entries;
    if (l1i >= q->l1_size || !(q->l1[l1i] & OFFSET_MASK)) {
        *entry = 0;
        return 0;
    }
    uint64_t *l2 = l2_get(q, q->l1[l1i] & OFFSET_MASK);
    if (!l2)
        return -1;
    *entry = l2[gcl % q->l2_entries];
    return 0;
}

/* ------------------------------------------------------------ leitura */

static void compressed_span(struct qcow2 *q, uint64_t e, uint64_t *off, uint64_t *len)
{
    unsigned x = 62 - (q->cluster_bits - 8);
    *off = e & ((1ULL << x) - 1);
    uint64_t sectors = ((e & ~OFLAG_COMPRESSED & ~OFLAG_COPIED) >> x) + 1;
    *len = sectors * 512 - (*off & 511);
}

/* conteudo inteiro do cluster comprimido em q->zbuf */
static int read_compressed(struct qcow2 *q, uint64_t gcl, uint64_t e)
{
    if (q->zc_guest == gcl)
        return 0;
    uint64_t off, len;
    compressed_span(q, e, &off, &len);
    if (len > 2 * q->csize)
        len = 2 * q->csize;
    if (pread_full(q->fd, q->zin, len, off) < 0)
        return -1;
    long n = mvm_inflate(q->zin, len, q->zbuf, q->csize);
    if (n < 0) {
        errno = EIO;
        return -1;
    }
    if ((uint64_t)n < q->csize)
        memset(q->zbuf + n, 0, q->csize - (uint64_t)n);
    q->zc_guest = gcl;
    return 0;
}

/* le [in, in+len) do cluster gcl (len <= tamanho do cluster) */
static int read_in_cluster(struct qcow2 *q, uint64_t gcl, uint64_t e, uint64_t in, uint8_t *buf, size_t len)
{
    if (e & OFLAG_COMPRESSED) {
        if (read_compressed(q, gcl, e) < 0)
            return -1;
        memcpy(buf, q->zbuf + in, len);
        return 0;
    }
    uint64_t host = e & OFFSET_MASK;
    if (q->version >= 3 && (e & OFLAG_ZERO)) {
        memset(buf, 0, len);
        return 0;
    }
    if (!host) {
        uint64_t g = (gcl << q->cluster_bits) + in;
        if (!q->backing || g >= q->backing->size) {
            memset(buf, 0, len);
            return 0;
        }
        size_t n = len;
        if (g + n > q->backing->size) {
            n = (size_t)(q->backing->size - g);
            memset(buf + n, 0, len - n);
        }
        return blk_read(q->backing, g, buf, n);
    }
    return pread_full(q->fd, buf, len, host + in);
}

static int qcow2_read(void *st, uint64_t off, void *buf, size_t len)
{
    struct qcow2 *q = st;
    uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&q->lock);
    while (len) {
        uint64_t gcl = off >> q->cluster_bits, in = off & (q->csize - 1);
        size_t n = (size_t)(q->csize - in);
        if (n > len)
            n = len;
        uint64_t e;
        if (off >= q->vsize) {
            memset(p, 0, n);
        } else if (l2_lookup(q, gcl, &e) < 0 || read_in_cluster(q, gcl, e, in, p, n) < 0) {
            r = -1;
            break;
        }
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&q->lock);
    return r;
}

static uint64_t qcow2_zero_span(void *st, uint64_t off)
{
    struct qcow2 *q = st;
    uint64_t e;
    if (off >= q->vsize)
        return 0;
    pthread_mutex_lock(&q->lock);
    int r = l2_lookup(q, off >> q->cluster_bits, &e);
    pthread_mutex_unlock(&q->lock);
    if (r < 0 || (e & OFLAG_COMPRESSED))
        return 0;
    bool zero = (q->version >= 3 && (e & OFLAG_ZERO)) || (!(e & OFFSET_MASK) && !q->backing);
    return zero ? q->csize - (off & (q->csize - 1)) : 0;
}

/* ------------------------------------------------------------ escrita */

/* tabela L2 gravavel (refcount 1) para o indice L1; cria ou copia se preciso */
static int l2_writable(struct qcow2 *q, uint64_t l1i, uint64_t **out, uint64_t *l2off)
{
    uint64_t ent = q->l1[l1i], old = ent & OFFSET_MASK;
    if (old && (ent & OFLAG_COPIED)) {
        *l2off = old;
        *out = l2_get(q, old);
        return *out ? 0 : -1;
    }
    uint64_t nw;
    if (alloc_cluster(q, !old, &nw) < 0)
        return -1;
    if (old) { /* tabela compartilhada com um snapshot: copia */
        if (pread_full(q->fd, q->tmp, q->csize, old) < 0 || pwrite_full(q->fd, q->tmp, q->csize, nw) < 0)
            return -1;
        if (add_refcount(q, old, -1) < 0)
            return -1;
    }
    q->l1[l1i] = nw | OFLAG_COPIED;
    if (write_be64(q, q->l1_off + 8 * l1i, q->l1[l1i]) < 0)
        return -1;
    for (int i = 0; i < L2_CACHE; i++) /* a copia pode estar em cache com o offset antigo */
        if (q->l2c[i].off == nw)
            q->l2c[i].off = 0;
    *l2off = nw;
    *out = l2_get(q, nw);
    return *out ? 0 : -1;
}

static int write_in_cluster(struct qcow2 *q, uint64_t gcl, uint64_t in, const uint8_t *buf, size_t len)
{
    uint64_t l1i = gcl / q->l2_entries, l2i = gcl % q->l2_entries;
    if (l1i >= q->l1_size) {
        errno = ENOSPC;
        return -1;
    }
    uint64_t *l2, l2off;
    if (l2_writable(q, l1i, &l2, &l2off) < 0)
        return -1;
    uint64_t e = l2[l2i];
    bool zero = q->version >= 3 && (e & OFLAG_ZERO);
    if (!(e & OFLAG_COMPRESSED) && (e & OFLAG_COPIED) && (e & OFFSET_MASK) && !zero)
        return pwrite_full(q->fd, buf, len, (e & OFFSET_MASK) + in); /* no lugar */

    /* copia na escrita para um cluster novo */
    if (len < q->csize) {
        if (read_in_cluster(q, gcl, e, 0, q->tmp, q->csize) < 0)
            return -1;
    }
    uint8_t *data = q->tmp;
    memcpy(data + in, buf, len);
    uint64_t nw = q->file_end;
    q->file_end += q->csize;
    if (pwrite_full(q->fd, data, q->csize, nw) < 0 || set_refcount(q, nw, 1) < 0)
        return -1;
    /* l2 pode ter saido do cache durante as alocacoes */
    l2 = l2_get(q, l2off);
    if (!l2)
        return -1;
    l2[l2i] = nw | OFLAG_COPIED;
    if (write_be64(q, l2off + 8 * l2i, l2[l2i]) < 0)
        return -1;
    /* solta a referencia ao conteudo antigo */
    if (e & OFLAG_COMPRESSED) {
        uint64_t off, clen;
        compressed_span(q, e, &off, &clen);
        for (uint64_t c = off & ~(q->csize - 1); c < off + clen; c += q->csize)
            if (add_refcount(q, c, -1) < 0)
                return -1;
        if (q->zc_guest == gcl)
            q->zc_guest = ~0ULL;
    } else if (e & OFFSET_MASK) {
        if (add_refcount(q, e & OFFSET_MASK, -1) < 0)
            return -1;
    }
    return 0;
}

static int qcow2_write(void *st, uint64_t off, const void *buf, size_t len)
{
    struct qcow2 *q = st;
    const uint8_t *p = buf;
    int r = 0;
    if (q->ro) {
        errno = EROFS;
        return -1;
    }
    pthread_mutex_lock(&q->lock);
    while (len) {
        if (off >= q->vsize) {
            r = -1;
            errno = ENOSPC;
            break;
        }
        uint64_t gcl = off >> q->cluster_bits, in = off & (q->csize - 1);
        size_t n = (size_t)(q->csize - in);
        if (n > len)
            n = len;
        if (write_in_cluster(q, gcl, in, p, n) < 0) {
            r = -1;
            break;
        }
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&q->lock);
    return r;
}

static int qcow2_flush(void *st)
{
    struct qcow2 *q = st;
    return q->ro ? 0 : fdatasync(q->fd);
}

/* ------------------------------------------------------------ abertura */

static void qcow2_close(void *st)
{
    struct qcow2 *q = st;
    if (!q)
        return;
    qcow2_flush(q);
    for (int i = 0; i < L2_CACHE; i++)
        free(q->l2c[i].e);
    for (int i = 0; i < RB_CACHE; i++)
        free(q->rbc[i].d);
    free(q->l1);
    free(q->rt);
    free(q->zbuf);
    free(q->zin);
    free(q->tmp);
    blk_close(q->backing);
    pthread_mutex_destroy(&q->lock);
    free(q);
}

static bool qcow2_probe(const img_probe_info *p)
{
    return p->head[0] == 'Q' && p->head[1] == 'F' && p->head[2] == 'I' && p->head[3] == 0xfb;
}

#define FAIL(...)                                    \
    do {                                             \
        snprintf(err, errlen, __VA_ARGS__);          \
        qcow2_close(q);                              \
        return NULL;                                 \
    } while (0)

static void *qcow2_open(img_open_args *a, char *err, size_t errlen)
{
    int fd = a->fd;
    uint8_t h[112] = {0};
    struct qcow2 *q = calloc(1, sizeof(*q));
    if (!q) {
        snprintf(err, errlen, "sem memoria");
        return NULL;
    }
    pthread_mutex_init(&q->lock, NULL);
    q->fd = fd;
    q->ro = a->readonly;
    q->zc_guest = ~0ULL;
    if (pread_full(fd, h, sizeof(h), 0) < 0)
        FAIL("qcow2: erro ao ler o cabecalho: %s", strerror(errno));
    q->version = (int)be32(h + 4);
    if (q->version != 2 && q->version != 3)
        FAIL("qcow2: versao %d nao suportada", q->version);
    q->cluster_bits = be32(h + 20);
    if (q->cluster_bits < 9 || q->cluster_bits > 21)
        FAIL("qcow2: tamanho de cluster invalido (2^%u)", q->cluster_bits);
    q->csize = 1ULL << q->cluster_bits;
    q->vsize = be64(h + 24);
    if (be32(h + 32))
        FAIL("qcow2: imagens criptografadas nao sao suportadas");
    q->l1_size = be32(h + 36);
    q->l1_off = be64(h + 40);
    q->rt_off = be64(h + 48);
    uint32_t rt_clusters = be32(h + 56);
    q->rc_order = 4;
    uint64_t incompat = 0, autoclear = 0;
    if (q->version >= 3) {
        incompat = be64(h + 72);
        autoclear = be64(h + 88);
        q->rc_order = be32(h + 96);
        uint32_t hlen = be32(h + 100);
        if (q->rc_order > 6)
            FAIL("qcow2: largura de refcount invalida");
        if (incompat & (1ULL << 1))
            FAIL("qcow2: a imagem esta marcada como corrompida; rode 'qemu-img check -r all'");
        if (incompat & (1ULL << 2))
            FAIL("qcow2: arquivo de dados externo nao suportado");
        if (incompat & (1ULL << 4))
            FAIL("qcow2: entradas L2 estendidas (subclusters) nao suportadas");
        if (incompat & (1ULL << 3)) {
            if (hlen > 104 && h[104] != 0)
                FAIL("qcow2: compressao zstd nao suportada (somente zlib/deflate)");
        }
        if (incompat & ~0x1fULL)
            FAIL("qcow2: recurso incompativel desconhecido (0x%llx)", (unsigned long long)incompat);
        if ((incompat & 1) && !q->ro) {
            fprintf(stderr, "[mvm] qcow2: imagem com estado sujo (lazy refcounts); aberta somente leitura. "
                            "Rode 'qemu-img check -r all' para repara-la.\n");
            q->ro = true;
        }
    }
    q->l2_entries = q->csize / 8;
    q->rc_per_block = q->csize * 8 >> q->rc_order;
    if (q->vsize > (uint64_t)q->l1_size * q->l2_entries * q->csize)
        FAIL("qcow2: tabela L1 menor que o disco virtual");
    if (q->l1_size > (32u << 20) / 8 || (uint64_t)rt_clusters * q->csize > (64u << 20))
        FAIL("qcow2: tabelas de metadados grandes demais");

    q->l1 = malloc((size_t)q->l1_size * 8 + 8);
    q->rt_entries = (uint64_t)rt_clusters * q->csize / 8;
    q->rt = malloc((size_t)q->rt_entries * 8 + 8);
    q->zbuf = malloc(q->csize);
    q->zin = malloc(2 * q->csize);
    q->tmp = malloc(q->csize);
    if (!q->l1 || !q->rt || !q->zbuf || !q->zin || !q->tmp)
        FAIL("sem memoria para os metadados qcow2");
    if (pread_full(fd, q->l1, (size_t)q->l1_size * 8, q->l1_off) < 0 ||
        pread_full(fd, q->rt, (size_t)q->rt_entries * 8, q->rt_off) < 0)
        FAIL("qcow2: erro ao ler as tabelas: %s", strerror(errno));
    for (uint32_t i = 0; i < q->l1_size; i++)
        q->l1[i] = be64((const uint8_t *)&q->l1[i]);
    for (uint64_t i = 0; i < q->rt_entries; i++)
        q->rt[i] = be64((const uint8_t *)&q->rt[i]);

    struct stat st;
    if (fstat(fd, &st) < 0)
        FAIL("qcow2: fstat: %s", strerror(errno));
    q->file_end = ((uint64_t)st.st_size + q->csize - 1) & ~(q->csize - 1);

    if (!q->ro && autoclear) { /* recursos "autoclear" desconhecidos ficam invalidos: limpa */
        if (write_be64(q, 88, 0) < 0)
            FAIL("qcow2: erro ao atualizar o cabecalho: %s", strerror(errno));
    }
    uint64_t bf_off = be64(h + 8);
    uint32_t bf_len = be32(h + 16);
    if (bf_off) {
        char name[1024];
        if (bf_len == 0 || bf_len >= sizeof(name))
            FAIL("qcow2: nome do arquivo base invalido");
        if (pread_full(fd, name, bf_len, bf_off) < 0)
            FAIL("qcow2: erro ao ler o nome do arquivo base: %s", strerror(errno));
        name[bf_len] = 0;
        if (!(q->backing = img_open_backing(a, name, err, errlen))) {
            qcow2_close(q);
            return NULL;
        }
    }
    a->readonly = q->ro;
    a->vsize = q->vsize;
    return q;
}

const blk_driver img_qcow2 = {
    .name = "qcow2",
    .probe = qcow2_probe,
    .open = qcow2_open,
    .read = qcow2_read,
    .write = qcow2_write,
    .flush = qcow2_flush,
    .zero_span = qcow2_zero_span,
    .close = qcow2_close,
};
