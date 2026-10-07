/*
 * Descompressor DEFLATE bruto (RFC 1951), sem dependencias: usado pelos clusters
 * comprimidos de imagens qcow2. Decodificacao canonica de Huffman por contagem
 * de comprimentos (simples e suficiente para clusters de ate alguns MiB).
 */
#include "inflate.h"

#include <string.h>

typedef struct {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t outlen, outpos;
} istate;

typedef struct {
    uint16_t count[16];  /* codigos por comprimento */
    uint16_t symbol[320]; /* simbolos ordenados pelo codigo */
} huff;

/* -1 = fim da entrada */
static int bits(istate *s, int need)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen)
            return -1;
        v |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

static int build(huff *h, const uint8_t *len, int n)
{
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[len[i]]++;
    if (h->count[0] == n)
        return 0; /* sem codigos */
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0)
            return -1; /* excesso de codigos */
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (len[i])
            h->symbol[offs[len[i]]++] = (uint16_t)i;
    return left; /* > 0: incompleto (permitido so com 1 codigo) */
}

static int decode(istate *s, const huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        int b = bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        int count = h->count[l];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                   257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* 0 = bloco concluido, 1 = saida cheia, -1 = erro */
static int codes(istate *s, const huff *lc, const huff *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen)
                return 1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            int e = bits(s, lext[sym]);
            if (e < 0)
                return -1;
            size_t len = lbase[sym] + (size_t)e;
            int ds = decode(s, dc);
            if (ds < 0 || ds >= 30)
                return -1;
            e = bits(s, dext[ds]);
            if (e < 0)
                return -1;
            size_t dist = dbase[ds] + (size_t)e;
            if (dist > s->outpos)
                return -1;
            while (len--) {
                if (s->outpos >= s->outlen)
                    return 1;
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
}

static int fixed(istate *s)
{
    static huff lc, dc;
    static int ready;
    if (!ready) {
        uint8_t len[288];
        int i = 0;
        for (; i < 144; i++) len[i] = 8;
        for (; i < 256; i++) len[i] = 9;
        for (; i < 280; i++) len[i] = 7;
        for (; i < 288; i++) len[i] = 8;
        build(&lc, len, 288);
        for (i = 0; i < 30; i++) len[i] = 5;
        build(&dc, len, 30);
        ready = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(istate *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30)
        return -1;
    uint8_t len[320];
    memset(len, 0, sizeof(len));
    for (int i = 0; i < ncode; i++) {
        int b = bits(s, 3);
        if (b < 0)
            return -1;
        len[order[i]] = (uint8_t)b;
    }
    huff lc, dc;
    if (build(&lc, len, 19) != 0)
        return -1;
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(s, &lc);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            len[idx++] = (uint8_t)sym;
            continue;
        }
        int rep, val = 0;
        if (sym == 16) {
            if (idx == 0)
                return -1;
            val = len[idx - 1];
            rep = bits(s, 2);
            if (rep < 0) return -1;
            rep += 3;
        } else if (sym == 17) {
            rep = bits(s, 3);
            if (rep < 0) return -1;
            rep += 3;
        } else {
            rep = bits(s, 7);
            if (rep < 0) return -1;
            rep += 11;
        }
        if (idx + rep > nlen + ndist)
            return -1;
        while (rep--)
            len[idx++] = (uint8_t)val;
    }
    if (len[256] == 0)
        return -1;
    int err = build(&lc, len, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1))
        return -1;
    err = build(&dc, len + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1))
        return -1;
    return codes(s, &lc, &dc);
}

long mvm_inflate(const void *in, size_t inlen, void *out, size_t outlen)
{
    istate s = {.in = in, .inlen = inlen, .out = out, .outlen = outlen};
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0)
            return s.outpos ? (long)s.outpos : -1;
        int r;
        if (type == 0) { /* armazenado */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen)
                return -1;
            unsigned len = s.in[s.inpos] | (s.in[s.inpos + 1] << 8);
            unsigned nlen = s.in[s.inpos + 2] | (s.in[s.inpos + 3] << 8);
            s.inpos += 4;
            if (len != (~nlen & 0xffff) || s.inpos + len > s.inlen)
                return -1;
            size_t n = len;
            r = 0;
            if (n > s.outlen - s.outpos) {
                n = s.outlen - s.outpos;
                r = 1;
            }
            memcpy(s.out + s.outpos, s.in + s.inpos, n);
            s.outpos += n;
            s.inpos += len;
        } else if (type == 1) {
            r = fixed(&s);
        } else if (type == 2) {
            r = dynamic(&s);
        } else {
            return -1;
        }
        if (r < 0)
            return -1;
        if (r == 1)
            return (long)s.outpos; /* saida cheia (cluster completo) */
    } while (!last);
    return (long)s.outpos;
}
