/*
 * Codificador JPEG baseline (YCbCr 4:2:0, tabelas padrao do anexo K), usado pelo
 * VNC (Tight com JPEG) para blocos fotograficos: e o que deixa o VNC usavel com
 * papel de parede e imagens. Simples e sem dependencias; a DCT e a separavel em
 * ponto flutuante.
 */
#include "internal.h"

#include <math.h>

static const uint8_t zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static const uint8_t q_lum[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                  14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                  18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                  49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const uint8_t q_chr[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                  24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

static const uint8_t dc_lum_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_chr_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_lum_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_lum_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t ac_chr_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_chr_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} huff;

static huff h_dc_lum, h_dc_chr, h_ac_lum, h_ac_chr;
static float cos_t[8][8]; /* 0,5 * C(u) * cos((2x + 1) u pi / 16) */
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void build_huff(huff *h, const uint8_t bits[16], const uint8_t *vals)
{
    uint16_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++, k++) {
            h->code[vals[k]] = code++;
            h->len[vals[k]] = (uint8_t)l;
        }
        code <<= 1;
    }
}

static void init_tables(void)
{
    build_huff(&h_dc_lum, dc_lum_bits, dc_vals);
    build_huff(&h_dc_chr, dc_chr_bits, dc_vals);
    build_huff(&h_ac_lum, ac_lum_bits, ac_lum_vals);
    build_huff(&h_ac_chr, ac_chr_bits, ac_chr_vals);
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            cos_t[u][x] = (float)(0.5 * (u ? 1.0 : 1.0 / sqrt(2.0)) * cos((2 * x + 1) * u * M_PI / 16.0));
}

typedef struct {
    uint8_t *out;
    size_t cap, n;
    uint32_t acc;
    int nbits;
    bool overflow;
} bitw;

static void put_byte(bitw *b, uint8_t v)
{
    if (b->n < b->cap)
        b->out[b->n++] = v;
    else
        b->overflow = true;
}

static void put_bits(bitw *b, uint32_t v, int n)
{
    b->acc = (b->acc << n) | (v & ((1u << n) - 1));
    b->nbits += n;
    while (b->nbits >= 8) {
        uint8_t c = (uint8_t)(b->acc >> (b->nbits - 8));
        put_byte(b, c);
        if (c == 0xff)
            put_byte(b, 0); /* byte de enchimento */
        b->nbits -= 8;
    }
}

static void put_marker(bitw *b, uint8_t m, const uint8_t *data, int len)
{
    put_byte(b, 0xff);
    put_byte(b, m);
    if (data) {
        put_byte(b, (uint8_t)((len + 2) >> 8));
        put_byte(b, (uint8_t)(len + 2));
        for (int i = 0; i < len; i++)
            put_byte(b, data[i]);
    }
}

static void write_dht(bitw *b, int cls_id, const uint8_t bits[16], const uint8_t *vals, int nvals)
{
    uint8_t d[1 + 16 + 162];
    d[0] = (uint8_t)cls_id;
    memcpy(d + 1, bits, 16);
    memcpy(d + 17, vals, (size_t)nvals);
    put_marker(b, 0xc4, d, 17 + nvals);
}

static int category(int v)
{
    if (v < 0)
        v = -v;
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

/* DCT, quantizacao e Huffman de um bloco 8x8 (valores ja centrados em 0) */
static void encode_block(bitw *b, const float in[64], const float qt[64], int *dc_prev, const huff *dc, const huff *ac)
{
    float tmp[64], co[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++)
                s += in[y * 8 + x] * cos_t[u][x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++)
                s += tmp[y * 8 + u] * cos_t[v][y];
            co[v * 8 + u] = s;
        }
    int q[64];
    for (int i = 0; i < 64; i++) {
        float f = co[zigzag[i]] / qt[i];
        q[i] = (int)(f < 0 ? f - 0.5f : f + 0.5f);
    }
    int diff = q[0] - *dc_prev;
    *dc_prev = q[0];
    int c = category(diff);
    put_bits(b, dc->code[c], dc->len[c]);
    if (c)
        put_bits(b, (uint32_t)(diff < 0 ? diff - 1 : diff), c);
    int run = 0;
    int last = 63;
    while (last > 0 && !q[last])
        last--;
    for (int i = 1; i <= last; i++) {
        if (!q[i]) {
            run++;
            continue;
        }
        while (run > 15) { /* ZRL */
            put_bits(b, ac->code[0xf0], ac->len[0xf0]);
            run -= 16;
        }
        c = category(q[i]);
        int sym = (run << 4) | c;
        put_bits(b, ac->code[sym], ac->len[sym]);
        put_bits(b, (uint32_t)(q[i] < 0 ? q[i] - 1 : q[i]), c);
        run = 0;
    }
    if (last < 63)
        put_bits(b, ac->code[0], ac->len[0]); /* EOB */
}

/* pixel XRGB com repeticao da borda */
static inline uint32_t px_at(const uint32_t *p, uint32_t stride, int w, int h, int x, int y)
{
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    return p[(size_t)y * stride + (size_t)x];
}

size_t jpeg_encode(const uint32_t *xrgb, uint32_t stride, int w, int h, int quality, uint8_t *out, size_t cap)
{
    pthread_once(&init_once, init_tables);
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    uint8_t ql[64], qc[64];
    float fql[64], fqc[64];
    for (int i = 0; i < 64; i++) {
        int a = (q_lum[zigzag[i]] * scale + 50) / 100, c = (q_chr[zigzag[i]] * scale + 50) / 100;
        ql[i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a);
        qc[i] = (uint8_t)(c < 1 ? 1 : c > 255 ? 255 : c);
        fql[i] = ql[i];
        fqc[i] = qc[i];
    }
    bitw b = {out, cap, 0, 0, 0, false};
    put_marker(&b, 0xd8, NULL, 0); /* SOI */
    uint8_t dqt[130];
    dqt[0] = 0;
    memcpy(dqt + 1, ql, 64);
    dqt[65] = 1;
    memcpy(dqt + 66, qc, 64);
    put_marker(&b, 0xdb, dqt, 130);
    uint8_t sof[15] = {8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w, 3,
                       1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
    put_marker(&b, 0xc0, sof, 15);
    write_dht(&b, 0x00, dc_lum_bits, dc_vals, 12);
    write_dht(&b, 0x10, ac_lum_bits, ac_lum_vals, 162);
    write_dht(&b, 0x01, dc_chr_bits, dc_vals, 12);
    write_dht(&b, 0x11, ac_chr_bits, ac_chr_vals, 162);
    uint8_t sos[10] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    put_marker(&b, 0xda, sos, 10);

    int dcy = 0, dcb = 0, dcr = 0;
    float Y[4][64], Cb[64], Cr[64];
    for (int my = 0; my < h; my += 16)
        for (int mx = 0; mx < w; mx += 16) {
            float cbs[16][16], crs[16][16];
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    uint32_t p = px_at(xrgb, stride, w, h, mx + x, my + y);
                    float r = (float)((p >> 16) & 255), g = (float)((p >> 8) & 255), bl = (float)(p & 255);
                    int blk = (y >> 3) * 2 + (x >> 3);
                    Y[blk][(y & 7) * 8 + (x & 7)] = 0.299f * r + 0.587f * g + 0.114f * bl - 128.0f;
                    cbs[y][x] = -0.168736f * r - 0.331264f * g + 0.5f * bl;
                    crs[y][x] = 0.5f * r - 0.418688f * g - 0.081312f * bl;
                }
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    Cb[y * 8 + x] = (cbs[2 * y][2 * x] + cbs[2 * y][2 * x + 1] + cbs[2 * y + 1][2 * x] + cbs[2 * y + 1][2 * x + 1]) * 0.25f;
                    Cr[y * 8 + x] = (crs[2 * y][2 * x] + crs[2 * y][2 * x + 1] + crs[2 * y + 1][2 * x] + crs[2 * y + 1][2 * x + 1]) * 0.25f;
                }
            for (int k = 0; k < 4; k++)
                encode_block(&b, Y[k], fql, &dcy, &h_dc_lum, &h_ac_lum);
            encode_block(&b, Cb, fqc, &dcb, &h_dc_chr, &h_ac_chr);
            encode_block(&b, Cr, fqc, &dcr, &h_dc_chr, &h_ac_chr);
            if (b.overflow)
                return 0;
        }
    if (b.nbits) /* completa o ultimo byte com 1s */
        put_bits(&b, 0x7f, 8 - b.nbits);
    put_marker(&b, 0xd9, NULL, 0); /* EOI */
    return b.overflow ? 0 : b.n;
}
