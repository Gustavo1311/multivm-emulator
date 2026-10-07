/*
 * VMDK (VMware): descritor de texto (arquivo separado ou embutido) com extents
 * SPARSE (formato hosted "KDMV": grain directory + grain tables), FLAT e ZERO.
 * Cobre monolithicSparse, monolithicFlat, twoGbMaxExtentSparse/Flat e
 * streamOptimized (graos comprimidos com zlib; somente leitura).
 *
 * Escrita em extents esparsos: grao novo no fim do arquivo do extent, depois a
 * entrada da grain table (principal e redundante). Recusado: discos filhos
 * (parentCID, snapshots do VMware) e extents ESX (VMFSSPARSE/COWD).
 */
#include "img.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KDMV 0x564d444bu
#define GD_AT_END 0xffffffffffffffffULL
#define MAX_EXTENTS 256

enum { EX_SPARSE, EX_FLAT, EX_ZERO };

typedef struct {
    int type;
    int fd;
    bool own_fd;       /* fd aberto aqui (fecha no fim) */
    bool ro;
    uint64_t start;    /* byte inicial no disco virtual */
    uint64_t size;     /* bytes */
    uint64_t flat_off; /* FLAT: offset em bytes no arquivo */
    /* SPARSE */
    bool compressed;
    uint64_t grain;    /* tamanho do grao em bytes */
    uint32_t gtes;     /* entradas por grain table */
    uint32_t gd_n;
    uint64_t gd_sector, rgd_sector;
    uint32_t *gd, *rgd;
    uint32_t **gt;     /* tabelas em cache (indice do GD) */
    uint64_t file_end;
    uint64_t zc_grain; /* grao descomprimido em zbuf (~0 = nenhum) */
    uint8_t *zbuf, *zin;
} extent;

typedef struct {
    bool ro;
    uint64_t vsize;
    int n;
    extent *ex;
    uint8_t *tmp;
    pthread_mutex_t lock;
} vmdk;

/* ---------------------------------------------------------- extent esparso */

static uint32_t *gt_get(extent *e, uint32_t gdi)
{
    if (e->gt[gdi])
        return e->gt[gdi];
    uint32_t *t = malloc((size_t)e->gtes * 4);
    if (!t)
        return NULL;
    if (!e->gd[gdi])
        memset(t, 0, (size_t)e->gtes * 4);
    else if (img_pread(e->fd, t, (size_t)e->gtes * 4, (uint64_t)e->gd[gdi] * 512) < 0) {
        free(t);
        return NULL;
    }
    for (uint32_t i = 0; i < e->gtes; i++)
        t[i] = rd32le((const uint8_t *)&t[i]);
    return e->gt[gdi] = t;
}

static int read_grain_compressed(extent *e, uint64_t gi, uint32_t sector)
{
    if (e->zc_grain == gi)
        return 0;
    uint8_t hdr[12];
    uint64_t off = (uint64_t)sector * 512;
    if (img_pread(e->fd, hdr, 12, off) < 0)
        return -1;
    uint32_t size = rd32le(hdr + 8);
    if (!size || size > 2 * e->grain + 1024) {
        errno = EIO;
        return -1;
    }
    if (img_pread(e->fd, e->zin, size, off + 12) < 0)
        return -1;
    long n = img_zlib_inflate(e->zin, size, e->zbuf, (size_t)e->grain);
    if (n < 0) {
        e->zc_grain = ~0ULL;
        errno = EIO;
        return -1;
    }
    memset(e->zbuf + n, 0, (size_t)e->grain - (size_t)n);
    e->zc_grain = gi;
    return 0;
}

/* le dentro de um grao; off relativo ao extent */
static int sparse_read(extent *e, uint64_t off, uint8_t *buf, size_t len)
{
    uint64_t gi = off / e->grain, in = off % e->grain;
    uint32_t gdi = (uint32_t)(gi / e->gtes);
    if (gdi >= e->gd_n) {
        memset(buf, 0, len);
        return 0;
    }
    uint32_t *gt = gt_get(e, gdi);
    if (!gt)
        return -1;
    uint32_t s = gt[gi % e->gtes];
    if (s <= 1) { /* 0: nao alocado, 1: grao zerado */
        memset(buf, 0, len);
        return 0;
    }
    if (e->compressed) {
        if (read_grain_compressed(e, gi, s) < 0)
            return -1;
        memcpy(buf, e->zbuf + in, len);
        return 0;
    }
    return img_pread(e->fd, buf, len, (uint64_t)s * 512 + in);
}

static int put_u32(int fd, uint64_t off, uint32_t v)
{
    uint8_t b[4];
    wr32le(b, v);
    return img_pwrite(fd, b, 4, off);
}

/* grain table zerada nova no fim do arquivo; devolve o setor */
static int new_table(extent *e, uint8_t *tmp, uint32_t *sector)
{
    size_t sz = ((size_t)e->gtes * 4 + 511) & ~(size_t)511;
    memset(tmp, 0, sz);
    if (e->file_end / 512 > 0xffffffffULL || img_pwrite(e->fd, tmp, sz, e->file_end) < 0)
        return -1;
    *sector = (uint32_t)(e->file_end / 512);
    e->file_end += sz;
    return 0;
}

static int sparse_write(vmdk *v, extent *e, uint64_t off, const uint8_t *buf, size_t len)
{
    uint64_t gi = off / e->grain, in = off % e->grain;
    uint32_t gdi = (uint32_t)(gi / e->gtes), gti = (uint32_t)(gi % e->gtes);
    if (gdi >= e->gd_n) {
        errno = ENOSPC;
        return -1;
    }
    uint32_t *gt = gt_get(e, gdi);
    if (!gt)
        return -1;
    uint32_t s = gt[gti];
    if (s > 1)
        return img_pwrite(e->fd, buf, len, (uint64_t)s * 512 + in);
    if (img_all_zero(buf, len))
        return 0;
    if (!e->gd[gdi]) { /* grain table ainda nao existe: zerada, depois a entrada do GD */
        uint32_t ts, rts = 0;
        if (new_table(e, v->tmp, &ts) < 0 || (e->rgd && new_table(e, v->tmp, &rts) < 0))
            return -1;
        if (put_u32(e->fd, e->gd_sector * 512 + 4ULL * gdi, ts) < 0)
            return -1;
        e->gd[gdi] = ts;
        if (e->rgd) {
            if (put_u32(e->fd, e->rgd_sector * 512 + 4ULL * gdi, rts) < 0)
                return -1;
            e->rgd[gdi] = rts;
        }
    }
    memset(v->tmp, 0, (size_t)e->grain);
    memcpy(v->tmp + in, buf, len);
    uint64_t at = e->file_end;
    if (at / 512 > 0xffffffffULL) {
        errno = EFBIG;
        return -1;
    }
    if (img_pwrite(e->fd, v->tmp, (size_t)e->grain, at) < 0)
        return -1;
    e->file_end += e->grain;
    uint32_t ns = (uint32_t)(at / 512);
    if (put_u32(e->fd, (uint64_t)e->gd[gdi] * 512 + 4ULL * gti, ns) < 0)
        return -1;
    if (e->rgd && e->rgd[gdi] && put_u32(e->fd, (uint64_t)e->rgd[gdi] * 512 + 4ULL * gti, ns) < 0)
        return -1;
    gt[gti] = ns;
    return 0;
}

/* ------------------------------------------------------------- despacho */

static extent *find_extent(vmdk *v, uint64_t off)
{
    int lo = 0, hi = v->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        extent *e = &v->ex[mid];
        if (off < e->start)
            hi = mid - 1;
        else if (off >= e->start + e->size)
            lo = mid + 1;
        else
            return e;
    }
    return NULL;
}

/* trecho [off, off+*n) dentro de um extent (e de um grao, se esparso) */
static extent *span(vmdk *v, uint64_t off, size_t len, size_t *n)
{
    extent *e = find_extent(v, off);
    uint64_t lim = e ? e->start + e->size - off : len;
    if (e && e->type == EX_SPARSE) {
        uint64_t g = e->grain - (off - e->start) % e->grain;
        if (g < lim)
            lim = g;
    }
    *n = lim < len ? (size_t)lim : len;
    return e;
}

static int vmdk_read(void *st, uint64_t off, void *buf, size_t len)
{
    vmdk *v = st;
    uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&v->lock);
    while (len && r == 0) {
        size_t n;
        extent *e = span(v, off, len, &n);
        if (!e || e->type == EX_ZERO)
            memset(p, 0, n);
        else if (e->type == EX_FLAT)
            r = img_pread(e->fd, p, n, e->flat_off + (off - e->start));
        else
            r = sparse_read(e, off - e->start, p, n);
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&v->lock);
    return r;
}

static int vmdk_write(void *st, uint64_t off, const void *buf, size_t len)
{
    vmdk *v = st;
    const uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&v->lock);
    while (len && r == 0) {
        size_t n;
        extent *e = span(v, off, len, &n);
        if (!e || e->ro || e->type == EX_ZERO || e->compressed) {
            errno = e ? EROFS : ENOSPC;
            r = -1;
            break;
        }
        if (e->type == EX_FLAT)
            r = img_pwrite(e->fd, p, n, e->flat_off + (off - e->start));
        else
            r = sparse_write(v, e, off - e->start, p, n);
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&v->lock);
    return r;
}

static uint64_t vmdk_zero_span(void *st, uint64_t off)
{
    vmdk *v = st;
    uint64_t z = 0;
    pthread_mutex_lock(&v->lock);
    extent *e = find_extent(v, off);
    if (e && e->type == EX_ZERO) {
        z = e->start + e->size - off;
    } else if (e && e->type == EX_SPARSE) {
        uint64_t rel = off - e->start, gi = rel / e->grain;
        uint32_t gdi = (uint32_t)(gi / e->gtes);
        uint32_t *gt = gdi < e->gd_n ? gt_get(e, gdi) : NULL;
        if (gt && gt[gi % e->gtes] <= 1)
            z = e->grain - rel % e->grain;
    }
    pthread_mutex_unlock(&v->lock);
    return z;
}

static int vmdk_flush(void *st)
{
    vmdk *v = st;
    if (v->ro)
        return 0;
    for (int i = 0; i < v->n; i++) {
        extent *e = &v->ex[i];
        if (e->type == EX_ZERO || e->ro)
            continue;
        bool seen = false; /* varios extents podem dividir o mesmo arquivo */
        for (int k = 0; k < i && !seen; k++)
            seen = v->ex[k].fd == e->fd && v->ex[k].type != EX_ZERO;
        if (!seen && fdatasync(e->fd) < 0)
            return -1;
    }
    return 0;
}

static void vmdk_close(void *st)
{
    vmdk *v = st;
    if (!v)
        return;
    for (int i = 0; i < v->n; i++) {
        extent *e = &v->ex[i];
        if (e->gt)
            for (uint32_t k = 0; k < e->gd_n; k++)
                free(e->gt[k]);
        free(e->gt);
        free(e->gd);
        free(e->rgd);
        free(e->zbuf);
        free(e->zin);
        if (e->own_fd)
            close(e->fd);
    }
    free(v->ex);
    free(v->tmp);
    pthread_mutex_destroy(&v->lock);
    free(v);
}

static bool vmdk_probe(const img_probe_info *p)
{
    return rd32le(p->head) == KDMV || !memcmp(p->head, "# Disk DescriptorFile", 21) ||
           !memcmp(p->head, "COWD", 4);
}

/* ------------------------------------------------------------- abertura */

/* abre o extent esparso KDMV no fd; sectors_out = capacidade */
static int sparse_open(extent *e, uint64_t fsize, char *err, size_t errlen)
{
    uint8_t h[512];
    if (img_pread(e->fd, h, 512, 0) < 0 || rd32le(h) != KDMV) {
        snprintf(err, errlen, "vmdk: extent esparso sem cabecalho KDMV");
        return -1;
    }
    uint32_t flags = rd32le(h + 8);
    uint64_t gd_off = rd64le(h + 56);
    if (gd_off == GD_AT_END) { /* streamOptimized: cabecalho valido no rodape */
        if (fsize < 1536 || img_pread(e->fd, h, 512, fsize - 1024) < 0 || rd32le(h) != KDMV) {
            snprintf(err, errlen, "vmdk: rodape do streamOptimized nao encontrado");
            return -1;
        }
        flags = rd32le(h + 8);
        gd_off = rd64le(h + 56);
    }
    uint32_t version = rd32le(h + 4);
    if (version < 1 || version > 3) {
        snprintf(err, errlen, "vmdk: versao %u do extent esparso nao suportada", version);
        return -1;
    }
    uint64_t capacity = rd64le(h + 12), grain = rd64le(h + 20);
    e->gtes = rd32le(h + 44);
    e->compressed = (flags >> 16) & 1;
    if (e->compressed && rd16le(h + 77) != 1) {
        snprintf(err, errlen, "vmdk: algoritmo de compressao %u nao suportado", rd16le(h + 77));
        return -1;
    }
    if (grain < 1 || grain > 2048 || (grain & (grain - 1)) || e->gtes < 1 || e->gtes > 65536 || capacity > (1ULL << 40)) {
        snprintf(err, errlen, "vmdk: cabecalho do extent esparso invalido");
        return -1;
    }
    e->grain = grain * 512;
    e->gd_sector = gd_off;
    e->rgd_sector = (flags & 2) ? rd64le(h + 48) : 0;
    uint64_t gts = (capacity + grain * e->gtes - 1) / (grain * e->gtes);
    if (gts > (16u << 20)) {
        snprintf(err, errlen, "vmdk: grain directory grande demais");
        return -1;
    }
    e->gd_n = (uint32_t)gts;
    e->gd = malloc((size_t)gts * 4 + 4);
    e->gt = calloc((size_t)gts + 1, sizeof(*e->gt));
    e->rgd = e->rgd_sector ? malloc((size_t)gts * 4 + 4) : NULL;
    if (!e->gd || !e->gt || (e->rgd_sector && !e->rgd)) {
        snprintf(err, errlen, "sem memoria");
        return -1;
    }
    if (img_pread(e->fd, e->gd, (size_t)gts * 4, gd_off * 512) < 0 ||
        (e->rgd && img_pread(e->fd, e->rgd, (size_t)gts * 4, e->rgd_sector * 512) < 0)) {
        snprintf(err, errlen, "vmdk: erro ao ler o grain directory: %s", strerror(errno));
        return -1;
    }
    for (uint64_t i = 0; i < gts; i++) {
        e->gd[i] = rd32le((const uint8_t *)&e->gd[i]);
        if (e->rgd)
            e->rgd[i] = rd32le((const uint8_t *)&e->rgd[i]);
    }
    if (e->compressed) {
        e->ro = true;
        e->zbuf = malloc((size_t)e->grain);
        e->zin = malloc((size_t)(2 * e->grain + 1024));
        e->zc_grain = ~0ULL;
        if (!e->zbuf || !e->zin) {
            snprintf(err, errlen, "sem memoria");
            return -1;
        }
    }
    e->file_end = (fsize + 511) & ~511ULL;
    e->size = capacity * 512;
    return 0;
}

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* le um campo "aspas" ou palavra; devolve o resto */
static char *field(char *p, char *out, size_t outlen)
{
    p = skip_ws(p);
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (o + 1 < outlen)
                out[o++] = *p;
            p++;
        }
        if (*p == '"')
            p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (o + 1 < outlen)
                out[o++] = *p;
            p++;
        }
    }
    out[o] = 0;
    return p;
}

#define FAIL(...)                           \
    do {                                    \
        snprintf(err, errlen, __VA_ARGS__); \
        goto fail;                          \
    } while (0)

static void *vmdk_open(img_open_args *a, char *err, size_t errlen)
{
    vmdk *v = calloc(1, sizeof(*v));
    char *desc = NULL;
    if (!v) {
        snprintf(err, errlen, "sem memoria");
        return NULL;
    }
    pthread_mutex_init(&v->lock, NULL);
    v->ro = a->readonly;
    v->ex = calloc(MAX_EXTENTS, sizeof(extent));
    if (!v->ex)
        FAIL("sem memoria");
    uint8_t h[512];
    if (img_pread(a->fd, h, 512, 0) < 0)
        FAIL("vmdk: erro ao ler o arquivo: %s", strerror(errno));
    if (!memcmp(h, "COWD", 4))
        FAIL("vmdk: extents esparsos do ESX (COWD) nao suportados; converta com 'vmkfstools' ou 'qemu-img'");

    /* descritor: embutido no extent esparso ou o proprio arquivo */
    size_t dlen;
    bool embedded = rd32le(h) == KDMV;
    if (embedded) {
        uint64_t doff = rd64le(h + 28), dsz = rd64le(h + 36);
        if (!doff || !dsz) { /* esparso sem descritor: um extent so */
            dlen = 0;
        } else {
            if (dsz > 2048)
                FAIL("vmdk: descritor embutido grande demais");
            dlen = (size_t)dsz * 512;
            desc = calloc(1, dlen + 1);
            if (!desc || img_pread(a->fd, desc, dlen, doff * 512) < 0)
                FAIL("vmdk: erro ao ler o descritor");
        }
    } else {
        if (a->fsize > (1u << 20))
            FAIL("vmdk: descritor grande demais");
        dlen = (size_t)a->fsize;
        desc = calloc(1, dlen + 1);
        if (!desc || img_pread(a->fd, desc, dlen, 0) < 0)
            FAIL("vmdk: erro ao ler o descritor");
    }

    uint64_t pos = 0;
    if (!desc) {
        extent *e = &v->ex[v->n++];
        e->type = EX_SPARSE;
        e->fd = a->fd;
        e->ro = v->ro;
        if (sparse_open(e, a->fsize, err, errlen) < 0)
            goto fail;
        pos = e->size;
    }
    for (char *line = desc; line && *line;) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = 0;
        char *p = skip_ws(line);
        if (!strncmp(p, "parentCID", 9)) {
            char *q = strchr(p, '=');
            if (q && strtoul(skip_ws(q + 1), NULL, 16) != 0xffffffffUL)
                FAIL("vmdk: discos filhos (snapshots do VMware, parentCID) nao sao suportados; "
                     "consolide os snapshots ou converta o disco");
        } else if (!strncmp(p, "RW ", 3) || !strncmp(p, "RDONLY ", 7) || !strncmp(p, "NOACCESS ", 9)) {
            char acc[16], sz[32], type[32], name[1024] = "", offs[32] = "";
            p = field(p, acc, sizeof(acc));
            p = field(p, sz, sizeof(sz));
            p = field(p, type, sizeof(type));
            p = field(p, name, sizeof(name));
            field(p, offs, sizeof(offs));
            if (v->n >= MAX_EXTENTS)
                FAIL("vmdk: extents demais");
            extent *e = &v->ex[v->n];
            uint64_t sectors = strtoull(sz, NULL, 10);
            e->start = pos;
            e->ro = v->ro || strcmp(acc, "RW") != 0;
            if (!strcmp(type, "ZERO") || !strcmp(acc, "NOACCESS")) {
                e->type = EX_ZERO;
            } else if (!strcmp(type, "FLAT") || !strcmp(type, "VMFS") || !strcmp(type, "SPARSE")) {
                e->type = strcmp(type, "SPARSE") ? EX_FLAT : EX_SPARSE;
                if (embedded) {
                    e->fd = a->fd; /* monolithicSparse: o extent e o proprio arquivo */
                } else {
                    if (!a->path)
                        FAIL("vmdk: o descritor aponta para o arquivo '%s', que nao pode ser aberto pelo "
                             "seletor de arquivos; abra a imagem pelo caminho ou converta-a", name);
                    char path[4096];
                    img_rel_path(a->path, name, path, sizeof(path));
                    e->fd = open(path, e->ro ? O_RDONLY : O_RDWR);
                    if (e->fd < 0 && !e->ro && (errno == EACCES || errno == EROFS)) {
                        e->fd = open(path, O_RDONLY);
                        e->ro = v->ro = true;
                    }
                    if (e->fd < 0)
                        FAIL("vmdk: extent '%s': %s", path, strerror(errno));
                    e->own_fd = true;
                }
                if (e->type == EX_SPARSE) {
                    if (sparse_open(e, img_file_size(e->fd), err, errlen) < 0) {
                        v->n++;
                        goto fail;
                    }
                    if (e->compressed)
                        v->ro = true;
                } else {
                    e->flat_off = strtoull(offs, NULL, 10) * 512;
                }
            } else {
                FAIL("vmdk: tipo de extent '%s' nao suportado", type);
            }
            e->size = sectors * 512;
            pos += e->size;
            v->n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!v->n)
        FAIL("vmdk: nenhum extent no descritor");
    size_t maxg = 65536;
    for (int i = 0; i < v->n; i++) {
        if (v->ex[i].type == EX_SPARSE && v->ex[i].grain > maxg)
            maxg = (size_t)v->ex[i].grain;
        if (v->ex[i].ro && v->ex[i].type != EX_ZERO)
            v->ro = true;
    }
    v->tmp = malloc(maxg);
    if (!v->tmp)
        FAIL("sem memoria");
    v->vsize = pos;
    free(desc);
    a->readonly = v->ro;
    a->vsize = v->vsize;
    return v;
fail:
    free(desc);
    vmdk_close(v);
    return NULL;
}

const blk_driver img_vmdk = {
    .name = "vmdk",
    .probe = vmdk_probe,
    .open = vmdk_open,
    .read = vmdk_read,
    .write = vmdk_write,
    .flush = vmdk_flush,
    .zero_span = vmdk_zero_span,
    .close = vmdk_close,
};
