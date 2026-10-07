/* Runtime minimo para os testes em alvos de 32 bits (ARM EABI e i386). */
#include <stdint.h>

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    if (!d) {
        if (rem) *rem = n;
        return ~0ULL;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

static int64_t sdivmod64(int64_t a, int64_t b, int64_t *rem)
{
    uint64_t ua = a < 0 ? 0 - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? 0 - (uint64_t)b : (uint64_t)b;
    uint64_t r;
    uint64_t q = udivmod64(ua, ub, &r);
    if ((a < 0) != (b < 0)) q = 0 - q;
    if (a < 0) r = 0 - r;
    if (rem) *rem = (int64_t)r;
    return (int64_t)q;
}

#if defined(__arm__)
typedef struct { uint64_t q, r; } uqr;
typedef struct { int64_t q, r; } sqr;
/* As funcoes __aeabi_*ldivmod retornam o par em r0-r3: usamos wrappers em assembly. */
uint64_t __mvm_udivmod(uint64_t n, uint64_t d, uint64_t *r) { return udivmod64(n, d, r); }
int64_t __mvm_sdivmod(int64_t n, int64_t d, int64_t *r) { return sdivmod64(n, d, r); }
__asm__(
    ".global __aeabi_uldivmod\n.type __aeabi_uldivmod, %function\n"
    "__aeabi_uldivmod:\n"
    "  push {r4, lr}\n"
    "  sub sp, sp, #16\n"
    "  add r4, sp, #8\n"
    "  str r4, [sp]\n"
    "  bl __mvm_udivmod\n"
    "  ldr r2, [sp, #8]\n"
    "  ldr r3, [sp, #12]\n"
    "  add sp, sp, #16\n"
    "  pop {r4, pc}\n"
    ".global __aeabi_ldivmod\n.type __aeabi_ldivmod, %function\n"
    "__aeabi_ldivmod:\n"
    "  push {r4, lr}\n"
    "  sub sp, sp, #16\n"
    "  add r4, sp, #8\n"
    "  str r4, [sp]\n"
    "  bl __mvm_sdivmod\n"
    "  ldr r2, [sp, #8]\n"
    "  ldr r3, [sp, #12]\n"
    "  add sp, sp, #16\n"
    "  pop {r4, pc}\n");

__attribute__((pcs("aapcs"))) int64_t __aeabi_d2lz(double d)
{
    if (d != d) return 0;
    if (d >= 9223372036854775808.0) return INT64_MAX;
    if (d < -9223372036854775808.0) return INT64_MIN;
    int neg = d < 0;
    if (neg) d = -d;
    uint32_t hi = (uint32_t)(d / 4294967296.0);
    double rest = d - (double)hi * 4294967296.0;
    uint64_t v = ((uint64_t)hi << 32) | (uint32_t)rest;
    return neg ? -(int64_t)v : (int64_t)v;
}

__attribute__((pcs("aapcs"))) uint64_t __aeabi_d2ulz(double d)
{
    if (!(d > 0)) return 0;
    if (d >= 18446744073709551616.0) return ~0ULL;
    uint32_t hi = (uint32_t)(d / 4294967296.0);
    double rest = d - (double)hi * 4294967296.0;
    return ((uint64_t)hi << 32) | (uint32_t)rest;
}

__attribute__((pcs("aapcs"))) double __aeabi_ul2d(uint64_t v)
{
    return (double)(uint32_t)(v >> 32) * 4294967296.0 + (double)(uint32_t)v;
}

__attribute__((pcs("aapcs"))) double __aeabi_l2d(int64_t v)
{
    return v < 0 ? -__aeabi_ul2d(0 - (uint64_t)v) : __aeabi_ul2d((uint64_t)v);
}

void *memcpy(void *, const void *, unsigned);
void *memset(void *, int, unsigned);
void *memmove(void *, const void *, unsigned);
void __aeabi_memcpy(void *d, const void *s, unsigned n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, unsigned n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, unsigned n) { memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, unsigned n) { memmove(d, s, n); }
void __aeabi_memset(void *d, unsigned n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, unsigned n, int c) { memset(d, c, n); }
void __aeabi_memset8(void *d, unsigned n, int c) { memset(d, c, n); }
void __aeabi_memclr(void *d, unsigned n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, unsigned n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, unsigned n) { memset(d, 0, n); }
#endif

#if defined(__i386__)
uint64_t __udivdi3(uint64_t a, uint64_t b) { return udivmod64(a, b, 0); }
uint64_t __umoddi3(uint64_t a, uint64_t b) { uint64_t r; udivmod64(a, b, &r); return r; }
int64_t __divdi3(int64_t a, int64_t b) { return sdivmod64(a, b, 0); }
int64_t __moddi3(int64_t a, int64_t b) { int64_t r; sdivmod64(a, b, &r); return r; }
uint64_t __udivmoddi4(uint64_t a, uint64_t b, uint64_t *r) { return udivmod64(a, b, r); }
int64_t __divmoddi4(int64_t a, int64_t b, int64_t *r) { return sdivmod64(a, b, r); }
#endif

/* floor/ceil/trunc por manipulacao de bits (como no musl) */
double trunc(double x)
{
    union { double f; uint64_t i; } u = {x};
    int e = (int)(u.i >> 52 & 0x7ff) - 0x3ff + 12;
    uint64_t m;
    if (e >= 52 + 12) return x;
    if (e < 12) e = 1;
    m = -1ULL >> e;
    if ((u.i & m) == 0) return x;
    u.i &= ~m;
    return u.f;
}

double floor(double x)
{
    double t = trunc(x);
    return (t != x && x < 0) ? t - 1.0 : t;
}

double ceil(double x)
{
    double t = trunc(x);
    return (t != x && x > 0) ? t + 1.0 : t;
}
