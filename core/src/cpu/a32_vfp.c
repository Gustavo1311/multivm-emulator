/* ARMv7: VFPv3-D32 (+ VFMA do VFPv4). Sem Advanced SIMD (NEON). */
#include "cpu_arm32.h"

#include <math.h>

#define FPSID_VAL 0x410330c0u
#define MVFR0_VAL 0x10110222u /* 32 regs D, simples/dupla, divisao, raiz */
#define MVFR1_VAL 0x00000011u /* sem NEON */

bool a32_vfp_enabled(a32_cpu *c)
{
    unsigned cp10 = (c->cpacr >> 20) & 3;
    if (cp10 == 0 || cp10 == 2 || (cp10 == 1 && !a32_priv(c)))
        return false;
    return (c->fpexc >> 30) & 1;
}

static inline float S(a32_cpu *c, unsigned n) { return c->vfp.fs[n]; }
static inline double D(a32_cpu *c, unsigned n) { return c->vfp.fd[n]; }

static double rd_op(a32_cpu *c, unsigned n, bool dp) { return dp ? D(c, n) : (double)S(c, n); }

static void wr_op(a32_cpu *c, unsigned n, bool dp, double v)
{
    if (dp) c->vfp.fd[n] = v;
    else c->vfp.fs[n] = (float)v;
}

static uint32_t fcmp(double a, double b)
{
    if (isnan(a) || isnan(b)) return 3;
    if (a == b) return 6;
    if (a < b) return 8;
    return 2;
}

static int rmode_fpscr(a32_cpu *c) { return (int)((c->fpscr >> 22) & 3); }

static double round_mode(double x, int rm)
{
    switch (rm) {
    case 0: return nearbyint(x);
    case 1: return ceil(x);
    case 2: return floor(x);
    default: return trunc(x);
    }
}

static uint32_t to_int32(double x, int rm, bool uns)
{
    if (isnan(x)) return 0;
    double r = round_mode(x, rm);
    if (uns) {
        if (r <= 0) return 0;
        if (r >= 4294967296.0) return 0xffffffffu;
        return (uint32_t)r;
    }
    if (r >= 2147483648.0) return 0x7fffffffu;
    if (r < -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}

static double expand_imm(unsigned imm8)
{
    uint64_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    uint64_t exp = ((b6 ^ 1) << 10) | ((b6 ? 0xffULL : 0) << 2) | ((imm8 >> 4) & 3);
    uint64_t bits = (sign << 63) | (exp << 52) | ((uint64_t)(imm8 & 0xf) << 48);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void vfp_dp(a32_cpu *c, uint32_t insn)
{
    bool dp = BIT(insn, 8);
    unsigned opc1 = BITS(insn, 23, 20), opc2 = BITS(insn, 19, 16), opc3 = BITS(insn, 7, 6);
    unsigned vd = dp ? (BIT(insn, 22) << 4) | BITS(insn, 15, 12) : (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
    unsigned vn = dp ? (BIT(insn, 7) << 4) | BITS(insn, 19, 16) : (BITS(insn, 19, 16) << 1) | BIT(insn, 7);
    unsigned vm = dp ? (BIT(insn, 5) << 4) | BITS(insn, 3, 0) : (BITS(insn, 3, 0) << 1) | BIT(insn, 5);
    unsigned o1 = opc1 & 0xb;
    bool op6 = BIT(insn, 6);
    double n = rd_op(c, vn, dp), m = rd_op(c, vm, dp), d = rd_op(c, vd, dp), r;

#define RND(x) (dp ? (x) : (double)(float)(x))
    switch (o1) {
    case 0x0: r = RND(n * m); r = op6 ? d - r : d + r; wr_op(c, vd, dp, r); return;       /* VMLA/VMLS */
    case 0x1: r = RND(n * m); r = op6 ? -d - r : -d + r; wr_op(c, vd, dp, r); return;     /* VNMLS/VNMLA */
    case 0x2: r = n * m; wr_op(c, vd, dp, op6 ? -r : r); return;                          /* VMUL/VNMUL */
    case 0x3: wr_op(c, vd, dp, op6 ? n - m : n + m); return;                              /* VADD/VSUB */
    case 0x8: if (op6) a32_undef(c); wr_op(c, vd, dp, n / m); return;                     /* VDIV */
    case 0x9: /* VFNMA/VFNMS */
        if (dp) r = fma(op6 ? -n : n, m, -d);
        else r = fmaf(op6 ? -(float)n : (float)n, (float)m, -(float)d);
        wr_op(c, vd, dp, r);
        return;
    case 0xa: /* VFMA/VFMS */
        if (dp) r = fma(op6 ? -n : n, m, d);
        else r = fmaf(op6 ? -(float)n : (float)n, (float)m, (float)d);
        wr_op(c, vd, dp, r);
        return;
    case 0xb:
        break;
    default:
        a32_undef(c);
    }
    if (!op6) { /* VMOV imediato */
        wr_op(c, vd, dp, expand_imm((BITS(insn, 19, 16) << 4) | BITS(insn, 3, 0)));
        return;
    }
    switch (opc2) {
    case 0x0:
        if (opc3 == 1) { /* VMOV registrador (copia bits) */
            if (dp) c->vfp.d[vd] = c->vfp.d[vm];
            else c->vfp.s[vd] = c->vfp.s[vm];
        } else { /* VABS */
            if (dp) c->vfp.d[vd] = c->vfp.d[vm] & ~(1ULL << 63);
            else c->vfp.s[vd] = c->vfp.s[vm] & ~(1u << 31);
        }
        return;
    case 0x1:
        if (opc3 == 1) { /* VNEG */
            if (dp) c->vfp.d[vd] = c->vfp.d[vm] ^ (1ULL << 63);
            else c->vfp.s[vd] = c->vfp.s[vm] ^ (1u << 31);
        } else {
            wr_op(c, vd, dp, dp ? sqrt(m) : (double)sqrtf((float)m));
        }
        return;
    case 0x2: case 0x3: { /* VCVTB/VCVTT meia precisao */
        bool top = BIT(insn, 7);
        unsigned svd = (BITS(insn, 15, 12) << 1) | BIT(insn, 22), svm = (BITS(insn, 3, 0) << 1) | BIT(insn, 5);
        if (opc2 == 2) { /* half -> single */
            uint16_t h = (uint16_t)(c->vfp.s[svm] >> (top ? 16 : 0));
            c->vfp.fs[svd] = f16_to_f32(h);
        } else {
            uint16_t h = f32_to_f16(c->vfp.fs[svm]);
            if (top) c->vfp.s[svd] = (c->vfp.s[svd] & 0xffff) | ((uint32_t)h << 16);
            else c->vfp.s[svd] = (c->vfp.s[svd] & 0xffff0000u) | h;
        }
        return;
    }
    case 0x4: case 0x5: { /* VCMP/VCMPE */
        double b = opc2 == 5 ? 0.0 : m;
        c->fpscr = (c->fpscr & 0x0fffffffu) | (fcmp(d, b) << 28);
        return;
    }
    case 0x7: { /* VCVT dupla <-> simples */
        if (opc3 != 3) a32_undef(c);
        if (dp) {
            unsigned svd = (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
            c->vfp.fs[svd] = (float)D(c, vm);
        } else {
            unsigned dvd = (BIT(insn, 22) << 4) | BITS(insn, 15, 12);
            c->vfp.fd[dvd] = (double)S(c, vm);
        }
        return;
    }
    case 0x8: { /* VCVT inteiro -> fp: origem sempre S */
        unsigned svm = (BITS(insn, 3, 0) << 1) | BIT(insn, 5);
        uint32_t v = c->vfp.s[svm];
        double x = BIT(insn, 7) ? (double)(int32_t)v : (double)v;
        wr_op(c, vd, dp, x);
        return;
    }
    case 0xc: case 0xd: { /* VCVT fp -> inteiro: destino sempre S */
        unsigned svd = (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
        int rm = BIT(insn, 7) ? 3 : rmode_fpscr(c);
        c->vfp.s[svd] = to_int32(m, rm, opc2 == 0xc);
        return;
    }
    case 0xa: case 0xb: case 0xe: case 0xf: { /* VCVT ponto fixo (in-place em Vd) */
        bool to_fixed = BIT(insn, 18), uns = BIT(insn, 16), sx32 = BIT(insn, 7);
        unsigned size = sx32 ? 32 : 16;
        unsigned imm = (BITS(insn, 3, 0) << 1) | BIT(insn, 5);
        int frac = (int)size - (int)imm;
        if (to_fixed) {
            double x = ldexp(rd_op(c, vd, dp), frac);
            uint32_t v;
            if (size == 16) {
                double t = trunc(x);
                if (uns) v = isnan(t) || t < 0 ? 0 : t > 65535 ? 65535 : (uint32_t)t;
                else v = isnan(t) ? 0 : t > 32767 ? 0x7fff : t < -32768 ? 0xffff8000u : (uint32_t)(int32_t)t;
            } else {
                v = to_int32(x, 3, uns);
            }
            if (dp) c->vfp.d[vd] = uns ? (uint64_t)v : (uint64_t)(int64_t)(int32_t)v;
            else c->vfp.s[vd] = v;
        } else {
            uint32_t raw = dp ? (uint32_t)c->vfp.d[vd] : c->vfp.s[vd];
            double x;
            if (size == 16) x = uns ? (double)(raw & 0xffff) : (double)(int16_t)raw;
            else x = uns ? (double)raw : (double)(int32_t)raw;
            wr_op(c, vd, dp, ldexp(x, -frac));
        }
        return;
    }
    default:
        a32_undef(c);
    }
#undef RND
}

static void vfp_ldst(a32_cpu *c, uint32_t insn)
{
    bool dp = BIT(insn, 8);
    bool p = BIT(insn, 24), u = BIT(insn, 23), w = BIT(insn, 21), l = BIT(insn, 20);
    unsigned rn = BITS(insn, 19, 16), imm8 = insn & 0xff;
    unsigned vd = dp ? (BIT(insn, 22) << 4) | BITS(insn, 15, 12) : (BITS(insn, 15, 12) << 1) | BIT(insn, 22);
    uint32_t base = rn == 15 ? (c->cur + (c->thumb ? 4 : 8)) & ~3u : c->r[rn];
    if (p && !w) { /* VLDR/VSTR */
        uint32_t addr = u ? base + imm8 * 4 : base - imm8 * 4;
        if (dp) {
            if (l) {
                uint32_t lo = a32_rd(c, addr, 4), hi = a32_rd(c, addr + 4, 4);
                c->vfp.d[vd] = ((uint64_t)hi << 32) | lo;
            } else {
                a32_wr(c, addr + 4, (uint32_t)(c->vfp.d[vd] >> 32), 4);
                a32_wr(c, addr, (uint32_t)c->vfp.d[vd], 4);
            }
        } else {
            if (l) c->vfp.s[vd] = a32_rd(c, addr, 4);
            else a32_wr(c, addr, c->vfp.s[vd], 4);
        }
        return;
    }
    /* VLDM/VSTM */
    if (p == u) a32_undef(c);
    uint32_t words = imm8;
    uint32_t addr = u ? base : base - words * 4;
    if (dp) {
        unsigned nregs = words / 2;
        for (unsigned i = 0; i < nregs && vd + i < 32; i++) {
            if (l) {
                uint32_t lo = a32_rd(c, addr, 4), hi = a32_rd(c, addr + 4, 4);
                c->vfp.d[vd + i] = ((uint64_t)hi << 32) | lo;
            } else {
                a32_wr(c, addr, (uint32_t)c->vfp.d[vd + i], 4);
                a32_wr(c, addr + 4, (uint32_t)(c->vfp.d[vd + i] >> 32), 4);
            }
            addr += 8;
        }
    } else {
        for (unsigned i = 0; i < words && vd + i < 32; i++) {
            if (l) c->vfp.s[vd + i] = a32_rd(c, addr, 4);
            else a32_wr(c, addr, c->vfp.s[vd + i], 4);
            addr += 4;
        }
    }
    if (w)
        c->r[rn] = u ? base + words * 4 : base - words * 4;
}

void a32_vfp(a32_cpu *c, uint32_t insn)
{
    unsigned cp10 = (c->cpacr >> 20) & 3;
    if (cp10 == 0 || cp10 == 2 || (cp10 == 1 && !a32_priv(c)))
        a32_undef(c);
    bool en = (c->fpexc >> 30) & 1;
    unsigned top = BITS(insn, 27, 24);

    /* VMRS/VMSR de registradores de sistema sao permitidos com FPEXC.EN = 0 (PL1) */
    if (top == 0xe && BIT(insn, 4) && BITS(insn, 23, 21) == 7 && !BIT(insn, 8)) {
        unsigned reg = BITS(insn, 19, 16), rt = BITS(insn, 15, 12);
        bool l = BIT(insn, 20);
        if (!en && (reg == 1 || !a32_priv(c)))
            a32_undef(c);
        if (l) {
            uint32_t v;
            switch (reg) {
            case 0: v = FPSID_VAL; break;
            case 1: v = c->fpscr; break;
            case 6: v = MVFR1_VAL; break;
            case 7: v = MVFR0_VAL; break;
            case 8: v = c->fpexc; break;
            case 9: v = c->fpinst; break;
            case 10: v = c->fpinst2; break;
            default: a32_undef(c);
            }
            if (rt == 15) {
                if (reg != 1) a32_undef(c);
                c->nzcv = v >> 28;
            } else {
                c->r[rt] = v;
            }
        } else {
            uint32_t v = c->r[rt];
            switch (reg) {
            case 0: break;
            case 1: c->fpscr = v & 0xffc89f9fu; break;
            case 8: c->fpexc = v & 0xe0000000u; break;
            case 9: c->fpinst = v; break;
            case 10: c->fpinst2 = v; break;
            default: a32_undef(c);
            }
        }
        return;
    }
    if (!en)
        a32_undef(c);

    if (top == 0xe) {
        if (!BIT(insn, 4)) {
            vfp_dp(c, insn);
            return;
        }
        bool l = BIT(insn, 20), cbit = BIT(insn, 8);
        unsigned a = BITS(insn, 23, 21), rt = BITS(insn, 15, 12);
        if (!cbit && a == 0) { /* VMOV core <-> simples */
            unsigned sn = (BITS(insn, 19, 16) << 1) | BIT(insn, 7);
            if (l) c->r[rt] = c->vfp.s[sn];
            else c->vfp.s[sn] = c->r[rt];
            return;
        }
        if (cbit && (a & 4) == 0 && BITS(insn, 6, 5) == 0 && !BIT(insn, 22)) { /* VMOV.32 escalar */
            unsigned dn = (BIT(insn, 7) << 4) | BITS(insn, 19, 16);
            unsigned idx = BIT(insn, 21);
            if (l) c->r[rt] = c->vfp.s[dn * 2 + idx];
            else c->vfp.s[dn * 2 + idx] = c->r[rt];
            return;
        }
        a32_undef(c);
    }
    if ((insn & 0x0fe00000u) == 0x0c400000u) { /* transferencias de 64 bits */
        bool l = BIT(insn, 20), dp = BIT(insn, 8);
        unsigned rt = BITS(insn, 15, 12), rt2 = BITS(insn, 19, 16);
        if (dp) {
            unsigned dm = (BIT(insn, 5) << 4) | BITS(insn, 3, 0);
            if (l) {
                c->r[rt] = (uint32_t)c->vfp.d[dm];
                c->r[rt2] = (uint32_t)(c->vfp.d[dm] >> 32);
            } else {
                c->vfp.d[dm] = ((uint64_t)c->r[rt2] << 32) | c->r[rt];
            }
        } else {
            unsigned sm = (BITS(insn, 3, 0) << 1) | BIT(insn, 5);
            if (sm == 31) a32_undef(c);
            if (l) {
                c->r[rt] = c->vfp.s[sm];
                c->r[rt2] = c->vfp.s[sm + 1];
            } else {
                c->vfp.s[sm] = c->r[rt];
                c->vfp.s[sm + 1] = c->r[rt2];
            }
        }
        return;
    }
    if (top == 0xc || top == 0xd) {
        vfp_ldst(c, insn);
        return;
    }
    a32_undef(c);
}
