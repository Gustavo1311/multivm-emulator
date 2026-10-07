/*
 * VHD (Virtual PC / Virtual Server / Hyper-V), leitura e escrita: discos fixos,
 * dinamicos e diferenciais (pai pelos "parent locators" W2ru/W2ku/MacX).
 *
 * Fixo: dados crus seguidos do rodape de 512 bytes. Dinamico: rodape (copia no
 * inicio), cabecalho "cxsparse", BAT (setores, big-endian) e blocos com um bitmap
 * de setores na frente. Bloco novo: grava bitmap+dados no lugar do rodape, move o
 * rodape para o novo fim e so entao a entrada da BAT.
 */
#include "img.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VHD_FIXED 2
#define VHD_DYNAMIC 3
#define VHD_DIFF 4
#define BAT_FREE 0xffffffffu

typedef struct {
    int fd;
    bool ro;
    int type;
    uint64_t vsize;
    uint8_t footer[512];
    uint64_t file_end;   /* offset do rodape (fim dos dados) */
    uint64_t bat_off;
    uint32_t bat_n;
    uint32_t *bat;
    uint32_t bsize;
    uint32_t bm_size;    /* bitmap de setores, arredondado a 512 */
    uint8_t **bm;        /* bitmaps em cache (so diferencial) */
    uint8_t *tmp;
    mvm_blk *parent;
    pthread_mutex_t lock;
} vhd;

static uint32_t vhd_checksum(const uint8_t *b, size_t n, size_t skip)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++)
        if (i < skip || i >= skip + 4)
            s += b[i];
    return ~s;
}

static bool vhd_probe(const img_probe_info *p)
{
    return !memcmp(p->tail, "conectix", 8) || (!memcmp(p->head, "conectix", 8) && rd32be(p->head + 60) != VHD_FIXED);
}

static uint64_t data_off(const vhd *v, uint32_t bi) { return (uint64_t)v->bat[bi] * 512 + v->bm_size; }

/* bitmap do bloco (diferencial), lido sob demanda */
static uint8_t *bitmap(vhd *v, uint32_t bi)
{
    if (v->bm[bi])
        return v->bm[bi];
    uint8_t *b = malloc(v->bm_size);
    if (!b)
        return NULL;
    if (img_pread(v->fd, b, v->bm_size, (uint64_t)v->bat[bi] * 512) < 0) {
        free(b);
        return NULL;
    }
    return v->bm[bi] = b;
}

static int read_parent(vhd *v, uint64_t g, uint8_t *buf, size_t len)
{
    if (!v->parent || g >= v->parent->size) {
        memset(buf, 0, len);
        return 0;
    }
    size_t n = len;
    if (g + n > v->parent->size) {
        n = (size_t)(v->parent->size - g);
        memset(buf + n, 0, len - n);
    }
    return blk_read(v->parent, g, buf, n);
}

static int read_in_block(vhd *v, uint32_t bi, uint64_t in, uint8_t *buf, size_t len)
{
    uint64_t g = (uint64_t)bi * v->bsize + in;
    if (v->bat[bi] == BAT_FREE)
        return v->type == VHD_DIFF ? read_parent(v, g, buf, len) : (memset(buf, 0, len), 0);
    if (v->type != VHD_DIFF)
        return img_pread(v->fd, buf, len, data_off(v, bi) + in);
    uint8_t *bm = bitmap(v, bi);
    if (!bm)
        return -1;
    /* trechos de setores com o mesmo bit: do arquivo (1) ou do pai (0) */
    while (len) {
        uint64_t s = in / 512;
        bool mine = (bm[s / 8] >> (7 - s % 8)) & 1;
        size_t n = 512 - (size_t)(in % 512);
        while (n < len) {
            uint64_t s2 = (in + n) / 512;
            if ((((bm[s2 / 8] >> (7 - s2 % 8)) & 1) != 0) != mine)
                break;
            n += 512;
        }
        if (n > len)
            n = len;
        int r = mine ? img_pread(v->fd, buf, n, data_off(v, bi) + in) : read_parent(v, (uint64_t)bi * v->bsize + in, buf, n);
        if (r < 0)
            return -1;
        buf += n;
        in += n;
        len -= n;
    }
    return 0;
}

static int vhd_read(void *st, uint64_t off, void *buf, size_t len)
{
    vhd *v = st;
    uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&v->lock);
    if (v->type == VHD_FIXED) {
        size_t n = off >= v->vsize ? 0 : (v->vsize - off < len ? (size_t)(v->vsize - off) : len);
        r = img_pread(v->fd, p, n, off);
        memset(p + n, 0, len - n);
        pthread_mutex_unlock(&v->lock);
        return r;
    }
    while (len) {
        uint64_t bi = off / v->bsize, in = off % v->bsize;
        size_t n = v->bsize - in < len ? (size_t)(v->bsize - in) : len;
        if (off >= v->vsize || bi >= v->bat_n)
            memset(p, 0, n);
        else if (read_in_block(v, (uint32_t)bi, in, p, n) < 0) {
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

static int write_in_block(vhd *v, uint32_t bi, uint64_t in, const uint8_t *buf, size_t len)
{
    if (v->bat[bi] != BAT_FREE) {
        if (img_pwrite(v->fd, buf, len, data_off(v, bi) + in) < 0)
            return -1;
        if (v->type != VHD_DIFF)
            return 0;
        uint8_t *bm = bitmap(v, bi);
        if (!bm)
            return -1;
        uint64_t s0 = in / 512, s1 = (in + len - 1) / 512;
        bool changed = false;
        for (uint64_t s = s0; s <= s1; s++) {
            if (!((bm[s / 8] >> (7 - s % 8)) & 1)) {
                bm[s / 8] |= (uint8_t)(0x80 >> (s % 8));
                changed = true;
            }
        }
        if (!changed)
            return 0;
        uint64_t b0 = s0 / 8 & ~511ULL, b1 = (s1 / 8 + 512) & ~511ULL;
        return img_pwrite(v->fd, bm + b0, (size_t)(b1 - b0), (uint64_t)v->bat[bi] * 512 + b0);
    }
    if (v->type != VHD_DIFF && img_all_zero(buf, len))
        return 0;
    /* bloco novo no lugar do rodape: bitmap cheio + bloco inteiro */
    uint8_t *t = v->tmp;
    memset(t, 0xff, v->bm_size);
    if (v->type == VHD_DIFF) {
        if (read_parent(v, (uint64_t)bi * v->bsize, t + v->bm_size, v->bsize) < 0)
            return -1;
    } else {
        memset(t + v->bm_size, 0, v->bsize);
    }
    memcpy(t + v->bm_size + in, buf, len);
    uint64_t at = v->file_end;
    uint64_t total = v->bm_size + (uint64_t)v->bsize;
    if (img_pwrite(v->fd, t, (size_t)total, at) < 0 || img_pwrite(v->fd, v->footer, 512, at + total) < 0)
        return -1;
    uint8_t b[4];
    wr32be(b, (uint32_t)(at / 512));
    if (img_pwrite(v->fd, b, 4, v->bat_off + 4ULL * bi) < 0)
        return -1;
    v->bat[bi] = (uint32_t)(at / 512);
    v->file_end = at + total;
    return 0;
}

static int vhd_write(void *st, uint64_t off, const void *buf, size_t len)
{
    vhd *v = st;
    const uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&v->lock);
    if (off + len > v->vsize) {
        errno = ENOSPC;
        r = -1;
    } else if (v->type == VHD_FIXED) {
        r = img_pwrite(v->fd, p, len, off);
        len = 0;
    }
    while (r == 0 && len) {
        uint64_t bi = off / v->bsize, in = off % v->bsize;
        size_t n = v->bsize - in < len ? (size_t)(v->bsize - in) : len;
        if (bi >= v->bat_n || write_in_block(v, (uint32_t)bi, in, p, n) < 0) {
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

static uint64_t vhd_zero_span(void *st, uint64_t off)
{
    vhd *v = st;
    if (v->type != VHD_DYNAMIC || off >= v->vsize)
        return 0;
    uint64_t bi = off / v->bsize;
    if (bi >= v->bat_n || v->bat[bi] != BAT_FREE)
        return 0;
    return v->bsize - off % v->bsize;
}

static int vhd_flush(void *st)
{
    vhd *v = st;
    return v->ro ? 0 : fdatasync(v->fd);
}

static void vhd_close(void *st)
{
    vhd *v = st;
    if (!v)
        return;
    if (v->bm)
        for (uint32_t i = 0; i < v->bat_n; i++)
            free(v->bm[i]);
    free(v->bm);
    free(v->bat);
    free(v->tmp);
    blk_close(v->parent);
    pthread_mutex_destroy(&v->lock);
    free(v);
}

/* caminho do pai a partir dos parent locators (UTF-16LE ou URL UTF-8) */
static int parent_path(vhd *v, const uint8_t *dh, char *out, size_t outlen)
{
    static const uint32_t prefer[] = {0x57327275 /* W2ru */, 0x57326b75 /* W2ku */, 0x4d616358 /* MacX */};
    for (size_t k = 0; k < 3; k++) {
        for (int i = 0; i < 8; i++) {
            const uint8_t *e = dh + 576 + 24 * i;
            if (rd32be(e) != prefer[k])
                continue;
            uint32_t len = rd32be(e + 8);
            uint64_t off = rd64be(e + 16);
            uint8_t raw[2048];
            if (!len || len > sizeof(raw) || img_pread(v->fd, raw, len, off) < 0)
                continue;
            size_t o = 0;
            if (prefer[k] == 0x4d616358) {
                size_t skip = len >= 7 && !memcmp(raw, "file://", 7) ? 7 : 0;
                for (size_t j = skip; j < len && raw[j] && o + 1 < outlen; j++)
                    out[o++] = (char)raw[j];
            } else {
                for (uint32_t j = 0; j + 1 < len; j += 2) { /* UTF-16LE -> UTF-8 (BMP) */
                    unsigned c = raw[j] | (raw[j + 1] << 8);
                    if (!c)
                        break;
                    if (c == '\\')
                        c = '/';
                    if (c < 0x80 && o + 1 < outlen)
                        out[o++] = (char)c;
                    else if (c < 0x800 && o + 2 < outlen) {
                        out[o++] = (char)(0xc0 | (c >> 6));
                        out[o++] = (char)(0x80 | (c & 63));
                    } else if (o + 3 < outlen) {
                        out[o++] = (char)(0xe0 | (c >> 12));
                        out[o++] = (char)(0x80 | ((c >> 6) & 63));
                        out[o++] = (char)(0x80 | (c & 63));
                    }
                }
            }
            out[o] = 0;
            char *p = out;
            if (prefer[k] == 0x57327275 && p[0] == '.' && p[1] == '/')
                memmove(p, p + 2, strlen(p + 2) + 1);
            if (p[0] && p[1] == ':') { /* C:/...: caminho do Windows; procura o pai ao lado da imagem */
                char *base = strrchr(p, '/');
                if (base)
                    memmove(p, base + 1, strlen(base + 1) + 1);
            }
            if (out[0])
                return 0;
        }
    }
    return -1;
}

#define FAIL(...)                           \
    do {                                    \
        snprintf(err, errlen, __VA_ARGS__); \
        vhd_close(v);                       \
        return NULL;                        \
    } while (0)

static void *vhd_open(img_open_args *a, char *err, size_t errlen)
{
    vhd *v = calloc(1, sizeof(*v));
    if (!v) {
        snprintf(err, errlen, "sem memoria");
        return NULL;
    }
    pthread_mutex_init(&v->lock, NULL);
    v->fd = a->fd;
    v->ro = a->readonly;
    uint8_t *f = v->footer;
    bool at_end = a->fsize >= 512 && img_pread(v->fd, f, 512, a->fsize - 512) == 0 && !memcmp(f, "conectix", 8);
    if (!at_end) { /* rodape so na copia do inicio (arquivo truncado ou rodape de 511 bytes) */
        if (img_pread(v->fd, f, 512, 0) < 0 || memcmp(f, "conectix", 8))
            FAIL("vhd: rodape nao encontrado");
        if (!v->ro) {
            fprintf(stderr, "[mvm] vhd: rodape ausente no fim do arquivo; aberto somente leitura\n");
            v->ro = true;
        }
    }
    if (rd32be(f + 64) != vhd_checksum(f, 512, 64))
        fprintf(stderr, "[mvm] vhd: checksum do rodape nao confere (usando mesmo assim)\n");
    v->type = (int)rd32be(f + 60);
    v->vsize = rd64be(f + 48);
    v->file_end = at_end ? a->fsize - 512 : a->fsize;
    if (v->type == VHD_FIXED) {
        if (v->vsize > v->file_end)
            v->vsize = v->file_end;
        a->readonly = v->ro;
        a->vsize = v->vsize;
        return v;
    }
    if (v->type != VHD_DYNAMIC && v->type != VHD_DIFF)
        FAIL("vhd: tipo de disco %d desconhecido", v->type);
    uint8_t dh[1024];
    uint64_t dh_off = rd64be(f + 16);
    if (img_pread(v->fd, dh, sizeof(dh), dh_off) < 0 || memcmp(dh, "cxsparse", 8))
        FAIL("vhd: cabecalho dinamico invalido");
    v->bat_off = rd64be(dh + 16);
    v->bat_n = rd32be(dh + 28);
    v->bsize = rd32be(dh + 32);
    if (v->bsize < 512 || (v->bsize & (v->bsize - 1)) || v->bsize > (256u << 20))
        FAIL("vhd: tamanho de bloco invalido");
    if (v->bat_n > (64u << 20) / 4 || (uint64_t)v->bat_n * v->bsize < v->vsize)
        FAIL("vhd: BAT invalida");
    v->bm_size = ((v->bsize / 512 / 8) + 511) & ~511u;
    v->bat = malloc((size_t)v->bat_n * 4 + 4);
    v->tmp = malloc((size_t)v->bm_size + v->bsize);
    v->bm = calloc(v->bat_n + 1, sizeof(*v->bm));
    if (!v->bat || !v->tmp || !v->bm)
        FAIL("vhd: sem memoria");
    if (img_pread(v->fd, v->bat, (size_t)v->bat_n * 4, v->bat_off) < 0)
        FAIL("vhd: erro ao ler a BAT: %s", strerror(errno));
    for (uint32_t i = 0; i < v->bat_n; i++) {
        v->bat[i] = rd32be((const uint8_t *)&v->bat[i]);
        if (v->bat[i] != BAT_FREE && (uint64_t)v->bat[i] * 512 + v->bm_size + v->bsize > v->file_end + 512)
            FAIL("vhd: BAT aponta para fora do arquivo (bloco %u)", i);
    }
    if (v->type == VHD_DIFF) {
        char name[2048];
        if (parent_path(v, dh, name, sizeof(name)) < 0)
            FAIL("vhd: disco diferencial sem localizador do pai utilizavel");
        if (!(v->parent = img_open_backing(a, name, err, errlen))) {
            vhd_close(v);
            return NULL;
        }
    }
    a->readonly = v->ro;
    a->vsize = v->vsize;
    return v;
}

const blk_driver img_vhd = {
    .name = "vhd",
    .probe = vhd_probe,
    .open = vhd_open,
    .read = vhd_read,
    .write = vhd_write,
    .flush = vhd_flush,
    .zero_span = vhd_zero_span,
    .close = vhd_close,
};
