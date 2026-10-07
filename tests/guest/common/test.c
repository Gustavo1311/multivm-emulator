/*
 * Teste de CPU portavel: roda nativamente (gera a saida de referencia) e como
 * programa "bare-metal" dentro do emulador. As saidas devem ser identicas.
 * Evita comportamento dependente de implementacao (so tipos de largura fixa).
 */
#include <stddef.h>
#include <stdint.h>

void test_putc(char c);

static void puts_(const char *s)
{
    while (*s)
        test_putc(*s++);
}

static void put_hex(uint64_t v)
{
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    buf[16] = 0;
    puts_(buf);
}

static void put_dec(int64_t v)
{
    char buf[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    if (v < 0)
        test_putc('-');
    do {
        buf[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n)
        test_putc(buf[--n]);
}

static void report(const char *name, uint64_t v)
{
    puts_(name);
    puts_(": ");
    put_hex(v);
    test_putc('\n');
}

/* impede que o compilador calcule tudo em tempo de compilacao */
static volatile uint64_t seed_v = 0x123456789abcdefULL;

static uint64_t rng_state;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

#ifdef __SIZEOF_INT128__
static uint64_t umulh(uint64_t a, uint64_t b) { return (uint64_t)((unsigned __int128)a * b >> 64); }
static uint64_t smulh(int64_t a, int64_t b) { return (uint64_t)((__int128)a * b >> 64); }
#else
static uint64_t umulh(uint64_t a, uint64_t b)
{
    uint64_t al = a & 0xffffffffu, ah = a >> 32, bl = b & 0xffffffffu, bh = b >> 32;
    uint64_t t = al * bl;
    uint64_t w1 = ah * bl + (t >> 32);
    uint64_t w2 = al * bh + (w1 & 0xffffffffu);
    return ah * bh + (w1 >> 32) + (w2 >> 32);
}
static uint64_t smulh(int64_t a, int64_t b)
{
    uint64_t r = umulh((uint64_t)a, (uint64_t)b);
    if (a < 0) r -= (uint64_t)b;
    if (b < 0) r -= (uint64_t)a;
    return r;
}
#endif

static uint64_t test_int(void)
{
    uint64_t h = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t a = rng(), b = rng() | 1;
        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        int64_t sa = (int64_t)a, sb = (int64_t)b;
        h = h * 31 + a / b;
        h = h * 31 + a % b;
        h = h * 31 + (uint64_t)(sa / sb);
        h = h * 31 + (uint64_t)(sa % sb);
        h = h * 31 + a32 / b32;
        h = h * 31 + (uint32_t)((int32_t)a32 / (int32_t)b32);
        h = h * 31 + (a >> (b & 63)) + (a << (b & 63));
        h = h * 31 + (uint64_t)(sa >> (b & 63));
        h = h * 31 + umulh(a, b);
        h = h * 31 + smulh(sa, sb);
        h = h * 31 + (uint64_t)__builtin_clzll(a | 1) + (uint64_t)__builtin_ctzll(b);
        h = h * 31 + (uint64_t)__builtin_popcountll(a);
        h = h * 31 + __builtin_bswap64(a) + __builtin_bswap32(a32);
        h = h * 31 + (a32 > b32 ? a32 - b32 : b32 - a32);
        h = h * 31 + (uint64_t)(sa < sb) + (uint64_t)(a < b) * 2;
        h = h * 31 + (uint64_t)(int64_t)(int8_t)a + (uint64_t)(int64_t)(int16_t)b;
        h = h * 31 + ((a & 0xff00ff) | (b & ~0xff00ffULL)) ^ (a32 * b32);
        uint32_t r = (a32 >> (b32 & 31)) | (a32 << ((32 - (b32 & 31)) & 31));
        h = h * 31 + r;
    }
    return h;
}

static uint64_t test_mem(void)
{
    static uint8_t buf[4096], buf2[4096];
    uint64_t h = 0;
    for (int i = 0; i < 4096; i++)
        buf[i] = (uint8_t)rng();
    for (int round = 0; round < 200; round++) {
        size_t off = rng() % 2000, off2 = rng() % 2000, len = rng() % 2000;
        __builtin_memcpy(buf2 + off2, buf + off, len);
        __builtin_memset(buf + (rng() % 1000), (int)(rng() & 0xff), rng() % 1000);
        for (size_t k = 0; k < 4096; k += 61)
            h = h * 131 + buf2[k] + buf[k];
    }
    /* acessos desalinhados */
    for (int i = 0; i < 100; i++) {
        uint32_t v32;
        uint64_t v64;
        uint16_t v16;
        __builtin_memcpy(&v32, buf + i * 7 + 1, 4);
        __builtin_memcpy(&v64, buf + i * 5 + 3, 8);
        __builtin_memcpy(&v16, buf + i * 3 + 1, 2);
        h = h * 7 + v32 + v64 + v16;
    }
    return h;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void sort_u32(uint32_t *a, int n)
{
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++)
            for (int j = i; j >= gap && cmp_u32(&a[j - gap], &a[j]) > 0; j -= gap) {
                uint32_t t = a[j];
                a[j] = a[j - gap];
                a[j - gap] = t;
            }
}

static uint64_t test_sort(void)
{
    static uint32_t a[1000];
    for (int i = 0; i < 1000; i++)
        a[i] = (uint32_t)rng();
    sort_u32(a, 1000);
    uint64_t h = 0;
    for (int i = 0; i < 1000; i++)
        h = h * 33 + a[i];
    return h;
}

/* SHA-256 */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static uint64_t test_sha256(void)
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64];
    for (int blk = 0; blk < 64; blk++) {
        for (int i = 0; i < 64; i++)
            block[i] = (uint8_t)(blk * 7 + i * 13);
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
                   (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    return ((uint64_t)h[0] << 32 | h[1]) ^ ((uint64_t)h[6] << 32 | h[7]);
}

/* laços vetorizaveis */
static uint64_t test_vec(void)
{
    static int8_t b8[1024];
    static int16_t b16[1024];
    static int32_t b32[1024];
    static uint32_t u32[1024];
    static float f32[1024];
    for (int i = 0; i < 1024; i++) {
        uint64_t r = rng();
        b8[i] = (int8_t)r;
        b16[i] = (int16_t)(r >> 8);
        b32[i] = (int32_t)(r >> 16);
        u32[i] = (uint32_t)(r >> 24);
        f32[i] = (float)(int32_t)(r >> 40) / 1024.0f;
    }
    int32_t s8 = 0, s16 = 0;
    int64_t s32 = 0;
    uint32_t mx = 0, mn = 0xffffffffu;
    for (int i = 0; i < 1024; i++) s8 += b8[i];
    for (int i = 0; i < 1024; i++) s16 += b16[i] * b16[(i + 1) & 1023];
    for (int i = 0; i < 1024; i++) s32 += b32[i];
    for (int i = 0; i < 1024; i++) { mx = u32[i] > mx ? u32[i] : mx; mn = u32[i] < mn ? u32[i] : mn; }
    for (int i = 0; i < 1024; i++) b32[i] = b32[i] * 3 + (b16[i] >> 2);
    for (int i = 0; i < 1024; i++) b8[i] = (int8_t)(b8[i] < 0 ? -b8[i] : b8[i]);
    for (int i = 0; i < 1024; i++) u32[i] = (u32[i] >> 3) ^ (u32[i] << 5);
    float fs = 0;
    for (int i = 0; i < 1024; i++) f32[i] = f32[i] * 1.5f + 0.25f;
    for (int i = 0; i < 1024; i += 4) fs += f32[i];
    uint64_t h = (uint64_t)(uint32_t)s8 * 3 + (uint64_t)(uint32_t)s16 * 5 + (uint64_t)s32 * 7 + mx + mn;
    for (int i = 0; i < 1024; i++)
        h = h * 3 + (uint32_t)b32[i] + (uint8_t)b8[i] + u32[i];
    uint32_t fb;
    __builtin_memcpy(&fb, &fs, 4);
    return h ^ fb;
}

static uint64_t test_float(void)
{
    uint64_t h = 0;
    double acc = 1.0;
    float facc = 1.0f;
    for (int i = 1; i < 500; i++) {
        double x = (double)(int32_t)rng() / 65536.0;
        float fx = (float)x;
        acc = acc * 0.999 + x / (double)i;
        facc = facc * 0.5f + fx * 0.25f;
        double s = __builtin_sqrt(x < 0 ? -x : x);
        float fs = __builtin_sqrtf(fx < 0 ? -fx : fx);
        int64_t iv = (int64_t)(x * 1000.0);
        int32_t fiv = (int32_t)(fx * 10.0f);
        uint64_t bits, sbits;
        uint32_t fbits;
        __builtin_memcpy(&bits, &acc, 8);
        __builtin_memcpy(&sbits, &s, 8);
        __builtin_memcpy(&fbits, &fs, 4);
        h = h * 31 + bits + sbits + fbits + (uint64_t)iv + (uint64_t)(int64_t)fiv;
        h = h * 31 + (uint64_t)(x < acc) + (uint64_t)(fx >= facc) * 2;
        double fl = __builtin_floor(x), ce = __builtin_ceil(x), tr = __builtin_trunc(x);
        h = h * 31 + (uint64_t)(int64_t)fl + (uint64_t)(int64_t)ce + (uint64_t)(int64_t)tr;
        h = h * 31 + (uint64_t)(double)(uint32_t)rng();
        h = h * 31 + (uint64_t)(int64_t)(float)(int16_t)rng();
    }
    uint32_t fb;
    __builtin_memcpy(&fb, &facc, 4);
    return h ^ fb;
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static uint64_t test_switch(void)
{
    uint64_t h = 0;
    for (int i = 0; i < 3000; i++) {
        switch ((int)(rng() % 13)) {
        case 0: h += 1; break;
        case 1: h ^= 0x55; break;
        case 2: h *= 3; break;
        case 3: h -= 7; break;
        case 4: h = (h << 3) | (h >> 61); break;
        case 5: h += (uint64_t)fib(10); break;
        case 6: h ^= h >> 17; break;
        case 7: h += 0x1234; break;
        case 8: h = ~h; break;
        case 9: h += h / 3; break;
        case 10: h ^= 0xdeadbeef; break;
        case 11: h -= h >> 2; break;
        default: h += 99; break;
        }
    }
    return h;
}

int test_main(void)
{
    rng_state = seed_v;
    puts_("MultiVM CPU test\n");
    report("int", test_int());
    report("mem", test_mem());
    report("sort", test_sort());
    report("sha256", test_sha256());
    report("vec", test_vec());
    report("float", test_float());
    report("switch", test_switch());
    puts_("fib(20)=");
    put_dec(fib(20));
    test_putc('\n');
    puts_("done\n");
    return 0;
}
