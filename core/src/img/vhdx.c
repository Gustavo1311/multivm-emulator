/*
 * VHDX (Hyper-V), leitura e escrita de discos fixos e dinamicos.
 *
 * - Dois cabecalhos (64 e 128 KiB) com CRC32C: vale o de maior SequenceNumber.
 * - Tabela de regioes (BAT e metadados); metadados: tamanho do bloco, tamanho
 *   virtual, tamanho do setor logico.
 * - BAT com uma entrada de bitmap de setores a cada "chunk ratio" blocos.
 * - Log: se o cabecalho tem LogGuid, a sequencia ativa do log e reaplicada
 *   (no arquivo, se gravavel; senao so na memoria) antes de qualquer acesso.
 * - Escrita: bloco novo alinhado a 1 MiB no fim do arquivo, depois a entrada
 *   da BAT (gravacao de 8 bytes, atomica no setor). Antes da primeira escrita
 *   o cabecalho ganha um DataWriteGuid novo, como pede a especificacao.
 * Recusado: discos diferenciais (HasParent).
 */
#include "img.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MiB (1ULL << 20)
#define PB_NOT_PRESENT 0
#define PB_UNDEFINED 1
#define PB_ZERO 2
#define PB_UNMAPPED 3
#define PB_FULLY_PRESENT 6
#define PB_PARTIALLY_PRESENT 7

typedef struct {
    uint64_t off;
    uint32_t len;
    uint8_t *data; /* NULL: zeros */
} patch;

typedef struct {
    int fd;
    bool ro;
    uint64_t vsize;
    uint32_t bsize;
    uint32_t lsec;
    uint64_t chunk;     /* blocos por bitmap de setores */
    uint64_t nblocks;
    uint64_t bat_off;
    uint64_t bat_n;
    uint64_t *bat;
    uint64_t file_end;
    uint8_t hdr[4096];  /* cabecalho atual */
    int hdr_slot;       /* 0: 64 KiB, 1: 128 KiB */
    bool header_updated;
    patch *patches;     /* log aplicado so na memoria (arquivo somente leitura) */
    int npatches;
    uint8_t *tmp;
    pthread_mutex_t lock;
} vhdx;

/* GUID em texto -> bytes no formato do arquivo (3 primeiros campos little-endian) */
static void guid(const char *s, uint8_t *g)
{
    unsigned v[16];
    sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[3], &v[2], &v[1], &v[0], &v[5], &v[4], &v[7],
           &v[6], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
    for (int i = 0; i < 16; i++)
        g[i] = (uint8_t)v[i];
}

static bool is_guid(const uint8_t *p, const char *s)
{
    uint8_t g[16];
    guid(s, g);
    return !memcmp(p, g, 16);
}

#define GUID_BAT "2DC27766-F623-4200-9D64-115E9BFD4A08"
#define GUID_META "8B7CA206-4790-4B9A-B8FE-575F050F886E"
#define GUID_FILE_PARAMS "CAA16737-FA36-4D43-B3B6-33F0AA44E76B"
#define GUID_DISK_SIZE "2FA54224-CD1B-4876-B211-5DBED83BF4B8"
#define GUID_LOGICAL_SECTOR "8141BF1D-A96F-4709-BA47-F233A8FAAB5F"

/* leitura do arquivo com o log aplicado na memoria por cima */
static int fread_at(vhdx *x, void *buf, size_t len, uint64_t off)
{
    if (img_pread(x->fd, buf, len, off) < 0)
        return -1;
    for (int i = 0; i < x->npatches; i++) {
        patch *p = &x->patches[i];
        uint64_t a = p->off > off ? p->off : off, b = p->off + p->len < off + len ? p->off + p->len : off + len;
        if (a >= b)
            continue;
        if (p->data)
            memcpy((uint8_t *)buf + (a - off), p->data + (a - p->off), (size_t)(b - a));
        else
            memset((uint8_t *)buf + (a - off), 0, (size_t)(b - a));
    }
    return 0;
}

static bool crc_ok(uint8_t *b, size_t n)
{
    uint32_t want = rd32le(b + 4);
    wr32le(b + 4, 0);
    uint32_t got = img_crc32c(0, b, n);
    wr32le(b + 4, want);
    return want == got;
}

/* ------------------------------------------------------------------ log */

static int apply(vhdx *x, uint64_t off, uint32_t len, const uint8_t *data)
{
    if (!x->ro) {
        static const uint8_t z[4096];
        for (uint32_t k = 0; k < len; k += 4096) {
            uint32_t n = len - k < 4096 ? len - k : 4096;
            if (img_pwrite(x->fd, data ? data + k : z, n, off + k) < 0)
                return -1;
        }
        return 0;
    }
    patch *np = realloc(x->patches, (size_t)(x->npatches + 1) * sizeof(*np));
    if (!np)
        return -1;
    x->patches = np;
    patch *p = &np[x->npatches++];
    p->off = off;
    p->len = len;
    p->data = NULL;
    if (data) {
        if (!(p->data = malloc(len)))
            return -1;
        memcpy(p->data, data, len);
    }
    return 0;
}

/* entrada do log valida em 'pos'? (cabecalho, GUID e CRC) */
static uint8_t *log_entry(vhdx *x, uint64_t log_off, uint32_t log_len, uint32_t pos, const uint8_t *lguid)
{
    uint8_t h[64];
    if (img_pread(x->fd, h, 64, log_off + pos) < 0 || memcmp(h, "loge", 4) || memcmp(h + 32, lguid, 16))
        return NULL;
    uint32_t elen = rd32le(h + 8);
    if (elen < 4096 || elen % 4096 || elen > log_len)
        return NULL;
    uint8_t *e = malloc(elen);
    if (!e)
        return NULL;
    for (uint32_t k = 0; k < elen; k += 4096) /* o log e circular */
        if (img_pread(x->fd, e + k, 4096, log_off + (pos + k) % log_len) < 0) {
            free(e);
            return NULL;
        }
    uint32_t ndesc = rd32le(e + 24);
    if (!crc_ok(e, elen) || 64 + 32ULL * ndesc > elen) {
        free(e);
        return NULL;
    }
    return e;
}

static int replay_entry(vhdx *x, const uint8_t *e)
{
    uint32_t ndesc = rd32le(e + 24);
    uint64_t seq = rd64le(e + 16);
    uint64_t dsec = (64 + 32ULL * ndesc + 4095) / 4096 * 4096; /* setores de dados */
    for (uint32_t i = 0; i < ndesc; i++) {
        const uint8_t *d = e + 64 + 32 * i;
        if (rd64le(d + 24) != seq)
            return -1;
        if (!memcmp(d, "zero", 4)) {
            uint64_t zl = rd64le(d + 8), fo = rd64le(d + 16);
            for (uint64_t k = 0; k < zl; k += 1u << 30)
                if (apply(x, fo + k, (uint32_t)(zl - k < (1u << 30) ? zl - k : (1u << 30)), NULL) < 0)
                    return -1;
        } else if (!memcmp(d, "desc", 4)) {
            const uint8_t *s = e + dsec;
            dsec += 4096;
            if (memcmp(s, "data", 4) || ((uint64_t)rd32le(s + 4) << 32 | rd32le(s + 4092)) != seq)
                return -1;
            uint8_t sec[4096];
            memcpy(sec, d + 8, 8);         /* LeadingBytes */
            memcpy(sec + 8, s + 8, 4084);
            memcpy(sec + 4092, d + 4, 4);  /* TrailingBytes */
            if (apply(x, rd64le(d + 16), 4096, sec) < 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int replay_log(vhdx *x, char *err, size_t errlen)
{
    const uint8_t *lguid = x->hdr + 48;
    uint32_t log_len = rd32le(x->hdr + 68);
    uint64_t log_off = rd64le(x->hdr + 72);
    if (!log_len || log_len % MiB || log_off % MiB) {
        snprintf(err, errlen, "vhdx: regiao de log invalida");
        return -1;
    }
    /* acha a sequencia valida mais longa com o maior numero de sequencia */
    uint64_t best_seq = 0;
    uint32_t best_start = 0, best_count = 0;
    for (uint32_t pos = 0; pos < log_len; pos += 4096) {
        uint8_t *e = log_entry(x, log_off, log_len, pos, lguid);
        if (!e)
            continue;
        uint64_t seq = rd64le(e + 16);
        uint32_t count = 0, p = pos;
        while (e && rd64le(e + 16) == seq + count && count < log_len / 4096) {
            count++;
            p = (p + rd32le(e + 8)) % log_len;
            free(e);
            e = log_entry(x, log_off, log_len, p, lguid);
        }
        free(e);
        if (count && seq + count - 1 > best_seq) {
            best_seq = seq + count - 1;
            best_start = pos;
            best_count = count;
        }
    }
    if (!best_count) { /* log vazio/invalido: nada a reaplicar */
        fprintf(stderr, "[mvm] vhdx: LogGuid definido, mas nenhuma entrada valida no log\n");
        return 0;
    }
    /* a cauda da ultima entrada diz onde a sequencia ativa comeca */
    uint32_t p = best_start;
    uint8_t *last = NULL;
    for (uint32_t i = 0; i < best_count; i++) {
        free(last);
        last = log_entry(x, log_off, log_len, p, lguid);
        if (!last)
            break;
        if (i + 1 < best_count)
            p = (p + rd32le(last + 8)) % log_len;
    }
    uint32_t tail = last ? rd32le(last + 12) : best_start;
    free(last);
    p = tail;
    for (uint32_t i = 0; i < best_count + 1; i++) {
        uint8_t *e = log_entry(x, log_off, log_len, p, lguid);
        if (!e)
            break;
        uint64_t seq = rd64le(e + 16);
        if (seq > best_seq) {
            free(e);
            break;
        }
        int r = replay_entry(x, e);
        uint32_t elen = rd32le(e + 8);
        free(e);
        if (r < 0) {
            snprintf(err, errlen, "vhdx: entrada do log corrompida");
            return -1;
        }
        if (seq == best_seq)
            break;
        p = (p + elen) % log_len;
    }
    if (!x->ro && fdatasync(x->fd) < 0) {
        snprintf(err, errlen, "vhdx: erro ao gravar o log reaplicado: %s", strerror(errno));
        return -1;
    }
    fprintf(stderr, "[mvm] vhdx: log reaplicado (%s)\n", x->ro ? "na memoria" : "no arquivo");
    return 0;
}

/* grava o cabecalho novo na outra posicao (sequencia + 1) */
static int write_header(vhdx *x)
{
    uint64_t seq = rd64le(x->hdr + 8) + 1;
    wr64le(x->hdr + 8, seq);
    wr32le(x->hdr + 4, 0);
    wr32le(x->hdr + 4, img_crc32c(0, x->hdr, 4096));
    int slot = !x->hdr_slot;
    if (img_pwrite(x->fd, x->hdr, 4096, slot ? 128 * 1024 : 64 * 1024) < 0 || fdatasync(x->fd) < 0)
        return -1;
    x->hdr_slot = slot;
    return 0;
}

static void random_guid(uint8_t *g)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, g, 16) != 16) {
        uint64_t t = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        for (int i = 0; i < 16; i++, t = t * 6364136223846793005ULL + 1442695040888963407ULL)
            g[i] = (uint8_t)(t >> 56);
    }
    if (fd >= 0)
        close(fd);
    g[7] = (uint8_t)((g[7] & 0x0f) | 0x40);
    g[8] = (uint8_t)((g[8] & 0x3f) | 0x80);
}

/* ------------------------------------------------------------- acesso */

static uint64_t bat_index(const vhdx *x, uint64_t bi) { return bi + bi / x->chunk; }

static int vhdx_read(void *st, uint64_t off, void *buf, size_t len)
{
    vhdx *x = st;
    uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&x->lock);
    while (len) {
        uint64_t bi = off / x->bsize, in = off % x->bsize;
        size_t n = x->bsize - in < len ? (size_t)(x->bsize - in) : len;
        uint64_t e = off < x->vsize ? x->bat[bat_index(x, bi)] : 0;
        if ((e & 7) == PB_FULLY_PRESENT) {
            if (fread_at(x, p, n, (e >> 20) * MiB + in) < 0) {
                r = -1;
                break;
            }
        } else {
            memset(p, 0, n);
        }
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&x->lock);
    return r;
}

static int write_in_block(vhdx *x, uint64_t bi, uint64_t in, const uint8_t *buf, size_t len)
{
    uint64_t bix = bat_index(x, bi), e = x->bat[bix];
    if ((e & 7) == PB_FULLY_PRESENT)
        return img_pwrite(x->fd, buf, len, (e >> 20) * MiB + in);
    if (img_all_zero(buf, len))
        return 0;
    if (!x->header_updated) { /* primeira escrita: DataWriteGuid novo */
        random_guid(x->hdr + 16);
        random_guid(x->hdr + 32);
        if (write_header(x) < 0)
            return -1;
        x->header_updated = true;
    }
    memset(x->tmp, 0, x->bsize);
    memcpy(x->tmp + in, buf, len);
    uint64_t at = x->file_end;
    if (img_pwrite(x->fd, x->tmp, x->bsize, at) < 0)
        return -1;
    x->file_end += (x->bsize + MiB - 1) & ~(MiB - 1);
    uint64_t ne = ((at / MiB) << 20) | PB_FULLY_PRESENT;
    uint8_t b[8];
    wr64le(b, ne);
    if (img_pwrite(x->fd, b, 8, x->bat_off + 8 * bix) < 0)
        return -1;
    x->bat[bix] = ne;
    return 0;
}

static int vhdx_write(void *st, uint64_t off, const void *buf, size_t len)
{
    vhdx *x = st;
    const uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&x->lock);
    while (len) {
        uint64_t bi = off / x->bsize, in = off % x->bsize;
        size_t n = x->bsize - in < len ? (size_t)(x->bsize - in) : len;
        if (off >= x->vsize) {
            errno = ENOSPC;
            r = -1;
            break;
        }
        if (write_in_block(x, bi, in, p, n) < 0) {
            r = -1;
            break;
        }
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&x->lock);
    return r;
}

static uint64_t vhdx_zero_span(void *st, uint64_t off)
{
    vhdx *x = st;
    if (off >= x->vsize || (x->bat[bat_index(x, off / x->bsize)] & 7) == PB_FULLY_PRESENT)
        return 0;
    return x->bsize - off % x->bsize;
}

static int vhdx_flush(void *st)
{
    vhdx *x = st;
    return x->ro ? 0 : fdatasync(x->fd);
}

static void vhdx_close(void *st)
{
    vhdx *x = st;
    if (!x)
        return;
    for (int i = 0; i < x->npatches; i++)
        free(x->patches[i].data);
    free(x->patches);
    free(x->bat);
    free(x->tmp);
    pthread_mutex_destroy(&x->lock);
    free(x);
}

static bool vhdx_probe(const img_probe_info *p) { return !memcmp(p->head, "vhdxfile", 8); }

#define FAIL(...)                           \
    do {                                    \
        snprintf(err, errlen, __VA_ARGS__); \
        vhdx_close(x);                      \
        return NULL;                        \
    } while (0)

static void *vhdx_open(img_open_args *a, char *err, size_t errlen)
{
    vhdx *x = calloc(1, sizeof(*x));
    if (!x) {
        snprintf(err, errlen, "sem memoria");
        return NULL;
    }
    pthread_mutex_init(&x->lock, NULL);
    x->fd = a->fd;
    x->ro = a->readonly;

    /* cabecalho valido de maior sequencia */
    uint8_t h[2][4096];
    bool ok[2];
    for (int i = 0; i < 2; i++)
        ok[i] = img_pread(x->fd, h[i], 4096, (uint64_t)(i + 1) * 64 * 1024) == 0 && !memcmp(h[i], "head", 4) &&
                crc_ok(h[i], 4096);
    if (!ok[0] && !ok[1])
        FAIL("vhdx: nenhum cabecalho valido");
    x->hdr_slot = !ok[0] || (ok[1] && rd64le(h[1] + 8) > rd64le(h[0] + 8));
    memcpy(x->hdr, h[x->hdr_slot], 4096);
    if (rd16le(x->hdr + 66) != 1)
        FAIL("vhdx: versao %u nao suportada", rd16le(x->hdr + 66));

    static const uint8_t zero16[16];
    if (memcmp(x->hdr + 48, zero16, 16)) {
        if (replay_log(x, err, errlen) < 0) {
            vhdx_close(x);
            return NULL;
        }
        if (!x->ro) { /* log aplicado: limpa o LogGuid */
            memset(x->hdr + 48, 0, 16);
            if (write_header(x) < 0)
                FAIL("vhdx: erro ao atualizar o cabecalho: %s", strerror(errno));
        }
    }

    /* tabela de regioes */
    uint8_t *rt = malloc(65536);
    if (!rt)
        FAIL("sem memoria");
    bool rt_ok = false;
    for (int i = 0; i < 2 && !rt_ok; i++)
        rt_ok = fread_at(x, rt, 65536, (uint64_t)(192 + 64 * i) * 1024) == 0 && !memcmp(rt, "regi", 4) && crc_ok(rt, 65536);
    uint64_t meta_off = 0;
    uint32_t meta_len = 0, bat_len = 0;
    if (rt_ok) {
        uint32_t n = rd32le(rt + 8);
        for (uint32_t i = 0; i < n && i < 2047; i++) {
            const uint8_t *e = rt + 16 + 32 * i;
            if (is_guid(e, GUID_BAT)) {
                x->bat_off = rd64le(e + 16);
                bat_len = rd32le(e + 24);
            } else if (is_guid(e, GUID_META)) {
                meta_off = rd64le(e + 16);
                meta_len = rd32le(e + 24);
            } else if (rd32le(e + 28) & 1) {
                free(rt);
                FAIL("vhdx: regiao obrigatoria desconhecida");
            }
        }
    }
    free(rt);
    if (!rt_ok || !x->bat_off || !meta_off || meta_len < 65536 || meta_len > (16u << 20))
        FAIL("vhdx: tabela de regioes invalida");

    /* metadados */
    uint8_t *m = malloc(meta_len);
    if (!m)
        FAIL("sem memoria");
    if (fread_at(x, m, meta_len, meta_off) < 0 || memcmp(m, "metadata", 8)) {
        free(m);
        FAIL("vhdx: regiao de metadados invalida");
    }
    uint32_t flags = 0;
    bool have_bs = false, have_size = false;
    x->lsec = 512;
    uint16_t nitems = rd16le(m + 10);
    for (uint16_t i = 0; i < nitems && 32 + 32 * (uint32_t)(i + 1) <= 65536; i++) {
        const uint8_t *e = m + 32 + 32 * i;
        uint32_t o = rd32le(e + 16), l = rd32le(e + 20);
        if ((uint64_t)o + l > meta_len)
            continue;
        if (is_guid(e, GUID_FILE_PARAMS) && l >= 8) {
            x->bsize = rd32le(m + o);
            flags = rd32le(m + o + 4);
            have_bs = true;
        } else if (is_guid(e, GUID_DISK_SIZE) && l >= 8) {
            x->vsize = rd64le(m + o);
            have_size = true;
        } else if (is_guid(e, GUID_LOGICAL_SECTOR) && l >= 4) {
            x->lsec = rd32le(m + o);
        }
    }
    free(m);
    if (!have_bs || !have_size)
        FAIL("vhdx: metadados obrigatorios ausentes");
    if (flags & 2)
        FAIL("vhdx: discos diferenciais nao sao suportados; mescle-o no Hyper-V ou converta-o");
    if (x->bsize < MiB || x->bsize > 256 * MiB || (x->bsize & (x->bsize - 1)))
        FAIL("vhdx: tamanho de bloco invalido");
    if (x->lsec != 512 && x->lsec != 4096)
        FAIL("vhdx: setor logico de %u bytes nao suportado", x->lsec);
    if (x->lsec == 4096)
        fprintf(stderr, "[mvm] vhdx: setor logico de 4 KiB; o convidado vera setores de 512 bytes\n");
    x->chunk = ((1ULL << 23) * x->lsec) / x->bsize;
    x->nblocks = (x->vsize + x->bsize - 1) / x->bsize;
    x->bat_n = x->nblocks + (x->nblocks ? (x->nblocks - 1) / x->chunk : 0);
    if (!x->chunk || x->bat_n * 8 > bat_len || x->bat_n > (64u << 20))
        FAIL("vhdx: BAT invalida");
    x->bat = malloc((size_t)x->bat_n * 8 + 8);
    x->tmp = malloc(x->bsize);
    if (!x->bat || !x->tmp)
        FAIL("vhdx: sem memoria");
    if (fread_at(x, x->bat, (size_t)x->bat_n * 8, x->bat_off) < 0)
        FAIL("vhdx: erro ao ler a BAT: %s", strerror(errno));
    uint64_t fsize = img_file_size(x->fd);
    for (uint64_t i = 0; i < x->bat_n; i++) {
        x->bat[i] = rd64le((const uint8_t *)&x->bat[i]);
        bool bitmap_entry = (i + 1) % (x->chunk + 1) == 0;
        unsigned s = x->bat[i] & 7;
        if (!bitmap_entry && s == PB_PARTIALLY_PRESENT)
            FAIL("vhdx: bloco parcialmente presente (disco diferencial)");
        if (!bitmap_entry && s == PB_FULLY_PRESENT && (x->bat[i] >> 20) * MiB + x->bsize > fsize + x->bsize)
            FAIL("vhdx: BAT aponta para fora do arquivo");
    }
    x->file_end = (fsize + MiB - 1) & ~(MiB - 1);
    a->readonly = x->ro;
    a->vsize = x->vsize;
    return x;
}

const blk_driver img_vhdx = {
    .name = "vhdx",
    .probe = vhdx_probe,
    .open = vhdx_open,
    .read = vhdx_read,
    .write = vhdx_write,
    .flush = vhdx_flush,
    .zero_span = vhdx_zero_span,
    .close = vhdx_close,
};
