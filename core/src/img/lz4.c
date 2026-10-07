/*
 * LZ4 no formato de bloco (compativel com o LZ4 de referencia): usado nos blocos
 * comprimidos do MVD. Compressor guloso com tabela de hash; descompressor com
 * verificacao de limites (dados vindos do arquivo nunca sao confiaveis).
 */
#include "img.h"

#include <string.h>

#define MINMATCH 4
#define LASTLITERALS 5
#define MFLIMIT 12
#define HASH_BITS 13

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static unsigned hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HASH_BITS); }

static uint8_t *put_len(uint8_t *op, uint8_t *oend, size_t len)
{
    while (len >= 255) {
        if (op >= oend)
            return NULL;
        *op++ = 255;
        len -= 255;
    }
    if (op >= oend)
        return NULL;
    *op++ = (uint8_t)len;
    return op;
}

/* emite literais [anchor, ip) e, se mlen > 0, um match (offset, mlen) */
static uint8_t *emit(uint8_t *op, uint8_t *oend, const uint8_t *anchor, size_t lit, size_t off, size_t mlen)
{
    if (op >= oend)
        return NULL;
    uint8_t *tok = op++;
    *tok = (uint8_t)((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15 && !(op = put_len(op, oend, lit - 15)))
        return NULL;
    if ((size_t)(oend - op) < lit)
        return NULL;
    memcpy(op, anchor, lit);
    op += lit;
    if (!mlen)
        return op;
    if (oend - op < 2)
        return NULL;
    *op++ = (uint8_t)off;
    *op++ = (uint8_t)(off >> 8);
    size_t m = mlen - MINMATCH;
    *tok |= (uint8_t)(m >= 15 ? 15 : m);
    if (m >= 15 && !(op = put_len(op, oend, m - 15)))
        return NULL;
    return op;
}

size_t lz4_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    static __thread uint32_t table[1 << HASH_BITS];
    uint8_t *op = dst, *oend = dst + cap;
    const uint8_t *ip = src, *anchor = src;
    if (n >= MFLIMIT + 1) {
        memset(table, 0, sizeof(table));
        const uint8_t *mflimit = src + n - MFLIMIT, *mlimit = src + n - LASTLITERALS;
        ip++;
        while (ip < mflimit) {
            uint32_t v = rd32(ip);
            unsigned h = hash4(v);
            const uint8_t *ref = src + table[h];
            table[h] = (uint32_t)(ip - src);
            if (ref >= ip || ip - ref > 65535 || rd32(ref) != v) {
                ip++;
                continue;
            }
            while (ip > anchor && ref > src && ip[-1] == ref[-1]) { /* estende para tras */
                ip--;
                ref--;
            }
            const uint8_t *e = ip + MINMATCH, *r = ref + MINMATCH;
            while (e < mlimit && *e == *r) {
                e++;
                r++;
            }
            if (!(op = emit(op, oend, anchor, (size_t)(ip - anchor), (size_t)(ip - ref), (size_t)(e - ip))))
                return 0;
            ip = anchor = e;
            if (ip < mflimit)
                table[hash4(rd32(ip - 2))] = (uint32_t)(ip - 2 - src);
        }
    }
    if (!(op = emit(op, oend, anchor, (size_t)(src + n - anchor), 0, 0)))
        return 0;
    return (size_t)(op - dst);
}

long lz4_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    const uint8_t *ip = src, *iend = src + n;
    uint8_t *op = dst, *oend = dst + cap;
    while (ip < iend) {
        unsigned tok = *ip++;
        size_t lit = tok >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= iend)
                    return -1;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit || (size_t)(oend - op) < lit)
            return -1;
        memcpy(op, ip, lit);
        ip += lit;
        op += lit;
        if (ip >= iend)
            break; /* ultima sequencia: so literais */
        if (iend - ip < 2)
            return -1;
        size_t off = ip[0] | ((size_t)ip[1] << 8);
        ip += 2;
        size_t mlen = tok & 15;
        if (mlen == 15) {
            unsigned b;
            do {
                if (ip >= iend)
                    return -1;
                b = *ip++;
                mlen += b;
            } while (b == 255);
        }
        mlen += MINMATCH;
        if (!off || off > (size_t)(op - dst) || (size_t)(oend - op) < mlen)
            return -1;
        const uint8_t *ref = op - off;
        if (off >= mlen) {
            memcpy(op, ref, mlen);
            op += mlen;
        } else {
            while (mlen--)
                *op++ = *ref++;
        }
    }
    return (long)(op - dst);
}
