/* AArch64: ponto flutuante escalar e Advanced SIMD (NEON). */
#include "cpu_arm64.h"

#include <math.h>

#define VR(n) (&c->v[n])

static inline uint64_t mask_bits(unsigned bits) { return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1); }

static inline uint64_t eget(const a64_vreg *v, unsigned i, unsigned sz)
{
    switch (sz) {
    case 0: return v->b[i];
    case 1: return v->h[i];
    case 2: return v->s[i];
    default: return v->d[i];
    }
}

static inline int64_t egets(const a64_vreg *v, unsigned i, unsigned sz)
{
    switch (sz) {
    case 0: return (int8_t)v->b[i];
    case 1: return (int16_t)v->h[i];
    case 2: return (int32_t)v->s[i];
    default: return (int64_t)v->d[i];
    }
}

static inline void eset(a64_vreg *v, unsigned i, unsigned sz, uint64_t x)
{
    switch (sz) {
    case 0: v->b[i] = (uint8_t)x; break;
    case 1: v->h[i] = (uint16_t)x; break;
    case 2: v->s[i] = (uint32_t)x; break;
    default: v->d[i] = x; break;
    }
}

static inline void qc(a64_cpu *c) { c->fpsr |= 1u << 27; }

/* satura um valor com sinal/sem sinal para 'bits' */
static uint64_t sat_s(a64_cpu *c, __int128 v, unsigned bits)
{
    __int128 max = ((__int128)1 << (bits - 1)) - 1, min = -((__int128)1 << (bits - 1));
    if (v > max) { qc(c); v = max; }
    if (v < min) { qc(c); v = min; }
    return (uint64_t)v & mask_bits(bits);
}

static uint64_t sat_u(a64_cpu *c, __int128 v, unsigned bits)
{
    __int128 max = bits >= 64 ? (__int128)UINT64_MAX : (((__int128)1 << bits) - 1);
    if (v > max) { qc(c); v = max; }
    if (v < 0) { qc(c); v = 0; }
    return (uint64_t)v;
}

/* ------------------------------------------------------- ajudantes FP */

/* tipo: 0 = simples, 1 = dupla, 2 = meia */
static inline double fget(const a64_vreg *v, unsigned i, unsigned t)
{
    if (t == 0) return v->fs[i];
    if (t == 1) return v->fd[i];
    return f16_to_f32(v->h[i]);
}

static inline void fset(a64_vreg *v, unsigned i, unsigned t, double x)
{
    if (t == 0) v->fs[i] = (float)x;
    else if (t == 1) v->fd[i] = x;
    else v->h[i] = f32_to_f16((float)x);
}

static inline void fp_write(a64_cpu *c, unsigned r, unsigned t, double x)
{
    a64_vreg *v = VR(r);
    v->d[0] = 0;
    v->d[1] = 0;
    fset(v, 0, t, x);
}

static inline double round_t(double x, unsigned t)
{
    return t == 0 ? (double)(float)x : x;
}

static double fmax_a(double a, double b)
{
    if (isnan(a) || isnan(b)) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? b : a;
    return a > b ? a : b;
}

static double fmin_a(double a, double b)
{
    if (isnan(a) || isnan(b)) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? a : b;
    return a < b ? a : b;
}

static double fmaxnm_a(double a, double b)
{
    if (isnan(a) && !isnan(b)) return b;
    if (isnan(b) && !isnan(a)) return a;
    return fmax_a(a, b);
}

static double fminnm_a(double a, double b)
{
    if (isnan(a) && !isnan(b)) return b;
    if (isnan(b) && !isnan(a)) return a;
    return fmin_a(a, b);
}

static uint32_t fcmp_flags(double a, double b)
{
    if (isnan(a) || isnan(b)) return 3;
    if (a == b) return 6;
    if (a < b) return 8;
    return 2;
}

/* modos: 0 N (par), 1 P, 2 M, 3 Z, 4 A (afastado do zero) */
static double fp_round(double x, int rm)
{
    switch (rm) {
    case 0: return nearbyint(x);
    case 1: return ceil(x);
    case 2: return floor(x);
    case 3: return trunc(x);
    default: return round(x);
    }
}

static uint64_t fp_to_int(double x, int rm, bool uns, unsigned bits)
{
    if (isnan(x)) return 0;
    double r = fp_round(x, rm);
    if (uns) {
        if (r <= 0) return 0;
        double lim = bits == 64 ? 18446744073709551616.0 : bits == 32 ? 4294967296.0 : 65536.0;
        if (r >= lim) return mask_bits(bits);
        return (uint64_t)r;
    }
    double lim = bits == 64 ? 9223372036854775808.0 : bits == 32 ? 2147483648.0 : 32768.0;
    if (r >= lim) return mask_bits(bits - 1);
    if (r < -lim) return (1ULL << (bits - 1)) & mask_bits(bits);
    return (uint64_t)(int64_t)r & mask_bits(bits);
}

static double int_to_fp(uint64_t v, bool uns, unsigned bits, unsigned t)
{
    if (t == 0) {
        if (uns) return (float)(v & mask_bits(bits));
        return (float)(int64_t)sext64(v, bits);
    }
    if (uns) return (double)(v & mask_bits(bits));
    return (double)(int64_t)sext64(v, bits);
}

static double fp_imm8(unsigned imm8)
{
    uint64_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    uint64_t exp = ((b6 ^ 1) << 10) | ((b6 ? 0xffULL : 0) << 2) | ((imm8 >> 4) & 3);
    uint64_t bits = (sign << 63) | (exp << 52) | ((uint64_t)(imm8 & 0xf) << 48);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static inline int fpcr_rmode(a64_cpu *c) { return (int)((c->fpcr >> 22) & 3); }

static double frecpe(double x) { return 1.0 / x; }
static double frsqrte(double x) { return 1.0 / sqrt(x); }

/* ------------------------------------------------ FP escalar (dados) */

static void fp_dp(a64_cpu *c, uint32_t insn)
{
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    unsigned ftype = BITS(insn, 23, 22);
    int t = ftype == 0 ? 0 : ftype == 1 ? 1 : ftype == 3 ? 2 : -1;

    if (BIT(insn, 24)) { /* 3 fontes */
        if (t < 0 || BIT(insn, 31) || BIT(insn, 29))
            a64_undef(c);
        unsigned ra = BITS(insn, 14, 10), o1 = BIT(insn, 21), o0 = BIT(insn, 15);
        double n = fget(VR(rn), 0, (unsigned)t), m = fget(VR(rm), 0, (unsigned)t), a = fget(VR(ra), 0, (unsigned)t);
        if (o1) a = -a;
        if (o0) n = -n;
        double r = t == 0 ? (double)fmaf((float)n, (float)m, (float)a) : fma(n, m, a);
        fp_write(c, rd, (unsigned)t, r);
        return;
    }
    if (!BIT(insn, 21)) { /* conversao ponto fixo */
        bool sf = BIT(insn, 31);
        unsigned rmode = BITS(insn, 20, 19), opc = BITS(insn, 18, 16), scale = BITS(insn, 15, 10);
        unsigned fbits = 64 - scale, ibits = sf ? 64 : 32;
        if (t < 0 || (!sf && scale < 32))
            a64_undef(c);
        if (rmode == 0 && (opc == 2 || opc == 3)) {
            double v = int_to_fp(c->x[rn], opc == 3, ibits, 1);
            if (t == 0) {
                /* conversao direta para float para evitar arredondamento duplo */
                v = opc == 3 ? (double)((float)(c->x[rn] & mask_bits(ibits))) :
                               (double)((float)(int64_t)sext64(c->x[rn], ibits));
                v = (double)(float)ldexp(v, -(int)fbits);
            } else {
                v = ldexp(v, -(int)fbits);
            }
            fp_write(c, rd, (unsigned)t, v);
        } else if (rmode == 3 && opc < 2) {
            double v = ldexp(fget(VR(rn), 0, (unsigned)t), (int)fbits);
            uint64_t r = fp_to_int(v, 3, opc == 1, ibits);
            if (rd != 31) c->x[rd] = r;
        } else {
            a64_undef(c);
        }
        return;
    }
    switch (BITS(insn, 11, 10)) {
    case 1: { /* FCCMP */
        if (t < 0) a64_undef(c);
        if (arm_cond(BITS(insn, 15, 12), c->nzcv))
            c->nzcv = fcmp_flags(fget(VR(rn), 0, (unsigned)t), fget(VR(rm), 0, (unsigned)t));
        else
            c->nzcv = insn & 0xf;
        return;
    }
    case 2: { /* 2 fontes */
        if (t < 0) a64_undef(c);
        double a = fget(VR(rn), 0, (unsigned)t), b = fget(VR(rm), 0, (unsigned)t), r;
        switch (BITS(insn, 15, 12)) {
        case 0: r = a * b; break;
        case 1: r = a / b; break;
        case 2: r = a + b; break;
        case 3: r = a - b; break;
        case 4: r = fmax_a(a, b); break;
        case 5: r = fmin_a(a, b); break;
        case 6: r = fmaxnm_a(a, b); break;
        case 7: r = fminnm_a(a, b); break;
        case 8: r = -(a * b); break;
        default: a64_undef(c);
        }
        fp_write(c, rd, (unsigned)t, r);
        return;
    }
    case 3: { /* FCSEL */
        if (t < 0) a64_undef(c);
        unsigned src = arm_cond(BITS(insn, 15, 12), c->nzcv) ? rn : rm;
        a64_vreg v = c->v[src], *d = VR(rd);
        d->d[0] = d->d[1] = 0;
        if (t == 1) d->d[0] = v.d[0];
        else if (t == 0) d->s[0] = v.s[0];
        else d->h[0] = v.h[0];
        return;
    }
    }
    unsigned b1512 = BITS(insn, 15, 12);
    if (b1512 & 1) { /* FMOV imediato */
        if (t < 0 || BITS(insn, 9, 5)) a64_undef(c);
        fp_write(c, rd, (unsigned)t, fp_imm8(BITS(insn, 20, 13)));
        return;
    }
    if ((b1512 & 3) == 2) { /* comparacao */
        if (t < 0 || BITS(insn, 15, 14)) a64_undef(c);
        unsigned opc2 = insn & 0x1f;
        double a = fget(VR(rn), 0, (unsigned)t);
        double b = (opc2 & 8) ? 0.0 : fget(VR(rm), 0, (unsigned)t);
        c->nzcv = fcmp_flags(a, b);
        return;
    }
    if ((b1512 & 7) == 4) { /* 1 fonte */
        unsigned opc = BITS(insn, 20, 15);
        if (t < 0) a64_undef(c);
        double a = fget(VR(rn), 0, (unsigned)t);
        switch (opc) {
        case 0: { /* FMOV registrador (copia de bits) */
            a64_vreg v = c->v[rn], *d = VR(rd);
            d->d[0] = d->d[1] = 0;
            if (t == 1) d->d[0] = v.d[0];
            else if (t == 0) d->s[0] = v.s[0];
            else d->h[0] = v.h[0];
            return;
        }
        case 1: { /* FABS (bits) */
            a64_vreg v = c->v[rn], *d = VR(rd);
            d->d[0] = d->d[1] = 0;
            if (t == 1) d->d[0] = v.d[0] & ~(1ULL << 63);
            else if (t == 0) d->s[0] = v.s[0] & ~(1u << 31);
            else d->h[0] = v.h[0] & 0x7fff;
            return;
        }
        case 2: { /* FNEG (bits) */
            a64_vreg v = c->v[rn], *d = VR(rd);
            d->d[0] = d->d[1] = 0;
            if (t == 1) d->d[0] = v.d[0] ^ (1ULL << 63);
            else if (t == 0) d->s[0] = v.s[0] ^ (1u << 31);
            else d->h[0] = v.h[0] ^ 0x8000;
            return;
        }
        case 3:
            fp_write(c, rd, (unsigned)t, t == 0 ? (double)sqrtf((float)a) : sqrt(a));
            return;
        case 4: case 5: case 7: { /* FCVT */
            unsigned dt = opc == 4 ? 0 : opc == 5 ? 1 : 2;
            if (dt == (unsigned)t) a64_undef(c);
            fp_write(c, rd, dt, a);
            return;
        }
        case 8: case 9: case 10: case 11: case 12: case 14: case 15: {
            int rm2 = opc == 8 ? 0 : opc == 9 ? 1 : opc == 10 ? 2 : opc == 11 ? 3 : opc == 12 ? 4 : fpcr_rmode(c);
            fp_write(c, rd, (unsigned)t, fp_round(a, rm2));
            return;
        }
        default:
            a64_undef(c);
        }
    }
    if (b1512 == 0) { /* FP <-> inteiro */
        bool sf = BIT(insn, 31);
        unsigned rmode = BITS(insn, 20, 19), opc = BITS(insn, 18, 16);
        unsigned ibits = sf ? 64 : 32;
        if (opc == 6 || opc == 7) { /* FMOV geral */
            if (rmode == 1) {
                if (!sf || ftype != 2) a64_undef(c);
                if (opc == 6) {
                    if (rd != 31) c->x[rd] = c->v[rn].d[1];
                } else {
                    c->v[rd].d[1] = c->x[rn];
                }
                return;
            }
            if (rmode != 0 || ftype == 2) a64_undef(c);
            if (opc == 6) {
                uint64_t v = t == 1 ? c->v[rn].d[0] : t == 0 ? c->v[rn].s[0] : c->v[rn].h[0];
                if (!sf && t == 1) a64_undef(c);
                if (rd != 31) c->x[rd] = v;
            } else {
                a64_vreg *d = VR(rd);
                uint64_t v = c->x[rn];
                d->d[0] = d->d[1] = 0;
                if (t == 1) d->d[0] = v;
                else if (t == 0) d->s[0] = (uint32_t)v;
                else d->h[0] = (uint16_t)v;
            }
            return;
        }
        if (t < 0) a64_undef(c);
        if (opc == 2 || opc == 3) { /* SCVTF/UCVTF */
            if (rmode) a64_undef(c);
            fp_write(c, rd, (unsigned)t, int_to_fp(c->x[rn], opc == 3, ibits, (unsigned)t));
            return;
        }
        int rm2;
        if (opc < 2)
            rm2 = (int)rmode;
        else if (opc == 4 || opc == 5) {
            if (rmode) a64_undef(c);
            rm2 = 4;
        } else
            a64_undef(c);
        uint64_t r = fp_to_int(fget(VR(rn), 0, (unsigned)t), rm2, opc & 1, ibits);
        if (rd != 31) c->x[rd] = r;
        return;
    }
    a64_undef(c);
}

/* ---------------------------------------------------- SIMD: 3 iguais */

static uint64_t shl_op(a64_cpu *c, uint64_t a, int64_t sa, int8_t sh, unsigned bits, bool u,
                       bool round, bool sat)
{
    uint64_t m = mask_bits(bits);
    if (sh >= 0) {
        if (sat) {
            __int128 v = u ? (__int128)a : (__int128)sa;
            if (sh >= (int)bits + 1) {
                if (v == 0) return 0;
                return u ? sat_u(c, (__int128)1 << 100, bits) : sat_s(c, v > 0 ? ((__int128)1 << 100) : -((__int128)1 << 100), bits);
            }
            v <<= sh;
            return u ? sat_u(c, v, bits) : sat_s(c, v, bits);
        }
        return sh >= (int)bits ? 0 : (a << sh) & m;
    }
    int s = -sh;
    if (u) {
        if (s > (int)bits) return 0;
        unsigned __int128 v = a;
        if (round) v += (unsigned __int128)1 << (s - 1);
        return (uint64_t)(v >> s) & m;
    }
    if (s > (int)bits) s = (int)bits;
    __int128 v = sa;
    if (round) v += (__int128)1 << (s - 1);
    return (uint64_t)(v >> s) & m;
}

static uint64_t int3_op(a64_cpu *c, unsigned opc, bool u, unsigned sz, uint64_t a, uint64_t b, uint64_t d)
{
    unsigned bits = 8u << sz;
    uint64_t m = mask_bits(bits);
    int64_t sa = (int64_t)sext64(a, bits), sb = (int64_t)sext64(b, bits);
    switch (opc) {
    case 0x00: return u ? (uint64_t)(((unsigned __int128)a + b) >> 1) & m : (uint64_t)(((__int128)sa + sb) >> 1) & m;
    case 0x01: return u ? sat_u(c, (__int128)a + b, bits) : sat_s(c, (__int128)sa + sb, bits);
    case 0x02: return u ? (uint64_t)(((unsigned __int128)a + b + 1) >> 1) & m : (uint64_t)(((__int128)sa + sb + 1) >> 1) & m;
    case 0x04: return u ? (uint64_t)(((__int128)a - (__int128)b) >> 1) & m : (uint64_t)(((__int128)sa - sb) >> 1) & m;
    case 0x05: return u ? sat_u(c, (__int128)a - (__int128)b, bits) : sat_s(c, (__int128)sa - sb, bits);
    case 0x06: return (u ? a > b : sa > sb) ? m : 0;
    case 0x07: return (u ? a >= b : sa >= sb) ? m : 0;
    case 0x08: return shl_op(c, a, sa, (int8_t)b, bits, u, false, false);
    case 0x09: return shl_op(c, a, sa, (int8_t)b, bits, u, false, true);
    case 0x0a: return shl_op(c, a, sa, (int8_t)b, bits, u, true, false);
    case 0x0b: return shl_op(c, a, sa, (int8_t)b, bits, u, true, true);
    case 0x0c: return u ? (a > b ? a : b) : (uint64_t)(sa > sb ? sa : sb) & m;
    case 0x0d: return u ? (a < b ? a : b) : (uint64_t)(sa < sb ? sa : sb) & m;
    case 0x0e: return u ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa) & m;
    case 0x0f: return (d + (u ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa))) & m;
    case 0x10: return (u ? a - b : a + b) & m;
    case 0x11: return u ? (a == b ? m : 0) : ((a & b) ? m : 0);
    case 0x12: return (u ? d - a * b : d + a * b) & m;
    case 0x13:
        if (u) { /* PMUL */
            uint64_t r = 0;
            for (unsigned i = 0; i < bits; i++)
                if (b & (1ULL << i)) r ^= a << i;
            return r & m;
        }
        return (a * b) & m;
    case 0x16: {
        __int128 p = (__int128)2 * sa * sb;
        if (u) p += (__int128)1 << (bits - 1);
        return sat_s(c, p >> bits, bits);
    }
    default:
        a64_undef(c);
    }
}

static void simd_fp3(a64_cpu *c, uint32_t insn, bool scalar)
{
    bool q = BIT(insn, 30), u = BIT(insn, 29), a = BIT(insn, 23), sz = BIT(insn, 22);
    unsigned opc = BITS(insn, 15, 11), rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    unsigned t = sz ? 1 : 0, esz = sz ? 3 : 2;
    if (!scalar && sz && !q) a64_undef(c);
    unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> esz;
    a64_vreg n = c->v[rn], m = c->v[rm], d = c->v[rd], r = {{0}};
    unsigned key = (u << 6) | (a << 5) | opc;
    bool pair = !scalar && (key == ((1u << 6) | 0x18) || key == ((1u << 6) | 0x1a) || key == ((1u << 6) | 0x1e) ||
                            key == ((1u << 6) | (1u << 5) | 0x18) || key == ((1u << 6) | (1u << 5) | 0x1e));
    for (unsigned i = 0; i < ne; i++) {
        double x, y;
        if (pair) {
            unsigned j = 2 * i;
            x = j < ne ? fget(&n, j, t) : fget(&m, j - ne, t);
            y = j + 1 < ne ? fget(&n, j + 1, t) : fget(&m, j + 1 - ne, t);
        } else {
            x = fget(&n, i, t);
            y = fget(&m, i, t);
        }
        double acc = fget(&d, i, t), res = 0;
        bool is_mask = false;
        uint64_t mres = 0, all = mask_bits(8u << esz);
        switch (key) {
        case 0x18: res = fmaxnm_a(x, y); break;
        case 0x19: res = t == 0 ? (double)fmaf((float)x, (float)y, (float)acc) : fma(x, y, acc); break;
        case 0x1a: res = x + y; break;
        case 0x1b: res = x * y; break; /* FMULX */
        case 0x1c: is_mask = true; mres = x == y ? all : 0; break;
        case 0x1e: res = fmax_a(x, y); break;
        case 0x1f: res = 2.0 - x * y; break;
        case 0x38: res = fminnm_a(x, y); break;
        case 0x39: res = t == 0 ? (double)fmaf(-(float)x, (float)y, (float)acc) : fma(-x, y, acc); break;
        case 0x3a: res = x - y; break;
        case 0x3e: res = fmin_a(x, y); break;
        case 0x3f: res = (3.0 - x * y) / 2.0; break;
        case 0x58: res = fmaxnm_a(x, y); break;
        case 0x5a: res = x + y; break;
        case 0x5b: res = x * y; break;
        case 0x5c: is_mask = true; mres = x >= y ? all : 0; break;
        case 0x5d: is_mask = true; mres = fabs(x) >= fabs(y) ? all : 0; break;
        case 0x5e: res = fmax_a(x, y); break;
        case 0x5f: res = x / y; break;
        case 0x78: res = fminnm_a(x, y); break;
        case 0x7a: res = fabs(x - y); break;
        case 0x7c: is_mask = true; mres = x > y ? all : 0; break;
        case 0x7d: is_mask = true; mres = fabs(x) > fabs(y) ? all : 0; break;
        case 0x7e: res = fmin_a(x, y); break;
        default: a64_undef(c);
        }
        if (is_mask)
            eset(&r, i, esz, mres);
        else
            fset(&r, i, t, round_t(res, t));
    }
    c->v[rd] = r;
}

static void simd_3same(a64_cpu *c, uint32_t insn, bool scalar)
{
    unsigned opc = BITS(insn, 15, 11);
    if (opc >= 0x18) {
        simd_fp3(c, insn, scalar);
        return;
    }
    bool q = BIT(insn, 30), u = BIT(insn, 29);
    unsigned sz = BITS(insn, 23, 22), rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    a64_vreg n = c->v[rn], m = c->v[rm], d = c->v[rd], r = {{0}};
    if (opc == 0x03) { /* logicos */
        for (int i = 0; i < 2; i++) {
            uint64_t x = n.d[i], y = m.d[i], z = d.d[i], v;
            switch ((u << 2) | sz) {
            case 0: v = x & y; break;
            case 1: v = x & ~y; break;
            case 2: v = x | y; break;
            case 3: v = x | ~y; break;
            case 4: v = x ^ y; break;
            case 5: v = (z & x) | (~z & y); break;  /* BSL */
            case 6: v = (z & ~y) | (x & y); break;  /* BIT */
            default: v = (z & y) | (x & ~y); break; /* BIF */
            }
            r.d[i] = v;
        }
        if (!q) r.d[1] = 0;
        c->v[rd] = r;
        return;
    }
    if (sz == 3 && !q && !scalar) a64_undef(c);
    unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> sz;
    if (opc == 0x14 || opc == 0x15 || opc == 0x17) { /* pares */
        if (scalar) a64_undef(c);
        for (unsigned i = 0; i < ne; i++) {
            unsigned j = 2 * i;
            const a64_vreg *src = j < ne ? &n : &m;
            unsigned k = j < ne ? j : j - ne;
            uint64_t x = eget(src, k, sz), y = eget(src, k + 1, sz);
            uint64_t v;
            if (opc == 0x17)
                v = x + y;
            else
                v = int3_op(c, opc == 0x14 ? 0x0c : 0x0d, u, sz, x, y, 0);
            eset(&r, i, sz, v);
        }
    } else {
        for (unsigned i = 0; i < ne; i++)
            eset(&r, i, sz, int3_op(c, opc, u, sz, eget(&n, i, sz), eget(&m, i, sz), eget(&d, i, sz)));
    }
    if (!q && !scalar) r.d[1] = 0;
    c->v[rd] = r;
}

/* ------------------------------------------------ SIMD: 2 reg misc */

static uint64_t rev_bits8(uint64_t x)
{
    uint8_t b = (uint8_t)x, r = 0;
    for (int i = 0; i < 8; i++)
        if (b & (1u << i)) r |= (uint8_t)(1u << (7 - i));
    return r;
}

static unsigned clz_n(uint64_t x, unsigned bits)
{
    if (!x) return bits;
    return (unsigned)__builtin_clzll(x) - (64 - bits);
}

static void simd_2misc_fp(a64_cpu *c, uint32_t insn, bool scalar)
{
    bool q = BIT(insn, 30), u = BIT(insn, 29), a = BIT(insn, 23), sz = BIT(insn, 22);
    unsigned opc = BITS(insn, 16, 12), rd = insn & 31, rn = (insn >> 5) & 31;
    a64_vreg n = c->v[rn], r = {{0}};
    unsigned key = (u << 6) | (a << 5) | opc;

    /* conversoes com estreitamento/alargamento */
    if (key == 0x16 || key == 0x17 || key == 0x56) {
        if (key == 0x17) { /* FCVTL */
            unsigned off = q ? (sz ? 2 : 4) : 0;
            unsigned ne = sz ? 2 : 4;
            for (unsigned i = 0; i < ne; i++) {
                if (sz) r.fd[i] = n.fs[off + i];
                else r.fs[i] = f16_to_f32(n.h[off + i]);
            }
            c->v[rd] = r;
            return;
        }
        /* FCVTN / FCVTXN */
        a64_vreg d = c->v[rd];
        unsigned ne = sz ? 2 : 4;
        if (scalar) ne = 1;
        unsigned off = (q && !scalar) ? ne : 0;
        r = q && !scalar ? d : r;
        if (q && !scalar) r.d[1] = 0, r.d[0] = d.d[0];
        for (unsigned i = 0; i < ne; i++) {
            if (sz) r.fs[off + i] = (float)n.fd[i];
            else r.h[off + i] = f32_to_f16(n.fs[i]);
        }
        c->v[rd] = r;
        return;
    }
    unsigned t = sz ? 1 : 0, esz = sz ? 3 : 2;
    if (!scalar && sz && !q) a64_undef(c);
    unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> esz;
    uint64_t all = mask_bits(8u << esz);
    for (unsigned i = 0; i < ne; i++) {
        double x = fget(&n, i, t);
        uint64_t raw = eget(&n, i, esz);
        switch (key) {
        case 0x18: fset(&r, i, t, fp_round(x, 0)); break;
        case 0x38: fset(&r, i, t, fp_round(x, 1)); break;
        case 0x19: fset(&r, i, t, fp_round(x, 2)); break;
        case 0x39: fset(&r, i, t, fp_round(x, 3)); break;
        case 0x58: fset(&r, i, t, fp_round(x, 4)); break;
        case 0x59: case 0x79: fset(&r, i, t, fp_round(x, fpcr_rmode(c))); break;
        case 0x1a: eset(&r, i, esz, fp_to_int(x, 0, false, 8u << esz)); break;
        case 0x3a: eset(&r, i, esz, fp_to_int(x, 1, false, 8u << esz)); break;
        case 0x1b: eset(&r, i, esz, fp_to_int(x, 2, false, 8u << esz)); break;
        case 0x3b: eset(&r, i, esz, fp_to_int(x, 3, false, 8u << esz)); break;
        case 0x1c: eset(&r, i, esz, fp_to_int(x, 4, false, 8u << esz)); break;
        case 0x5a: eset(&r, i, esz, fp_to_int(x, 0, true, 8u << esz)); break;
        case 0x7a: eset(&r, i, esz, fp_to_int(x, 1, true, 8u << esz)); break;
        case 0x5b: eset(&r, i, esz, fp_to_int(x, 2, true, 8u << esz)); break;
        case 0x7b: eset(&r, i, esz, fp_to_int(x, 3, true, 8u << esz)); break;
        case 0x5c: eset(&r, i, esz, fp_to_int(x, 4, true, 8u << esz)); break;
        case 0x1d: fset(&r, i, t, int_to_fp(raw, false, 8u << esz, t)); break;
        case 0x5d: fset(&r, i, t, int_to_fp(raw, true, 8u << esz, t)); break;
        case 0x3d: fset(&r, i, t, round_t(frecpe(x), t)); break;
        case 0x7d: fset(&r, i, t, round_t(frsqrte(x), t)); break;
        case 0x3c: eset(&r, i, esz, (uint32_t)raw < 0x80000000u ? 0xffffffffu : 0x7fffffffu); break; /* URECPE aprox */
        case 0x7c: eset(&r, i, esz, 0xffffffffu); break; /* URSQRTE aprox */
        case 0x2c: eset(&r, i, esz, x > 0 ? all : 0); break;
        case 0x2d: eset(&r, i, esz, x == 0 ? all : 0); break;
        case 0x2e: eset(&r, i, esz, x < 0 ? all : 0); break;
        case 0x6c: eset(&r, i, esz, x >= 0 ? all : 0); break;
        case 0x6d: eset(&r, i, esz, x <= 0 ? all : 0); break;
        case 0x2f: eset(&r, i, esz, raw & ~(1ULL << ((8u << esz) - 1))); break;
        case 0x6f: eset(&r, i, esz, raw ^ (1ULL << ((8u << esz) - 1))); break;
        case 0x7f: fset(&r, i, t, t == 0 ? (double)sqrtf((float)x) : sqrt(x)); break;
        default: a64_undef(c);
        }
    }
    c->v[rd] = r;
}

static void simd_2misc(a64_cpu *c, uint32_t insn, bool scalar)
{
    unsigned opc = BITS(insn, 16, 12);
    if ((opc >= 0x0c && opc <= 0x0f) || opc >= 0x16) {
        simd_2misc_fp(c, insn, scalar);
        return;
    }
    bool q = BIT(insn, 30), u = BIT(insn, 29);
    unsigned sz = BITS(insn, 23, 22), rd = insn & 31, rn = (insn >> 5) & 31;
    unsigned bits = 8u << sz;
    uint64_t m = mask_bits(bits);
    a64_vreg n = c->v[rn], d = c->v[rd], r = {{0}};
    unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> sz;

    switch ((u << 5) | opc) {
    case 0x12: case 0x14: case 0x32: case 0x34: { /* XTN, SQXTN, SQXTUN, UQXTN */
        if (sz == 3) a64_undef(c);
        unsigned nn = scalar ? 1 : 8u >> sz;
        unsigned off = (q && !scalar) ? nn : 0;
        if (q && !scalar) r.d[0] = d.d[0];
        for (unsigned i = 0; i < nn; i++) {
            unsigned wsz = sz + 1, wbits = 8u << wsz;
            uint64_t v = eget(&n, i, wsz);
            int64_t sv = (int64_t)sext64(v, wbits);
            uint64_t o;
            switch ((u << 5) | opc) {
            case 0x12: o = v & m; break;
            case 0x14: o = sat_s(c, sv, bits); break;
            case 0x32: o = sat_u(c, sv, bits); break;
            default: o = sat_u(c, v, bits); break;
            }
            eset(&r, off + i, sz, o);
        }
        c->v[rd] = r;
        return;
    }
    case 0x33: { /* SHLL */
        unsigned nn = 8u >> sz, off = q ? nn : 0;
        for (unsigned i = 0; i < nn; i++)
            eset(&r, i, sz + 1, eget(&n, off + i, sz) << bits);
        c->v[rd] = r;
        return;
    }
    case 0x02: case 0x06: case 0x22: case 0x26: { /* [SU]ADDLP, [SU]ADALP */
        if (sz == 3) a64_undef(c);
        unsigned nn = ne / 2;
        for (unsigned i = 0; i < nn; i++) {
            uint64_t v = u ? eget(&n, 2 * i, sz) + eget(&n, 2 * i + 1, sz)
                           : (uint64_t)(egets(&n, 2 * i, sz) + egets(&n, 2 * i + 1, sz));
            if (opc == 0x06)
                v += eget(&d, i, sz + 1);
            eset(&r, i, sz + 1, v);
        }
        if (!q) r.d[1] = 0;
        c->v[rd] = r;
        return;
    }
    }

    for (unsigned i = 0; i < ne; i++) {
        uint64_t a = eget(&n, i, sz);
        int64_t sa = egets(&n, i, sz);
        uint64_t v;
        switch ((u << 5) | opc) {
        case 0x00: /* REV64 */
        case 0x20: /* REV32 */
        case 0x01: { /* REV16 */
            unsigned grp = opc == 0x01 ? 16 : (u ? 32 : 64);
            unsigned per = grp / bits;
            if (per <= 1) a64_undef(c);
            unsigned base = i / per * per, idx = i % per;
            v = eget(&n, base + per - 1 - idx, sz);
            break;
        }
        case 0x03: v = sat_s(c, (__int128)sa + (__int128)eget(&d, i, sz), bits); break; /* SUQADD */
        case 0x23: v = sat_u(c, (__int128)a + (__int128)egets(&d, i, sz), bits); break; /* USQADD */
        case 0x04: { /* CLS */
            uint64_t x = (a ^ (a >> 1)) & (m >> 1);
            v = clz_n(x, bits) - 1;
            break;
        }
        case 0x24: v = clz_n(a, bits); break;
        case 0x05: v = (uint64_t)__builtin_popcountll(a); break; /* CNT */
        case 0x25: v = sz == 0 ? (~a & m) : rev_bits8(a); break; /* NOT/RBIT */
        case 0x07: v = sat_s(c, sa < 0 ? -(__int128)sa : sa, bits); break;
        case 0x27: v = sat_s(c, -(__int128)sa, bits); break;
        case 0x08: v = sa > 0 ? m : 0; break;
        case 0x09: v = a == 0 ? m : 0; break;
        case 0x0a: v = sa < 0 ? m : 0; break;
        case 0x0b: v = (uint64_t)(sa < 0 ? -sa : sa) & m; break;
        case 0x28: v = sa >= 0 ? m : 0; break;
        case 0x29: v = sa <= 0 ? m : 0; break;
        case 0x2b: v = (0 - a) & m; break;
        default: a64_undef(c);
        }
        eset(&r, i, sz, v);
    }
    if (!q && !scalar) r.d[1] = 0;
    c->v[rd] = r;
}

/* ------------------------------------------------ SIMD: entre lanes */

static void simd_across(a64_cpu *c, uint32_t insn)
{
    bool q = BIT(insn, 30), u = BIT(insn, 29);
    unsigned sz = BITS(insn, 23, 22), opc = BITS(insn, 16, 12), rd = insn & 31, rn = (insn >> 5) & 31;
    a64_vreg n = c->v[rn], r = {{0}};
    if (opc == 0x0c || opc == 0x0f) { /* FMAXNMV/FMINNMV/FMAXV/FMINV (simples) */
        if (!u || !q || (sz & 1)) a64_undef(c);
        bool mn = sz & 2;
        double acc = n.fs[0];
        for (int i = 1; i < 4; i++) {
            double x = n.fs[i];
            if (opc == 0x0c) acc = mn ? fminnm_a(acc, x) : fmaxnm_a(acc, x);
            else acc = mn ? fmin_a(acc, x) : fmax_a(acc, x);
        }
        r.fs[0] = (float)acc;
        c->v[rd] = r;
        return;
    }
    unsigned ne = (q ? 16u : 8u) >> sz;
    if (sz == 3 || (sz == 2 && !q)) a64_undef(c);
    unsigned bits = 8u << sz;
    switch (opc) {
    case 0x03: { /* [SU]ADDLV */
        __int128 s = 0;
        for (unsigned i = 0; i < ne; i++)
            s += u ? (__int128)eget(&n, i, sz) : (__int128)egets(&n, i, sz);
        eset(&r, 0, sz + 1, (uint64_t)s & mask_bits(bits * 2));
        break;
    }
    case 0x0a: case 0x1a: { /* MAXV/MINV */
        bool mx = opc == 0x0a;
        uint64_t best = eget(&n, 0, sz);
        int64_t sbest = egets(&n, 0, sz);
        for (unsigned i = 1; i < ne; i++) {
            uint64_t x = eget(&n, i, sz);
            int64_t sx = egets(&n, i, sz);
            if (u) {
                if (mx ? x > best : x < best) best = x;
            } else {
                if (mx ? sx > sbest : sx < sbest) sbest = sx;
            }
        }
        eset(&r, 0, sz, u ? best : (uint64_t)sbest & mask_bits(bits));
        break;
    }
    case 0x1b: { /* ADDV */
        if (u) a64_undef(c);
        uint64_t s = 0;
        for (unsigned i = 0; i < ne; i++)
            s += eget(&n, i, sz);
        eset(&r, 0, sz, s & mask_bits(bits));
        break;
    }
    default:
        a64_undef(c);
    }
    c->v[rd] = r;
}

/* ---------------------------------------------------------- copia */

static void simd_copy(a64_cpu *c, uint32_t insn, bool scalar)
{
    bool q = BIT(insn, 30), op = BIT(insn, 29);
    unsigned imm5 = BITS(insn, 20, 16), imm4 = BITS(insn, 14, 11), rd = insn & 31, rn = (insn >> 5) & 31;
    if (!(imm5 & 0xf)) a64_undef(c);
    unsigned sz = (unsigned)__builtin_ctz(imm5);
    unsigned idx = imm5 >> (sz + 1);
    a64_vreg n = c->v[rn];
    if (scalar) { /* DUP (elemento) escalar */
        if (op || imm4) a64_undef(c);
        a64_vreg r = {{0}};
        eset(&r, 0, sz, eget(&n, idx, sz));
        c->v[rd] = r;
        return;
    }
    if (op) { /* INS (elemento) */
        if (!q) a64_undef(c);
        unsigned sidx = imm4 >> sz;
        eset(VR(rd), idx, sz, eget(&n, sidx, sz));
        return;
    }
    switch (imm4) {
    case 0: { /* DUP elemento */
        uint64_t v = eget(&n, idx, sz);
        a64_vreg r = {{0}};
        unsigned ne = (q ? 16u : 8u) >> sz;
        for (unsigned i = 0; i < ne; i++) eset(&r, i, sz, v);
        c->v[rd] = r;
        return;
    }
    case 1: { /* DUP geral */
        uint64_t v = c->x[rn];
        a64_vreg r = {{0}};
        unsigned ne = (q ? 16u : 8u) >> sz;
        for (unsigned i = 0; i < ne; i++) eset(&r, i, sz, v);
        c->v[rd] = r;
        return;
    }
    case 3: /* INS geral */
        if (!q) a64_undef(c);
        eset(VR(rd), idx, sz, c->x[rn]);
        return;
    case 5: case 7: { /* SMOV / UMOV */
        uint64_t v = imm4 == 5 ? (uint64_t)egets(&n, idx, sz) : eget(&n, idx, sz);
        if (imm4 == 5 && !q) v = (uint32_t)v;
        if (rd != 31) c->x[rd] = v;
        return;
    }
    default:
        a64_undef(c);
    }
}

/* ------------------------------------------------ imediato modificado */

static void simd_modimm(a64_cpu *c, uint32_t insn)
{
    bool q = BIT(insn, 30), op = BIT(insn, 29);
    unsigned cmode = BITS(insn, 15, 12), rd = insn & 31;
    unsigned imm8 = (BITS(insn, 18, 16) << 5) | BITS(insn, 9, 5);
    uint64_t imm = 0;
    switch (cmode >> 1) {
    case 0: imm = (uint64_t)imm8 * 0x0000000100000001ULL; break;
    case 1: imm = ((uint64_t)imm8 << 8) * 0x0000000100000001ULL; break;
    case 2: imm = ((uint64_t)imm8 << 16) * 0x0000000100000001ULL; break;
    case 3: imm = ((uint64_t)imm8 << 24) * 0x0000000100000001ULL; break;
    case 4: imm = (uint64_t)imm8 * 0x0001000100010001ULL; break;
    case 5: imm = ((uint64_t)imm8 << 8) * 0x0001000100010001ULL; break;
    case 6:
        if (cmode & 1) imm = (((uint64_t)imm8 << 16) | 0xffff) * 0x0000000100000001ULL;
        else imm = (((uint64_t)imm8 << 8) | 0xff) * 0x0000000100000001ULL;
        break;
    default:
        if (!(cmode & 1) && !op) {
            imm = (uint64_t)imm8 * 0x0101010101010101ULL;
        } else if (!(cmode & 1) && op) {
            for (int i = 0; i < 8; i++)
                if (imm8 & (1u << i)) imm |= 0xffULL << (8 * i);
        } else if (!op) { /* FMOV simples */
            float f = (float)fp_imm8(imm8);
            uint32_t b;
            memcpy(&b, &f, 4);
            imm = (uint64_t)b * 0x0000000100000001ULL;
        } else { /* FMOV dupla */
            if (!q) a64_undef(c);
            double d = fp_imm8(imm8);
            memcpy(&imm, &d, 8);
        }
        break;
    }
    a64_vreg *d = VR(rd);
    bool is_orr_bic = (cmode < 12) && (cmode & 1);
    if (is_orr_bic) {
        if (op) { d->d[0] &= ~imm; d->d[1] &= ~imm; }
        else { d->d[0] |= imm; d->d[1] |= imm; }
    } else {
        if (op && cmode != 14 && cmode != 15) imm = ~imm; /* MVNI */
        d->d[0] = imm;
        d->d[1] = imm;
    }
    if (!q) d->d[1] = 0;
}

/* ---------------------------------------------- deslocamento imediato */

static void simd_shift(a64_cpu *c, uint32_t insn, bool scalar)
{
    bool q = BIT(insn, 30) || scalar, u = BIT(insn, 29);
    unsigned immh = BITS(insn, 22, 19), immb = BITS(insn, 18, 16), opc = BITS(insn, 15, 11);
    unsigned rd = insn & 31, rn = (insn >> 5) & 31;
    if (!immh) a64_undef(c);
    unsigned sz = 31 - (unsigned)__builtin_clz(immh);
    unsigned bits = 8u << sz, immhb = (immh << 3) | immb;
    unsigned shr = 2 * bits - immhb, shl = immhb - bits;
    uint64_t m = mask_bits(bits);
    a64_vreg n = c->v[rn], d = c->v[rd], r = {{0}};

    if (opc == 0x14) { /* SSHLL/USHLL */
        if (sz == 3 || scalar) a64_undef(c);
        unsigned nn = 8u >> sz, off = BIT(insn, 30) ? nn : 0;
        for (unsigned i = 0; i < nn; i++) {
            uint64_t v = u ? eget(&n, off + i, sz) : (uint64_t)egets(&n, off + i, sz);
            eset(&r, i, sz + 1, v << shl);
        }
        c->v[rd] = r;
        return;
    }
    if (opc >= 0x10 && opc <= 0x13) { /* estreitamento */
        if (sz == 3) a64_undef(c);
        /* aqui o elemento de origem e 2x: sz indica o destino */
        unsigned nn = scalar ? 1 : 8u >> sz;
        unsigned off = (!scalar && BIT(insn, 30)) ? nn : 0;
        if (off) r.d[0] = d.d[0];
        bool round = opc & 1;
        unsigned wbits = bits * 2;
        for (unsigned i = 0; i < nn; i++) {
            uint64_t v = eget(&n, i, sz + 1);
            __int128 sv = (__int128)(int64_t)sext64(v, wbits);
            __int128 uv = (__int128)v;
            __int128 rc = round ? ((__int128)1 << (shr - 1)) : 0;
            uint64_t o;
            if (opc <= 0x11 && !u) { /* SHRN/RSHRN */
                o = (uint64_t)((uv + rc) >> shr) & m;
            } else if (opc <= 0x11 && u) { /* SQSHRUN/SQRSHRUN */
                o = sat_u(c, (sv + rc) >> shr, bits);
            } else if (!u) { /* SQSHRN/SQRSHRN */
                o = sat_s(c, (sv + rc) >> shr, bits);
            } else { /* UQSHRN/UQRSHRN */
                o = sat_u(c, (uv + rc) >> shr, bits);
            }
            eset(&r, off + i, sz, o);
        }
        c->v[rd] = r;
        return;
    }
    if (opc == 0x1c || opc == 0x1f) { /* conversoes ponto fixo */
        if (sz < 2) a64_undef(c);
        unsigned t = sz == 3 ? 1 : 0, fbits = shr;
        unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> sz;
        for (unsigned i = 0; i < ne; i++) {
            if (opc == 0x1c) {
                double v = int_to_fp(eget(&n, i, sz), u, bits, 1);
                fset(&r, i, t, round_t(ldexp(v, -(int)fbits), t));
            } else {
                eset(&r, i, sz, fp_to_int(ldexp(fget(&n, i, t), (int)fbits), 3, u, bits));
            }
        }
        if (!q) r.d[1] = 0;
        c->v[rd] = r;
        return;
    }
    if (sz == 3 && !q) a64_undef(c);
    unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> sz;
    for (unsigned i = 0; i < ne; i++) {
        uint64_t a = eget(&n, i, sz), dv = eget(&d, i, sz), v;
        int64_t sa = egets(&n, i, sz);
        switch (opc) {
        case 0x00: case 0x02: case 0x04: case 0x06: { /* [SU]R?SHR, [SU]R?SRA */
            bool round = opc & 4;
            __int128 x = u ? (__int128)a : (__int128)sa;
            if (round) x += (__int128)1 << (shr - 1);
            if (shr >= 64 && !u) x = x < 0 ? -1 : 0;
            else if (shr >= 64 && u && !round) x = 0;
            else x >>= shr;
            v = (uint64_t)x & m;
            if (opc & 2) v = (dv + v) & m;
            break;
        }
        case 0x08: /* SRI */
            if (!u) a64_undef(c);
            if (shr >= bits) v = dv;
            else {
                uint64_t mk = m >> shr;
                v = (dv & ~mk) | (a >> shr);
            }
            break;
        case 0x0a: /* SHL / SLI */
            if (u) {
                uint64_t mk = (m << shl) & m;
                v = (dv & ~mk) | ((a << shl) & m);
            } else {
                v = (a << shl) & m;
            }
            break;
        case 0x0c: /* SQSHLU */
            if (!u) a64_undef(c);
            v = sat_u(c, (__int128)sa << shl, bits);
            break;
        case 0x0e: /* SQSHL/UQSHL imm */
            v = u ? sat_u(c, (__int128)a << shl, bits) : sat_s(c, (__int128)sa << shl, bits);
            break;
        default:
            a64_undef(c);
        }
        eset(&r, i, sz, v);
    }
    if (!q) r.d[1] = 0;
    c->v[rd] = r;
}

/* ------------------------------------------------ 3 registradores diferentes */

static void simd_3diff(a64_cpu *c, uint32_t insn)
{
    bool q = BIT(insn, 30), u = BIT(insn, 29);
    unsigned sz = BITS(insn, 23, 22), opc = BITS(insn, 15, 12);
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    if (sz == 3 && opc != 14) a64_undef(c);
    a64_vreg n = c->v[rn], m = c->v[rm], d = c->v[rd], r = {{0}};
    unsigned bits = 8u << sz, wbits = bits * 2;
    unsigned nn = 8u >> sz, off = q ? nn : 0;
    uint64_t wm = mask_bits(wbits);

    if (opc == 4 || opc == 6) { /* ADDHN/SUBHN (+R) */
        if (off) r.d[0] = d.d[0];
        for (unsigned i = 0; i < nn; i++) {
            uint64_t a = eget(&n, i, sz + 1), b = eget(&m, i, sz + 1);
            uint64_t s = (opc == 4 ? a + b : a - b) & wm;
            if (u) s = (s + (1ULL << (bits - 1))) & wm;
            eset(&r, off + i, sz, s >> bits);
        }
        c->v[rd] = r;
        return;
    }
    if (opc == 14) { /* PMULL */
        if (sz == 0) {
            for (unsigned i = 0; i < 8; i++) {
                uint64_t a = n.b[off + i], b = m.b[off + i], p = 0;
                for (int k = 0; k < 8; k++)
                    if (b & (1u << k)) p ^= a << k;
                r.h[i] = (uint16_t)p;
            }
        } else if (sz == 3) {
            uint64_t a = n.d[q], b = m.d[q];
            unsigned __int128 p = 0;
            for (int k = 0; k < 64; k++)
                if (b & (1ULL << k)) p ^= (unsigned __int128)a << k;
            r.d[0] = (uint64_t)p;
            r.d[1] = (uint64_t)(p >> 64);
        } else {
            a64_undef(c);
        }
        c->v[rd] = r;
        return;
    }
    for (unsigned i = 0; i < nn; i++) {
        bool wide = opc == 1 || opc == 3;
        __int128 a = wide ? (u ? (__int128)eget(&n, i, sz + 1) : (__int128)egets(&n, i, sz + 1))
                          : (u ? (__int128)eget(&n, off + i, sz) : (__int128)egets(&n, off + i, sz));
        __int128 b = u ? (__int128)eget(&m, off + i, sz) : (__int128)egets(&m, off + i, sz);
        __int128 acc = u ? (__int128)eget(&d, i, sz + 1) : (__int128)egets(&d, i, sz + 1);
        uint64_t v;
        switch (opc) {
        case 0: case 1: v = (uint64_t)(a + b); break;
        case 2: case 3: v = (uint64_t)(a - b); break;
        case 5: v = (uint64_t)(acc + (a > b ? a - b : b - a)); break;
        case 7: v = (uint64_t)(a > b ? a - b : b - a); break;
        case 8: v = (uint64_t)(acc + a * b); break;
        case 10: v = (uint64_t)(acc - a * b); break;
        case 12: v = (uint64_t)(a * b); break;
        case 9: case 11: case 13: {
            if (u) a64_undef(c);
            __int128 p = (__int128)sat_s(c, 2 * a * b, wbits);
            p = (__int128)(int64_t)sext64((uint64_t)p, wbits);
            if (opc == 9) v = sat_s(c, acc + p, wbits);
            else if (opc == 11) v = sat_s(c, acc - p, wbits);
            else v = (uint64_t)p;
            break;
        }
        default: a64_undef(c);
        }
        eset(&r, i, sz + 1, v & wm);
    }
    c->v[rd] = r;
}

/* ------------------------------------------------ por elemento (indexado) */

static void simd_indexed(a64_cpu *c, uint32_t insn, bool scalar)
{
    bool q = BIT(insn, 30) || scalar, u = BIT(insn, 29);
    unsigned sz = BITS(insn, 23, 22), opc = BITS(insn, 15, 12);
    unsigned L = BIT(insn, 21), M = BIT(insn, 20), H = BIT(insn, 11);
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = BITS(insn, 19, 16);
    bool is_fp = opc == 1 || opc == 5 || opc == 9;
    unsigned idx;
    if (is_fp) {
        if (sz == 2) { idx = (H << 1) | L; rm |= M << 4; }
        else if (sz == 3) { if (L) a64_undef(c); idx = H; rm |= M << 4; }
        else a64_undef(c); /* fp16 */
        unsigned t = sz == 3 ? 1 : 0, esz = sz;
        if (!scalar && t == 1 && !BIT(insn, 30)) a64_undef(c);
        unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> esz;
        a64_vreg n = c->v[rn], d = c->v[rd], r = {{0}};
        double e = fget(VR(rm), idx, t);
        for (unsigned i = 0; i < ne; i++) {
            double x = fget(&n, i, t), acc = fget(&d, i, t), v;
            if (opc == 1) v = t == 0 ? (double)fmaf((float)x, (float)e, (float)acc) : fma(x, e, acc);
            else if (opc == 5) v = t == 0 ? (double)fmaf(-(float)x, (float)e, (float)acc) : fma(-x, e, acc);
            else v = round_t(x * e, t);
            fset(&r, i, t, v);
        }
        if (!q) r.d[1] = 0;
        c->v[rd] = r;
        return;
    }
    if (sz == 1) idx = (H << 2) | (L << 1) | M;
    else if (sz == 2) { idx = (H << 1) | L; rm |= M << 4; }
    else a64_undef(c);
    a64_vreg n = c->v[rn], d = c->v[rd], r = {{0}};
    unsigned bits = 8u << sz;
    uint64_t mk = mask_bits(bits);
    uint64_t e = eget(VR(rm), idx, sz);
    int64_t se = egets(VR(rm), idx, sz);

    if (opc == 2 || opc == 6 || opc == 10 || opc == 3 || opc == 7 || opc == 11) { /* longos */
        unsigned nn = scalar ? 1 : 8u >> sz, off = (!scalar && BIT(insn, 30)) ? nn : 0;
        unsigned wbits = bits * 2;
        for (unsigned i = 0; i < nn; i++) {
            __int128 a = u ? (__int128)eget(&n, off + i, sz) : (__int128)egets(&n, off + i, sz);
            __int128 b = u ? (__int128)e : (__int128)se;
            __int128 acc = u ? (__int128)eget(&d, i, sz + 1) : (__int128)egets(&d, i, sz + 1);
            uint64_t v;
            switch (opc) {
            case 2: v = (uint64_t)(acc + a * b); break;
            case 6: v = (uint64_t)(acc - a * b); break;
            case 10: v = (uint64_t)(a * b); break;
            default: {
                if (u) a64_undef(c);
                __int128 p = (__int128)(int64_t)sext64(sat_s(c, 2 * a * b, wbits), wbits);
                if (opc == 3) v = sat_s(c, acc + p, wbits);
                else if (opc == 7) v = sat_s(c, acc - p, wbits);
                else v = (uint64_t)p;
            }
            }
            eset(&r, i, sz + 1, v & mask_bits(wbits));
        }
        c->v[rd] = r;
        return;
    }
    unsigned ne = scalar ? 1 : (q ? 16u : 8u) >> sz;
    for (unsigned i = 0; i < ne; i++) {
        uint64_t a = eget(&n, i, sz), acc = eget(&d, i, sz), v;
        int64_t sa = egets(&n, i, sz);
        switch (opc) {
        case 0: if (!u) a64_undef(c); v = acc + a * e; break;
        case 4: if (!u) a64_undef(c); v = acc - a * e; break;
        case 8: if (u) a64_undef(c); v = a * e; break;
        case 12: case 13: {
            __int128 p = (__int128)2 * sa * se;
            if (opc == 13) p += (__int128)1 << (bits - 1);
            v = sat_s(c, p >> bits, bits);
            break;
        }
        default: a64_undef(c);
        }
        eset(&r, i, sz, v & mk);
    }
    if (!q) r.d[1] = 0;
    c->v[rd] = r;
}

/* ------------------------------------------ TBL/ZIP/UZP/TRN/EXT */

static void simd_tbl(a64_cpu *c, uint32_t insn)
{
    bool q = BIT(insn, 30), tbx = BIT(insn, 12);
    unsigned len = BITS(insn, 14, 13) + 1, rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    uint8_t table[64];
    for (unsigned i = 0; i < len; i++)
        memcpy(table + 16 * i, c->v[(rn + i) % 32].b, 16);
    a64_vreg idx = c->v[rm], d = c->v[rd], r = {{0}};
    unsigned nb = q ? 16 : 8;
    for (unsigned i = 0; i < nb; i++) {
        unsigned k = idx.b[i];
        r.b[i] = k < 16 * len ? table[k] : (tbx ? d.b[i] : 0);
    }
    c->v[rd] = r;
}

static void simd_zip(a64_cpu *c, uint32_t insn)
{
    bool q = BIT(insn, 30);
    unsigned sz = BITS(insn, 23, 22), opc = BITS(insn, 14, 12);
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    if (sz == 3 && !q) a64_undef(c);
    unsigned ne = (q ? 16u : 8u) >> sz;
    a64_vreg n = c->v[rn], m = c->v[rm], r = {{0}};
    unsigned part = (opc >> 2) & 1;
    for (unsigned i = 0; i < ne; i++) {
        uint64_t v;
        switch (opc & 3) {
        case 1: { /* UZP */
            unsigned j = 2 * i + part;
            v = j < ne ? eget(&n, j, sz) : eget(&m, j - ne, sz);
            break;
        }
        case 2: { /* TRN */
            unsigned base = i & ~1u;
            v = (i & 1) ? eget(&m, base + part, sz) : eget(&n, base + part, sz);
            break;
        }
        case 3: { /* ZIP */
            unsigned base = part * ne / 2 + i / 2;
            v = (i & 1) ? eget(&m, base, sz) : eget(&n, base, sz);
            break;
        }
        default: a64_undef(c);
        }
        eset(&r, i, sz, v);
    }
    c->v[rd] = r;
}

static void simd_ext(a64_cpu *c, uint32_t insn)
{
    bool q = BIT(insn, 30);
    unsigned imm4 = BITS(insn, 14, 11), rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    unsigned nb = q ? 16 : 8;
    if (!q && imm4 >= 8) a64_undef(c);
    a64_vreg n = c->v[rn], m = c->v[rm], r = {{0}};
    for (unsigned i = 0; i < nb; i++) {
        unsigned j = i + imm4;
        r.b[i] = j < nb ? n.b[j] : m.b[j - nb];
    }
    c->v[rd] = r;
}

/* ------------------------------------------------ escalar: pares */

static void simd_scalar_pairwise(a64_cpu *c, uint32_t insn)
{
    bool u = BIT(insn, 29);
    unsigned sz = BITS(insn, 23, 22), opc = BITS(insn, 16, 12), rd = insn & 31, rn = (insn >> 5) & 31;
    a64_vreg n = c->v[rn], r = {{0}};
    if (!u && opc == 0x1b) { /* ADDP */
        if (sz != 3) a64_undef(c);
        r.d[0] = n.d[0] + n.d[1];
        c->v[rd] = r;
        return;
    }
    if (!u) a64_undef(c);
    unsigned t = (sz & 1) ? 1 : 0;
    double a = fget(&n, 0, t), b = fget(&n, 1, t), v;
    bool mn = sz & 2;
    switch (opc) {
    case 0x0c: v = mn ? fminnm_a(a, b) : fmaxnm_a(a, b); break;
    case 0x0d: if (mn) a64_undef(c); v = a + b; break;
    case 0x0f: v = mn ? fmin_a(a, b) : fmax_a(a, b); break;
    default: a64_undef(c);
    }
    fset(&r, 0, t, round_t(v, t));
    c->v[rd] = r;
}

/* ------------------------------------------------------ despacho */

void a64_simd_fp(a64_cpu *c, uint32_t insn)
{
    if (BIT(insn, 28) && !BIT(insn, 30)) {
        fp_dp(c, insn);
        return;
    }
    if ((insn & 0x9f200400) == 0x0e200400) simd_3same(c, insn, false);
    else if ((insn & 0x9f200c00) == 0x0e200000) simd_3diff(c, insn);
    else if ((insn & 0x9f3e0c00) == 0x0e200800) simd_2misc(c, insn, false);
    else if ((insn & 0x9f3e0c00) == 0x0e300800) simd_across(c, insn);
    else if ((insn & 0x9fe08400) == 0x0e000400) simd_copy(c, insn, false);
    else if ((insn & 0x9f000400) == 0x0f000000) simd_indexed(c, insn, false);
    else if ((insn & 0x9ff80400) == 0x0f000400) simd_modimm(c, insn);
    else if ((insn & 0x9f800400) == 0x0f000400) simd_shift(c, insn, false);
    else if ((insn & 0xbf208c00) == 0x0e000000) simd_tbl(c, insn);
    else if ((insn & 0xbf208c00) == 0x0e000800) simd_zip(c, insn);
    else if ((insn & 0xbf208400) == 0x2e000000) simd_ext(c, insn);
    else if ((insn & 0xdf200400) == 0x5e200400) simd_3same(c, insn, true);
    else if ((insn & 0xdf3e0c00) == 0x5e200800) simd_2misc(c, insn, true);
    else if ((insn & 0xdf3e0c00) == 0x5e300800) simd_scalar_pairwise(c, insn);
    else if ((insn & 0xdfe08400) == 0x5e000400) simd_copy(c, insn, true);
    else if ((insn & 0xdf000400) == 0x5f000000) simd_indexed(c, insn, true);
    else if ((insn & 0xdf800400) == 0x5f000400) simd_shift(c, insn, true);
    else a64_undef(c);
}

/* ------------------------------------------- loads/stores de estruturas */

void a64_simd_ldst(a64_cpu *c, uint32_t insn)
{
    bool q = BIT(insn, 30), L = BIT(insn, 22), post = BIT(insn, 23);
    unsigned rm = BITS(insn, 20, 16), rn = BITS(insn, 9, 5), rt = insn & 31;
    uint64_t base = rn == 31 ? c->sp[(c->el && c->spsel) ? 1 : 0] : c->x[rn];
    uint64_t addr = base;
    unsigned total;

    if (!BIT(insn, 24)) { /* multiplas estruturas */
        if (BIT(insn, 21) || (!post && rm)) a64_undef(c);
        unsigned opc = BITS(insn, 15, 12), sz = BITS(insn, 11, 10);
        unsigned rpt, selem;
        switch (opc) {
        case 0: rpt = 1; selem = 4; break;
        case 2: rpt = 4; selem = 1; break;
        case 4: rpt = 1; selem = 3; break;
        case 6: rpt = 3; selem = 1; break;
        case 7: rpt = 1; selem = 1; break;
        case 8: rpt = 1; selem = 2; break;
        case 10: rpt = 2; selem = 1; break;
        default: a64_undef(c);
        }
        if (sz == 3 && !q && selem != 1) a64_undef(c);
        unsigned eb = 1u << sz, ne = (q ? 16u : 8u) / eb;
        if (selem == 1) {
            /* LD1/ST1 de 1 a 4 registradores: caminho rapido por 8 bytes */
            for (unsigned r = 0; r < rpt; r++) {
                a64_vreg *v = VR((rt + r) % 32);
                unsigned words = q ? 2 : 1;
                for (unsigned w = 0; w < words; w++) {
                    if (L) v->d[w] = a64_rd(c, addr, 8);
                    else a64_wr(c, addr, v->d[w], 8);
                    addr += 8;
                }
                if (L && !q) v->d[1] = 0;
            }
        } else {
            for (unsigned r = 0; r < rpt; r++)
                for (unsigned e = 0; e < ne; e++)
                    for (unsigned s = 0; s < selem; s++) {
                        a64_vreg *v = VR((rt + r + s) % 32);
                        if (L) eset(v, e, sz, a64_rd(c, addr, eb));
                        else a64_wr(c, addr, eget(v, e, sz), eb);
                        addr += eb;
                    }
            if (L && !q)
                for (unsigned s = 0; s < selem * rpt; s++)
                    VR((rt + s) % 32)->d[1] = 0;
        }
        total = (q ? 16u : 8u) * rpt * selem;
    } else { /* estrutura unica */
        unsigned opc = BITS(insn, 15, 13), S = BIT(insn, 12), sz = BITS(insn, 11, 10), R = BIT(insn, 21);
        unsigned selem = (((opc & 1) << 1) | R) + 1;
        unsigned scale = opc >> 1, index = 0;
        bool replicate = false;
        switch (scale) {
        case 3:
            if (!L || S) a64_undef(c);
            scale = sz;
            replicate = true;
            break;
        case 0:
            index = (q << 3) | (S << 2) | sz;
            break;
        case 1:
            if (sz & 1) a64_undef(c);
            index = (q << 2) | (S << 1) | (sz >> 1);
            break;
        case 2:
            if (sz & 2) a64_undef(c);
            if (!(sz & 1)) {
                index = (q << 1) | S;
            } else {
                if (S) a64_undef(c);
                index = q;
                scale = 3;
            }
            break;
        }
        unsigned eb = 1u << scale;
        for (unsigned s = 0; s < selem; s++) {
            a64_vreg *v = VR((rt + s) % 32);
            if (replicate) {
                uint64_t x = a64_rd(c, addr, eb);
                a64_vreg r = {{0}};
                unsigned ne = (q ? 16u : 8u) / eb;
                for (unsigned e = 0; e < ne; e++) eset(&r, e, scale, x);
                *v = r;
            } else if (L) {
                eset(v, index, scale, a64_rd(c, addr, eb));
            } else {
                a64_wr(c, addr, eget(v, index, scale), eb);
            }
            addr += eb;
        }
        total = eb * selem;
    }
    if (post) {
        uint64_t nb = base + (rm == 31 ? total : c->x[rm]);
        if (rn == 31) c->sp[(c->el && c->spsel) ? 1 : 0] = nb;
        else c->x[rn] = nb;
    }
}
