/*
 * MVD (MultiVM Disk): formato de imagem proprio, pensado para o armazenamento
 * do Android (FUSE/sdcardfs, sem arquivos esparsos, gravacoes pequenas caras).
 *
 *   [cabecalho 4 KiB][mapa: 1 entrada de 64 bits por bloco][blocos de dados]
 *
 * - Blocos grandes (64 KiB a 4 MiB, padrao 256 KiB) anexados em sequencia; o
 *   arquivo cresce conforme o uso.
 * - O mapa inteiro fica na RAM: leitura e escrita nunca leem metadados.
 * - Metadados em lote: entradas novas ficam sujas na RAM e sao gravadas no flush
 *   do convidado (ou a cada ~2 s de escrita), sempre DEPOIS de um fdatasync dos
 *   dados: o mapa nunca aponta para dados nao gravados.
 * - Blocos liberados (TRIM, blocos inteiros zerados) sao reaproveitados, mas so
 *   depois que o mapa que os libera estiver gravado.
 * - Blocos podem estar comprimidos com LZ4 (gerados na conversao); gravar num
 *   deles o reescreve descomprimido.
 * - Arquivo base opcional (overlay): blocos nao alocados vem dele.
 *
 * Entrada do mapa: bits 62-63 estado (0 nao alocado, 1 dados, 2 zero, 3 LZ4);
 * bits 0-39 offset no arquivo em unidades de 512 B; bits 40-61 tamanho
 * comprimido em bytes (so LZ4).
 */
#include "img.h"
#include "mvd.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MVD_MAGIC "MVMDISK"
#define MVD_VERSION 1
#define HDR_SIZE 4096
#define MAP_OFF 4096
#define NAME_OFF 64
#define NAME_MAX_LEN 1024
#define CRC_OFF 4092

#define ST_SHIFT 62
#define ST_NONE 0ULL
#define ST_DATA 1ULL
#define ST_ZERO 2ULL
#define ST_LZ4 3ULL
#define E_STATE(e) ((e) >> ST_SHIFT)
#define E_OFF(e) (((e) & ((1ULL << 40) - 1)) << 9)
#define E_CLEN(e) (((e) >> 40) & ((1ULL << 22) - 1))
#define MK_DATA(off) ((ST_DATA << ST_SHIFT) | ((off) >> 9))
#define MK_LZ4(off, clen) ((ST_LZ4 << ST_SHIFT) | ((uint64_t)(clen) << 40) | ((off) >> 9))
#define MK_ZERO (ST_ZERO << ST_SHIFT)

#define COMMIT_NS 2000000000LL

typedef struct {
    int fd;
    bool ro;
    uint64_t vsize;
    unsigned bbits;
    uint64_t bsize;
    uint64_t nblocks;
    uint64_t data_off;   /* inicio da area de blocos */
    uint64_t *map;
    uint8_t *dirty;      /* uma marca por pagina de 4 KiB do mapa */
    uint64_t ndirty;
    /* espacos (slots de bsize a partir de data_off) */
    uint64_t nslots;     /* slots ate o fim do arquivo */
    uint64_t *freel;     /* livres agora */
    uint64_t nfree, capfree;
    uint64_t *pend;      /* livres depois do proximo commit do mapa */
    uint64_t npend, cappend;
    uint64_t packed_end; /* fim da area de blocos LZ4 gravados na conversao */
    uint64_t fend;       /* fim do que ja foi gravado no arquivo (alem disso tudo e zero) */
    int64_t last_commit;
    uint64_t zc_block;   /* bloco descomprimido em zbuf (~0 = nenhum) */
    uint8_t *zbuf, *zin, *tmp;
    mvm_blk *backing;
    char backing_name[1024];
    pthread_mutex_t lock;
} mvd;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool push(uint64_t **v, uint64_t *n, uint64_t *cap, uint64_t x)
{
    if (*n == *cap) {
        uint64_t nc = *cap ? *cap * 2 : 64;
        uint64_t *nv = realloc(*v, nc * 8);
        if (!nv)
            return false;
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = x;
    return true;
}

static uint64_t slot_off(const mvd *m, uint64_t s) { return m->data_off + s * m->bsize; }

static void set_entry(mvd *m, uint64_t bi, uint64_t e)
{
    m->map[bi] = e;
    uint64_t pg = bi / 512;
    if (!m->dirty[pg]) {
        m->dirty[pg] = 1;
        m->ndirty++;
    }
}

/* solta o espaco de uma entrada (dados) para reuso apos o proximo commit */
static void release(mvd *m, uint64_t e)
{
    if (E_STATE(e) == ST_DATA) {
        uint64_t off = E_OFF(e);
        if (off >= m->data_off && !((off - m->data_off) & (m->bsize - 1)))
            push(&m->pend, &m->npend, &m->cappend, (off - m->data_off) >> m->bbits);
    }
    /* blocos LZ4 ficam empacotados: o espaco volta so com 'mvm-img compact' */
}

/* grava as paginas sujas do mapa; sync: fdatasync antes (dados) e depois */
static int commit(mvd *m, bool final_sync)
{
    if (m->ro)
        return 0;
    if (!m->ndirty && !m->npend)
        return final_sync ? fdatasync(m->fd) : 0;
    if (fdatasync(m->fd) < 0)
        return -1;
    uint64_t npages = (m->nblocks + 511) / 512;
    uint8_t page[4096];
    for (uint64_t pg = 0; pg < npages; pg++) {
        if (!m->dirty[pg])
            continue;
        uint64_t first = pg * 512, n = m->nblocks - first < 512 ? m->nblocks - first : 512;
        memset(page, 0, sizeof(page));
        for (uint64_t i = 0; i < n; i++)
            wr64le(page + 8 * i, m->map[first + i]);
        if (img_pwrite(m->fd, page, (size_t)(n * 8), MAP_OFF + first * 8) < 0)
            return -1;
        m->dirty[pg] = 0;
    }
    m->ndirty = 0;
    if (final_sync && fdatasync(m->fd) < 0)
        return -1;
    /* o mapa gravado nao referencia mais estes espacos */
    for (uint64_t i = 0; i < m->npend; i++)
        push(&m->freel, &m->nfree, &m->capfree, m->pend[i]);
    m->npend = 0;
    m->last_commit = now_ns();
    return 0;
}

/* --------------------------------------------------------------- leitura */

static int read_backing(mvd *m, uint64_t g, uint8_t *buf, size_t len)
{
    if (!m->backing || g >= m->backing->size) {
        memset(buf, 0, len);
        return 0;
    }
    size_t n = len;
    if (g + n > m->backing->size) {
        n = (size_t)(m->backing->size - g);
        memset(buf + n, 0, len - n);
    }
    return blk_read(m->backing, g, buf, n);
}

static int load_lz4(mvd *m, uint64_t bi, uint64_t e)
{
    if (m->zc_block == bi)
        return 0;
    uint64_t clen = E_CLEN(e);
    if (!clen || clen > m->bsize || img_pread(m->fd, m->zin, (size_t)clen, E_OFF(e)) < 0)
        goto bad;
    long n = lz4_decompress(m->zin, (size_t)clen, m->zbuf, (size_t)m->bsize);
    if (n != (long)m->bsize)
        goto bad;
    m->zc_block = bi;
    return 0;
bad:
    m->zc_block = ~0ULL;
    errno = EIO;
    return -1;
}

static int read_in_block(mvd *m, uint64_t bi, uint64_t in, uint8_t *buf, size_t len)
{
    uint64_t e = m->map[bi];
    switch (E_STATE(e)) {
    case ST_DATA: return img_pread(m->fd, buf, len, E_OFF(e) + in);
    case ST_ZERO: memset(buf, 0, len); return 0;
    case ST_LZ4:
        if (load_lz4(m, bi, e) < 0)
            return -1;
        memcpy(buf, m->zbuf + in, len);
        return 0;
    default: return read_backing(m, (bi << m->bbits) + in, buf, len);
    }
}

static int mvd_read(void *st, uint64_t off, void *buf, size_t len)
{
    mvd *m = st;
    uint8_t *p = buf;
    int r = 0;
    pthread_mutex_lock(&m->lock);
    while (len) {
        uint64_t bi = off >> m->bbits, in = off & (m->bsize - 1);
        size_t n = (size_t)(m->bsize - in);
        if (n > len)
            n = len;
        uint64_t e = off < m->vsize ? m->map[bi] : 0;
        if (off < m->vsize && E_STATE(e) == ST_DATA) {
            /* junta blocos seguintes que estao em sequencia no arquivo: um pread so */
            uint64_t next = E_OFF(e) + m->bsize;
            for (uint64_t b2 = bi + 1; n < len && b2 < m->nblocks && m->map[b2] == MK_DATA(next); b2++, next += m->bsize)
                n = len - n < m->bsize ? len : n + (size_t)m->bsize;
            if (img_pread(m->fd, p, n, E_OFF(e) + in) < 0) {
                r = -1;
                break;
            }
        } else if (off >= m->vsize)
            memset(p, 0, n);
        else if (read_in_block(m, bi, in, p, n) < 0) {
            r = -1;
            break;
        }
        p += n;
        off += n;
        len -= n;
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

/* --------------------------------------------------------------- escrita */

static uint64_t alloc_slot(mvd *m)
{
    if (m->nfree)
        return m->freel[--m->nfree];
    return m->nslots++;
}

/* entrada que representa "so zeros" para o bloco */
static uint64_t zero_entry(const mvd *m) { return m->backing ? MK_ZERO : 0; }

static void maybe_commit(mvd *m)
{
    if ((m->ndirty || m->npend) && now_ns() - m->last_commit > COMMIT_NS)
        commit(m, false);
}

static int write_in_block(mvd *m, uint64_t bi, uint64_t in, const uint8_t *buf, size_t len)
{
    uint64_t e = m->map[bi];
    bool full = len == m->bsize;
    if (full && img_all_zero(buf, len)) { /* bloco inteiro zerado: sem espaco no arquivo */
        if (e != zero_entry(m)) {
            release(m, e);
            set_entry(m, bi, zero_entry(m));
            if (m->zc_block == bi)
                m->zc_block = ~0ULL;
        }
        return 0;
    }
    if (E_STATE(e) == ST_DATA)
        return img_pwrite(m->fd, buf, len, E_OFF(e) + in);

    uint64_t s = alloc_slot(m), off = slot_off(m, s);
    bool clean = E_STATE(e) == ST_ZERO || (E_STATE(e) == ST_NONE && !m->backing);
    if (!full && clean && off >= m->fend) {
        /* espaco nunca gravado (alem do fim do arquivo): o resto do bloco ja le zero; grava
         * so os dados do convidado em vez do bloco inteiro */
        if (img_all_zero(buf, len)) {
            push(&m->freel, &m->nfree, &m->capfree, s);
            return 0;
        }
        if (img_pwrite(m->fd, buf, len, off + in) < 0) {
            push(&m->freel, &m->nfree, &m->capfree, s);
            return -1;
        }
        if (off + in + len > m->fend)
            m->fend = off + in + len;
        if (m->zc_block == bi)
            m->zc_block = ~0ULL;
        set_entry(m, bi, MK_DATA(off));
        return 0;
    }

    /* bloco reaproveitado ou com conteudo anterior: monta o bloco inteiro e grava de uma vez */
    uint8_t *data = m->tmp;
    if (!full) {
        if (read_in_block(m, bi, 0, data, (size_t)m->bsize) < 0)
            return -1;
        if (clean && img_all_zero(buf, len)) { /* zeros num bloco que ja e zero */
            push(&m->freel, &m->nfree, &m->capfree, s);
            return 0;
        }
    }
    memcpy(data + in, buf, len);
    if (img_pwrite(m->fd, data, (size_t)m->bsize, off) < 0) {
        push(&m->freel, &m->nfree, &m->capfree, s);
        return -1;
    }
    if (off + m->bsize > m->fend)
        m->fend = off + m->bsize;
    if (m->zc_block == bi)
        m->zc_block = ~0ULL;
    set_entry(m, bi, MK_DATA(off));
    return 0;
}

static int mvd_write(void *st, uint64_t off, const void *buf, size_t len)
{
    mvd *m = st;
    const uint8_t *p = buf;
    int r = 0;
    if (m->ro) {
        errno = EROFS;
        return -1;
    }
    pthread_mutex_lock(&m->lock);
    while (len) {
        if (off >= m->vsize) {
            errno = ENOSPC;
            r = -1;
            break;
        }
        uint64_t bi = off >> m->bbits, in = off & (m->bsize - 1);
        size_t n = (size_t)(m->bsize - in);
        if (n > len)
            n = len;
        if (write_in_block(m, bi, in, p, n) < 0) {
            r = -1;
            break;
        }
        p += n;
        off += n;
        len -= n;
    }
    maybe_commit(m);
    pthread_mutex_unlock(&m->lock);
    return r;
}

static int mvd_discard(void *st, uint64_t off, uint64_t len)
{
    mvd *m = st;
    if (m->ro)
        return -1;
    int r = 0;
    pthread_mutex_lock(&m->lock);
    while (len && off < m->vsize) {
        uint64_t bi = off >> m->bbits, in = off & (m->bsize - 1);
        uint64_t n = m->bsize - in;
        if (n > len)
            n = len;
        if (n == m->bsize) {
            uint64_t e = m->map[bi];
            if (e != zero_entry(m)) {
                release(m, e);
                set_entry(m, bi, zero_entry(m));
                if (m->zc_block == bi)
                    m->zc_block = ~0ULL;
            }
        } else {
            uint64_t e = m->map[bi];
            bool already_zero = E_STATE(e) == ST_ZERO || (E_STATE(e) == ST_NONE && !m->backing);
            static const uint8_t zero[65536];
            for (uint64_t k = 0; !already_zero && k < n; k += sizeof(zero)) {
                size_t c = n - k < sizeof(zero) ? (size_t)(n - k) : sizeof(zero);
                if (write_in_block(m, bi, in + k, zero, c) < 0) {
                    r = -1;
                    break;
                }
            }
            if (r < 0)
                break;
        }
        off += n;
        len -= n;
    }
    maybe_commit(m);
    pthread_mutex_unlock(&m->lock);
    return r;
}

static uint64_t mvd_zero_span(void *st, uint64_t off)
{
    mvd *m = st;
    if (off >= m->vsize)
        return 0;
    uint64_t bi = off >> m->bbits, e = m->map[bi];
    if (E_STATE(e) == ST_ZERO || (E_STATE(e) == ST_NONE && !m->backing))
        return m->bsize - (off & (m->bsize - 1));
    return 0;
}

static int mvd_flush(void *st)
{
    mvd *m = st;
    pthread_mutex_lock(&m->lock);
    int r = commit(m, true);
    pthread_mutex_unlock(&m->lock);
    return r;
}

/* ------------------------------------------------- acesso para conversao */

int mvd_put_block(void *st, uint64_t bi, const uint8_t *data, bool compress)
{
    mvd *m = st;
    if (bi >= m->nblocks)
        return -1;
    if (!compress || E_STATE(m->map[bi]) != ST_NONE)
        return mvd_write(st, bi << m->bbits, data, (size_t)m->bsize);
    if (img_all_zero(data, (size_t)m->bsize))
        return 0;
    size_t clen = lz4_compress(data, (size_t)m->bsize, m->zin, (size_t)(m->bsize - m->bsize / 8));
    if (!clen)
        return mvd_write(st, bi << m->bbits, data, (size_t)m->bsize);
    /* empacotado em setores de 512 B no fim do arquivo (sem alinhamento de slot) */
    pthread_mutex_lock(&m->lock);
    uint64_t end = slot_off(m, m->nslots);
    if (!m->packed_end || m->packed_end < end)
        m->packed_end = end;
    uint64_t off = m->packed_end;
    int r = img_pwrite(m->fd, m->zin, clen, off);
    if (r == 0) {
        m->packed_end = (off + clen + 511) & ~511ULL;
        if (m->packed_end > m->fend)
            m->fend = m->packed_end;
        /* os slots normais seguintes comecam depois da area empacotada */
        m->nslots = (m->packed_end - m->data_off + m->bsize - 1) >> m->bbits;
        set_entry(m, bi, MK_LZ4(off, clen));
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

uint64_t mvd_block_size(void *st) { return ((mvd *)st)->bsize; }

void mvd_get_stats(void *st, mvd_stats *s)
{
    mvd *m = st;
    memset(s, 0, sizeof(*s));
    pthread_mutex_lock(&m->lock);
    s->block_size = m->bsize;
    s->blocks = m->nblocks;
    s->used_bytes = m->data_off;
    for (uint64_t i = 0; i < m->nblocks; i++) {
        uint64_t e = m->map[i];
        switch (E_STATE(e)) {
        case ST_DATA: s->data_blocks++; s->used_bytes += m->bsize; break;
        case ST_ZERO: s->zero_blocks++; break;
        case ST_LZ4: s->lz4_blocks++; s->used_bytes += E_CLEN(e); break;
        default: break;
        }
    }
    s->free_slots = m->nfree + m->npend;
    snprintf(s->backing, sizeof(s->backing), "%s", m->backing_name);
    pthread_mutex_unlock(&m->lock);
}

/* --------------------------------------------------- abertura e criacao */

static void mvd_close(void *st)
{
    mvd *m = st;
    if (!m)
        return;
    if (m->map)
        commit(m, true);
    free(m->map);
    free(m->dirty);
    free(m->freel);
    free(m->pend);
    free(m->zbuf);
    free(m->zin);
    free(m->tmp);
    blk_close(m->backing);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

static bool mvd_probe(const img_probe_info *p) { return !memcmp(p->head, MVD_MAGIC, 8); }

static void build_header(uint8_t *h, uint64_t vsize, unsigned bbits, uint64_t data_off, const char *backing)
{
    memset(h, 0, HDR_SIZE);
    memcpy(h, MVD_MAGIC, 8);
    wr32le(h + 8, MVD_VERSION);
    wr32le(h + 12, HDR_SIZE);
    wr64le(h + 16, vsize);
    wr32le(h + 24, bbits);
    wr64le(h + 32, MAP_OFF);
    wr64le(h + 40, (vsize + (1ULL << bbits) - 1) >> bbits);
    wr64le(h + 48, data_off);
    if (backing) {
        size_t n = strlen(backing);
        wr32le(h + 56, (uint32_t)n);
        memcpy(h + NAME_OFF, backing, n);
    }
    wr32le(h + CRC_OFF, img_crc32c(0, h, CRC_OFF));
}

static uint64_t data_offset(uint64_t nblocks)
{
    return (MAP_OFF + nblocks * 8 + 65535) & ~65535ULL;
}

#define FAIL(...)                           \
    do {                                    \
        snprintf(err, errlen, __VA_ARGS__); \
        mvd_close(m);                       \
        return NULL;                        \
    } while (0)

static void *mvd_open(img_open_args *a, char *err, size_t errlen)
{
    uint8_t h[HDR_SIZE];
    mvd *m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(err, errlen, "sem memoria");
        return NULL;
    }
    pthread_mutex_init(&m->lock, NULL);
    m->fd = a->fd;
    m->ro = a->readonly;
    m->zc_block = ~0ULL;
    if (img_pread(m->fd, h, HDR_SIZE, 0) < 0)
        FAIL("mvd: erro ao ler o cabecalho: %s", strerror(errno));
    if (rd32le(h + CRC_OFF) != img_crc32c(0, h, CRC_OFF))
        FAIL("mvd: cabecalho corrompido (CRC)");
    if (rd32le(h + 8) != MVD_VERSION)
        FAIL("mvd: versao %u nao suportada", rd32le(h + 8));
    m->vsize = rd64le(h + 16);
    m->bbits = rd32le(h + 24);
    if (m->bbits < 16 || m->bbits > 22)
        FAIL("mvd: tamanho de bloco invalido");
    m->bsize = 1ULL << m->bbits;
    m->nblocks = rd64le(h + 40);
    m->data_off = rd64le(h + 48);
    if (rd64le(h + 32) != MAP_OFF || m->nblocks != (m->vsize + m->bsize - 1) >> m->bbits ||
        m->nblocks > (1ULL << 28) || m->data_off < MAP_OFF + m->nblocks * 8)
        FAIL("mvd: cabecalho invalido");
    uint32_t nlen = rd32le(h + 56);
    if (nlen >= NAME_MAX_LEN)
        FAIL("mvd: nome do arquivo base invalido");

    uint64_t npages = (m->nblocks + 511) / 512;
    m->map = malloc((size_t)(npages * 4096));
    m->dirty = calloc((size_t)npages + 1, 1);
    m->zbuf = malloc((size_t)m->bsize);
    m->zin = malloc((size_t)m->bsize);
    m->tmp = malloc((size_t)m->bsize);
    if (!m->map || !m->dirty || !m->zbuf || !m->zin || !m->tmp)
        FAIL("mvd: sem memoria para o mapa");
    if (img_pread(m->fd, m->map, (size_t)(m->nblocks * 8), MAP_OFF) < 0)
        FAIL("mvd: erro ao ler o mapa: %s", strerror(errno));
    for (uint64_t i = 0; i < m->nblocks; i++)
        m->map[i] = rd64le((const uint8_t *)&m->map[i]);

    /* espacos em uso; o resto (escritas sem commit, TRIM) fica livre */
    uint64_t fsize = img_file_size(m->fd);
    uint64_t end = m->data_off;
    for (uint64_t i = 0; i < m->nblocks; i++) {
        uint64_t e = m->map[i];
        uint64_t st = E_STATE(e), o = E_OFF(e), len = st == ST_DATA ? m->bsize : E_CLEN(e);
        if (st == ST_DATA || st == ST_LZ4) {
            /* um bloco de dados pode terminar alem do fim do arquivo (o resto le zero) */
            if (o < m->data_off || (st == ST_DATA ? o >= fsize : o + len > fsize) ||
                (st == ST_LZ4 && (!len || len >= m->bsize)))
                FAIL("mvd: mapa aponta para fora do arquivo (bloco %llu); rode 'mvm-img check'",
                     (unsigned long long)i);
            if (o + len > end)
                end = o + len;
        }
    }
    m->nslots = (end - m->data_off + m->bsize - 1) >> m->bbits;
    if (fsize > m->data_off) {
        uint64_t fs = (fsize - m->data_off + m->bsize - 1) >> m->bbits;
        if (fs > m->nslots)
            m->nslots = fs;
    }
    if (m->nslots) {
        uint8_t *used = calloc((size_t)m->nslots, 1);
        if (!used)
            FAIL("mvd: sem memoria");
        for (uint64_t i = 0; i < m->nblocks; i++) {
            uint64_t e = m->map[i], st = E_STATE(e);
            if (st != ST_DATA && st != ST_LZ4)
                continue;
            uint64_t o = E_OFF(e) - m->data_off, len = st == ST_DATA ? m->bsize : E_CLEN(e);
            for (uint64_t s = o >> m->bbits; s <= (o + len - 1) >> m->bbits && s < m->nslots; s++) {
                if (st == ST_DATA && used[s]) {
                    free(used);
                    FAIL("mvd: dois blocos no mesmo lugar do arquivo; rode 'mvm-img check'");
                }
                used[s] = 1;
            }
        }
        for (uint64_t s = m->nslots; s-- > 0;)
            if (!used[s] && !push(&m->freel, &m->nfree, &m->capfree, s)) {
                free(used);
                FAIL("mvd: sem memoria");
            }
        free(used);
    }
    if (nlen) {
        char name[NAME_MAX_LEN];
        memcpy(name, h + NAME_OFF, nlen);
        name[nlen] = 0;
        snprintf(m->backing_name, sizeof(m->backing_name), "%s", name);
        if (!(m->backing = img_open_backing(a, name, err, errlen))) {
            mvd_close(m);
            return NULL;
        }
    }
    m->fend = fsize;
    m->last_commit = now_ns();
    a->readonly = m->ro;
    a->vsize = m->vsize;
    a->can_discard = true;
    return m;
}

int mvd_create_fd(int fd, uint64_t vsize, unsigned block_bits, const char *backing, char *err, size_t errlen)
{
    if (!block_bits)
        block_bits = MVD_DEFAULT_BLOCK_BITS;
    if (block_bits < 16 || block_bits > 22) {
        snprintf(err, errlen, "tamanho de bloco invalido (64 KiB a 4 MiB)");
        return -1;
    }
    if (!vsize || vsize > (1ULL << 28 << block_bits) || (vsize & 511)) {
        snprintf(err, errlen, "tamanho de disco invalido");
        return -1;
    }
    if (backing && strlen(backing) >= NAME_MAX_LEN) {
        snprintf(err, errlen, "nome do arquivo base longo demais");
        return -1;
    }
    uint64_t nblocks = (vsize + (1ULL << block_bits) - 1) >> block_bits;
    uint64_t data_off = data_offset(nblocks);
    uint8_t *h = malloc(HDR_SIZE);
    uint8_t *z = calloc(1, 65536);
    int r = -1;
    if (!h || !z) {
        snprintf(err, errlen, "sem memoria");
        goto out;
    }
    build_header(h, vsize, block_bits, data_off, backing);
    if (ftruncate(fd, 0) < 0 || img_pwrite(fd, h, HDR_SIZE, 0) < 0)
        goto ioerr;
    for (uint64_t o = MAP_OFF; o < data_off; o += 65536) { /* mapa zerado (nada alocado) */
        uint64_t n = data_off - o < 65536 ? data_off - o : 65536;
        if (img_pwrite(fd, z, (size_t)n, o) < 0)
            goto ioerr;
    }
    if (fdatasync(fd) < 0)
        goto ioerr;
    r = 0;
    goto out;
ioerr:
    snprintf(err, errlen, "erro ao gravar a imagem: %s", strerror(errno));
out:
    free(h);
    free(z);
    return r;
}

const blk_driver img_mvd = {
    .name = "mvd",
    .probe = mvd_probe,
    .open = mvd_open,
    .read = mvd_read,
    .write = mvd_write,
    .flush = mvd_flush,
    .discard = mvd_discard,
    .zero_span = mvd_zero_span,
    .close = mvd_close,
};
