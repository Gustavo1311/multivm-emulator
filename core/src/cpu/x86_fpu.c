/*
 * FPU x87. Os registradores sao mantidos em precisao dupla (53 bits de mantissa);
 * o formato estendido de 80 bits e convertido nas cargas/armazenamentos.
 */
#include "x86_priv.h"

#include <math.h>

void x86_decode_modrm(x86_cpu *c, x86_dec *d);

#define FSW_C0 0x0100
#define FSW_C1 0x0200
#define FSW_C2 0x0400
#define FSW_C3 0x4000
#define FSW_CC (FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3)

static inline int phys(x86_cpu *c, int i) { return (c->ftop + i) & 7; }
static inline double ST(x86_cpu *c, int i) { return c->st[phys(c, i)]; }
static inline void set_st(x86_cpu *c, int i, double v) { c->st[phys(c, i)] = v; }
static inline bool empty(x86_cpu *c, int p) { return ((c->ftw >> (2 * p)) & 3) == 3; }
static inline void set_tag(x86_cpu *c, int p, int t) { c->ftw = (uint16_t)((c->ftw & ~(3u << (2 * p))) | ((unsigned)t << (2 * p))); }

static void fpush(x86_cpu *c, double v)
{
    c->ftop = (c->ftop - 1) & 7;
    if (!empty(c, c->ftop))
        c->fsw |= 0x41 | FSW_C1; /* overflow da pilha */
    c->st[c->ftop] = v;
    set_tag(c, c->ftop, 0);
}

static void fpop(x86_cpu *c)
{
    set_tag(c, c->ftop, 3);
    c->ftop = (c->ftop + 1) & 7;
}

void x87_reset(x86_cpu *c)
{
    c->fcw = 0x37f;
    c->fsw = 0;
    c->ftw = 0xffff;
    c->ftop = 0;
    c->fop = 0;
    c->fip = c->fdp = 0;
    for (int i = 0; i < 8; i++)
        c->st[i] = 0;
}

static uint16_t get_fsw(x86_cpu *c) { return (uint16_t)((c->fsw & ~0x3800u) | ((unsigned)c->ftop << 11)); }

/* ---------------------------------------------- conversao 80 bits */

void x87_to_ext(double v, uint8_t out[10])
{
    uint16_t sign = signbit(v) ? 0x8000 : 0;
    uint64_t mant;
    uint16_t e;
    if (v == 0) {
        mant = 0;
        e = 0;
    } else if (isinf(v)) {
        mant = 1ULL << 63;
        e = 0x7fff;
    } else if (isnan(v)) {
        mant = 0xc000000000000000ULL;
        e = 0x7fff;
    } else {
        int ex;
        double m = frexp(fabs(v), &ex); /* m em [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64);
        e = (uint16_t)(ex - 1 + 16383);
    }
    memcpy(out, &mant, 8);
    uint16_t se = sign | e;
    memcpy(out + 8, &se, 2);
}

double x87_from_ext(const uint8_t in[10])
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, in, 8);
    memcpy(&se, in + 8, 2);
    int sign = se >> 15;
    int e = se & 0x7fff;
    double r;
    if (e == 0x7fff) {
        r = (mant << 1) ? NAN : INFINITY;
    } else if (e == 0 && mant == 0) {
        r = 0.0;
    } else {
        r = ldexp((double)mant, (e ? e : 1) - 16383 - 63);
    }
    return sign ? -r : r;
}

/* ------------------------------------------------------ utilitarios */

static double fround(x86_cpu *c, double v)
{
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static int64_t to_int(x86_cpu *c, double v, int bytes, bool trunc_mode)
{
    double r = trunc_mode ? trunc(v) : fround(c, v);
    double lim = bytes == 2 ? 32768.0 : bytes == 4 ? 2147483648.0 : 9223372036854775808.0;
    if (isnan(r) || r >= lim || r < -lim) {
        c->fsw |= 1; /* IE */
        return bytes == 2 ? INT16_MIN : bytes == 4 ? INT32_MIN : INT64_MIN;
    }
    return (int64_t)r;
}

static void fcom(x86_cpu *c, double a, double b)
{
    c->fsw &= ~FSW_CC;
    if (isnan(a) || isnan(b)) c->fsw |= FSW_C0 | FSW_C2 | FSW_C3;
    else if (a < b) c->fsw |= FSW_C0;
    else if (a == b) c->fsw |= FSW_C3;
}

static void fcomi(x86_cpu *c, double a, double b)
{
    uint64_t f = 0;
    if (isnan(a) || isnan(b)) f = EFL_ZF | EFL_PF | EFL_CF;
    else if (a < b) f = EFL_CF;
    else if (a == b) f = EFL_ZF;
    x86_set_arith_flags(c, f);
}

static double arith(int op, double a, double b)
{
    switch (op) {
    case 0: return a + b;
    case 1: return a * b;
    case 4: return a - b;
    case 5: return b - a;
    case 6: return a / b;
    default: return b / a;
    }
}

static double read_mem_fp(x86_cpu *c, uint64_t lin, int kind)
{
    switch (kind) {
    case 0: { uint32_t v = (uint32_t)x86_rd_lin(c, lin, 4); float f; memcpy(&f, &v, 4); return f; }
    case 1: return (double)(int32_t)x86_rd_lin(c, lin, 4);
    case 2: { uint64_t v = x86_rd_lin(c, lin, 8); double f; memcpy(&f, &v, 8); return f; }
    default: return (double)(int16_t)x86_rd_lin(c, lin, 2);
    }
}

static void store_env(x86_cpu *c, uint64_t lin, int osz)
{
    if (osz == 2) {
        x86_wr_lin(c, lin, c->fcw, 2);
        x86_wr_lin(c, lin + 2, get_fsw(c), 2);
        x86_wr_lin(c, lin + 4, c->ftw, 2);
        x86_wr_lin(c, lin + 6, c->fip & 0xffff, 2);
        x86_wr_lin(c, lin + 8, 0, 2);
        x86_wr_lin(c, lin + 10, c->fdp & 0xffff, 2);
        x86_wr_lin(c, lin + 12, 0, 2);
    } else {
        x86_wr_lin(c, lin, 0xffff0000u | c->fcw, 4);
        x86_wr_lin(c, lin + 4, 0xffff0000u | get_fsw(c), 4);
        x86_wr_lin(c, lin + 8, 0xffff0000u | c->ftw, 4);
        x86_wr_lin(c, lin + 12, (uint32_t)c->fip, 4);
        x86_wr_lin(c, lin + 16, c->fop, 4);
        x86_wr_lin(c, lin + 20, (uint32_t)c->fdp, 4);
        x86_wr_lin(c, lin + 24, 0, 4);
    }
}

static void load_env(x86_cpu *c, uint64_t lin, int osz)
{
    int step = osz == 2 ? 2 : 4;
    c->fcw = (uint16_t)x86_rd_lin(c, lin, 2);
    uint16_t sw = (uint16_t)x86_rd_lin(c, lin + (uint64_t)step, 2);
    c->ftw = (uint16_t)x86_rd_lin(c, lin + 2 * (uint64_t)step, 2);
    c->fsw = sw & ~0x3800u;
    c->ftop = (sw >> 11) & 7;
}

/* ------------------------------------------------------------ execucao */

void x87_exec(x86_cpu *c, x86_dec *d, int op)
{
    x87_exec_at(c, d, op, d->mem ? x86_ea_lin(c, d) : 0);
}

/* lin: endereco linear do operando de memoria (o JIT ja o calculou); usa de d so
 * modrm/reg/rm/mem/osz */
void x87_exec_at(x86_cpu *c, x86_dec *d, int op, uint64_t lin)
{
    unsigned reg = d->reg & 7;
    c->fop = (uint16_t)(((op & 7) << 8) | d->modrm);
    c->fip = c->cur_rip;
    if (d->mem) {
        c->fdp = lin;
        switch (op) {
        case 0xd8: case 0xda: case 0xdc: case 0xde: {
            int kind = op == 0xd8 ? 0 : op == 0xda ? 1 : op == 0xdc ? 2 : 3;
            double v = read_mem_fp(c, lin, kind);
            if (reg == 2 || reg == 3) {
                fcom(c, ST(c, 0), v);
                if (reg == 3) fpop(c);
            } else {
                set_st(c, 0, arith((int)reg, ST(c, 0), v));
            }
            return;
        }
        case 0xd9:
            switch (reg) {
            case 0: fpush(c, read_mem_fp(c, lin, 0)); return;
            case 2: case 3: {
                float f = (float)ST(c, 0);
                uint32_t v;
                memcpy(&v, &f, 4);
                x86_wr_lin(c, lin, v, 4);
                if (reg == 3) fpop(c);
                return;
            }
            case 4: load_env(c, lin, d->osz); return;
            case 5: c->fcw = (uint16_t)x86_rd_lin(c, lin, 2); return;
            case 6: store_env(c, lin, d->osz); c->fcw |= 0x3f; return;
            case 7: x86_wr_lin(c, lin, c->fcw, 2); return;
            default: x86_ud(c);
            }
        case 0xdb:
            switch (reg) {
            case 0: fpush(c, read_mem_fp(c, lin, 1)); return;
            case 1: case 2: case 3:
                x86_wr_lin(c, lin, (uint64_t)to_int(c, ST(c, 0), 4, reg == 1), 4);
                if (reg != 2) fpop(c);
                return;
            case 5: {
                uint8_t b[10];
                for (int i = 0; i < 10; i++) b[i] = (uint8_t)x86_rd_lin(c, lin + (uint64_t)i, 1);
                fpush(c, x87_from_ext(b));
                return;
            }
            case 7: {
                uint8_t b[10];
                x87_to_ext(ST(c, 0), b);
                x86_wr_lin(c, lin, ld_le(b, 8), 8);
                x86_wr_lin(c, lin + 8, ld_le(b + 8, 2), 2);
                fpop(c);
                return;
            }
            default: x86_ud(c);
            }
        case 0xdd:
            switch (reg) {
            case 0: fpush(c, read_mem_fp(c, lin, 2)); return;
            case 1:
                x86_wr_lin(c, lin, (uint64_t)to_int(c, ST(c, 0), 8, true), 8);
                fpop(c);
                return;
            case 2: case 3: {
                double v = ST(c, 0);
                uint64_t b;
                memcpy(&b, &v, 8);
                x86_wr_lin(c, lin, b, 8);
                if (reg == 3) fpop(c);
                return;
            }
            case 4: { /* FRSTOR */
                load_env(c, lin, d->osz);
                uint64_t off = d->osz == 2 ? 14 : 28;
                for (int i = 0; i < 8; i++) {
                    uint8_t b[10];
                    for (int k = 0; k < 10; k++) b[k] = (uint8_t)x86_rd_lin(c, lin + off + 10 * (uint64_t)i + (uint64_t)k, 1);
                    c->st[phys(c, i)] = x87_from_ext(b);
                }
                return;
            }
            case 6: { /* FNSAVE */
                store_env(c, lin, d->osz);
                uint64_t off = d->osz == 2 ? 14 : 28;
                for (int i = 0; i < 8; i++) {
                    uint8_t b[10];
                    x87_to_ext(ST(c, i), b);
                    for (int k = 0; k < 10; k++) x86_wr_lin(c, lin + off + 10 * (uint64_t)i + (uint64_t)k, b[k], 1);
                }
                x87_reset(c);
                return;
            }
            case 7: x86_wr_lin(c, lin, get_fsw(c), 2); return;
            default: x86_ud(c);
            }
        case 0xdf:
            switch (reg) {
            case 0: fpush(c, read_mem_fp(c, lin, 3)); return;
            case 1: case 2: case 3:
                x86_wr_lin(c, lin, (uint64_t)to_int(c, ST(c, 0), 2, reg == 1), 2);
                if (reg != 2) fpop(c);
                return;
            case 4: { /* FBLD */
                double v = 0, mul = 1;
                for (int i = 0; i < 9; i++) {
                    uint8_t byte = (uint8_t)x86_rd_lin(c, lin + (uint64_t)i, 1);
                    v += (byte & 15) * mul;
                    v += (byte >> 4) * mul * 10;
                    mul *= 100;
                }
                if (x86_rd_lin(c, lin + 9, 1) & 0x80) v = -v;
                fpush(c, v);
                return;
            }
            case 5: fpush(c, (double)(int64_t)x86_rd_lin(c, lin, 8)); return;
            case 6: { /* FBSTP */
                int64_t v = to_int(c, ST(c, 0), 8, false);
                bool neg = v < 0;
                uint64_t u = neg ? (uint64_t)-v : (uint64_t)v;
                for (int i = 0; i < 9; i++) {
                    uint8_t byte = (uint8_t)(u % 10);
                    u /= 10;
                    byte |= (uint8_t)((u % 10) << 4);
                    u /= 10;
                    x86_wr_lin(c, lin + (uint64_t)i, byte, 1);
                }
                x86_wr_lin(c, lin + 9, neg ? 0x80 : 0, 1);
                fpop(c);
                return;
            }
            case 7:
                x86_wr_lin(c, lin, (uint64_t)to_int(c, ST(c, 0), 8, false), 8);
                fpop(c);
                return;
            default: x86_ud(c);
            }
        }
        x86_ud(c);
    }

    /* formas com registrador */
    int i = d->modrm & 7;
    switch (op) {
    case 0xd8:
        if (reg == 2 || reg == 3) {
            fcom(c, ST(c, 0), ST(c, i));
            if (reg == 3) fpop(c);
        } else {
            set_st(c, 0, arith((int)reg, ST(c, 0), ST(c, i)));
        }
        return;
    case 0xd9:
        switch (d->modrm) {
        case 0xd0: return;
        case 0xe0: set_st(c, 0, -ST(c, 0)); return;
        case 0xe1: set_st(c, 0, fabs(ST(c, 0))); return;
        case 0xe4: fcom(c, ST(c, 0), 0.0); return;
        case 0xe5: {
            double v = ST(c, 0);
            c->fsw &= ~FSW_CC;
            if (signbit(v)) c->fsw |= FSW_C1;
            if (empty(c, phys(c, 0))) c->fsw |= FSW_C3 | FSW_C0;
            else if (isnan(v)) c->fsw |= FSW_C0;
            else if (isinf(v)) c->fsw |= FSW_C2 | FSW_C0;
            else if (v == 0) c->fsw |= FSW_C3;
            else if (fpclassify(v) == FP_SUBNORMAL) c->fsw |= FSW_C3 | FSW_C2;
            else c->fsw |= FSW_C2;
            return;
        }
        case 0xe8: fpush(c, 1.0); return;
        case 0xe9: fpush(c, 3.321928094887362347870319429489390175864831393); return;
        case 0xea: fpush(c, 1.442695040888963407359924681001892137426645954); return;
        case 0xeb: fpush(c, M_PI); return;
        case 0xec: fpush(c, 0.301029995663981195213738894724493026768189881); return;
        case 0xed: fpush(c, M_LN2); return;
        case 0xee: fpush(c, 0.0); return;
        case 0xf0: set_st(c, 0, exp2(ST(c, 0)) - 1.0); return;
        case 0xf1: set_st(c, 1, ST(c, 1) * log2(ST(c, 0))); fpop(c); return;
        case 0xf2: set_st(c, 0, tan(ST(c, 0))); fpush(c, 1.0); c->fsw &= ~FSW_C2; return;
        case 0xf3: set_st(c, 1, atan2(ST(c, 1), ST(c, 0))); fpop(c); return;
        case 0xf4: {
            double v = ST(c, 0);
            int e;
            double m = frexp(v, &e);
            set_st(c, 0, (double)(e - 1));
            fpush(c, m * 2.0);
            return;
        }
        case 0xf5: case 0xf8: { /* FPREM1 / FPREM */
            double a = ST(c, 0), b = ST(c, 1);
            double q = d->modrm == 0xf8 ? trunc(a / b) : nearbyint(a / b);
            double r = d->modrm == 0xf8 ? fmod(a, b) : remainder(a, b);
            set_st(c, 0, r);
            int64_t qi = isnan(q) ? 0 : (int64_t)fabs(q);
            c->fsw &= ~FSW_CC;
            if (qi & 1) c->fsw |= FSW_C1;
            if (qi & 2) c->fsw |= FSW_C3;
            if (qi & 4) c->fsw |= FSW_C0;
            return;
        }
        case 0xf6: c->ftop = (c->ftop - 1) & 7; return;
        case 0xf7: c->ftop = (c->ftop + 1) & 7; return;
        case 0xf9: set_st(c, 1, ST(c, 1) * log2(ST(c, 0) + 1.0)); fpop(c); return;
        case 0xfa: set_st(c, 0, sqrt(ST(c, 0))); return;
        case 0xfb: {
            double v = ST(c, 0);
            set_st(c, 0, sin(v));
            fpush(c, cos(v));
            c->fsw &= ~FSW_C2;
            return;
        }
        case 0xfc: set_st(c, 0, fround(c, ST(c, 0))); return;
        case 0xfd: set_st(c, 0, ldexp(ST(c, 0), (int)trunc(ST(c, 1)))); return;
        case 0xfe: set_st(c, 0, sin(ST(c, 0))); c->fsw &= ~FSW_C2; return;
        case 0xff: set_st(c, 0, cos(ST(c, 0))); c->fsw &= ~FSW_C2; return;
        default: break;
        }
        if (reg == 0) { fpush(c, ST(c, i)); return; }
        if (reg == 1) {
            double t = ST(c, 0);
            set_st(c, 0, ST(c, i));
            set_st(c, i, t);
            int p0 = phys(c, 0), pi = phys(c, i);
            int t0 = (c->ftw >> (2 * p0)) & 3, ti = (c->ftw >> (2 * pi)) & 3;
            set_tag(c, p0, ti);
            set_tag(c, pi, t0);
            return;
        }
        if (reg == 3) { set_st(c, i, ST(c, 0)); fpop(c); return; }
        x86_ud(c);
    case 0xda:
        if (d->modrm == 0xe9) {
            fcom(c, ST(c, 0), ST(c, 1));
            fpop(c);
            fpop(c);
            return;
        }
        {
            static const int cc[4] = {2, 4, 6, 10}; /* B, E, BE, P */
            if (reg < 4) {
                if (x86_cond(c, cc[reg])) set_st(c, 0, ST(c, i));
                return;
            }
        }
        x86_ud(c);
    case 0xdb:
        switch (d->modrm) {
        case 0xe2: c->fsw &= 0x7f00; return;
        case 0xe3: x87_reset(c); return;
        case 0xe0: case 0xe1: case 0xe4: return;
        default: break;
        }
        if (reg < 4) {
            static const int cc[4] = {3, 5, 7, 11}; /* NB, NE, NBE, NP */
            if (x86_cond(c, cc[reg])) set_st(c, 0, ST(c, i));
            return;
        }
        if (reg == 5 || reg == 6) { fcomi(c, ST(c, 0), ST(c, i)); return; }
        x86_ud(c);
    case 0xdc: {
        if (reg == 2 || reg == 3) {
            fcom(c, ST(c, 0), ST(c, i));
            if (reg == 3) fpop(c);
            return;
        }
        static const int swap[8] = {0, 1, 2, 3, 5, 4, 7, 6};
        set_st(c, i, arith(swap[reg], ST(c, i), ST(c, 0)));
        return;
    }
    case 0xdd:
        switch (reg) {
        case 0: set_tag(c, phys(c, i), 3); return;
        case 1: {
            double t = ST(c, 0);
            set_st(c, 0, ST(c, i));
            set_st(c, i, t);
            return;
        }
        case 2: set_st(c, i, ST(c, 0)); return;
        case 3: set_st(c, i, ST(c, 0)); fpop(c); return;
        case 4: fcom(c, ST(c, 0), ST(c, i)); return;
        case 5: fcom(c, ST(c, 0), ST(c, i)); fpop(c); return;
        default: x86_ud(c);
        }
    case 0xde: {
        if (d->modrm == 0xd9) {
            fcom(c, ST(c, 0), ST(c, 1));
            fpop(c);
            fpop(c);
            return;
        }
        if (reg == 2 || reg == 3) {
            fcom(c, ST(c, 0), ST(c, i));
            fpop(c);
            return;
        }
        static const int swap[8] = {0, 1, 2, 3, 5, 4, 7, 6};
        set_st(c, i, arith(swap[reg], ST(c, i), ST(c, 0)));
        fpop(c);
        return;
    }
    case 0xdf:
        if (d->modrm == 0xe0) {
            c->r[R_AX] = (c->r[R_AX] & ~0xffffULL) | get_fsw(c);
            return;
        }
        if (reg == 5 || reg == 6) {
            fcomi(c, ST(c, 0), ST(c, i));
            fpop(c);
            return;
        }
        if (reg == 0) { set_tag(c, phys(c, i), 3); fpop(c); return; }
        if (reg == 1) {
            double t = ST(c, 0);
            set_st(c, 0, ST(c, i));
            set_st(c, i, t);
            return;
        }
        if (reg == 2 || reg == 3) { set_st(c, i, ST(c, 0)); fpop(c); return; }
        x86_ud(c);
    }
    x86_ud(c);
}

void x87_fxsave_regs(x86_cpu *c, uint8_t *buf)
{
    /* buf: 512 bytes (formato FXSAVE) - preenche a parte x87 */
    st_le(buf + 0, c->fcw, 2);
    st_le(buf + 2, get_fsw(c), 2);
    uint8_t abr = 0;
    for (int p = 0; p < 8; p++)
        if (!empty(c, p)) abr |= (uint8_t)(1u << p);
    buf[4] = abr;
    st_le(buf + 6, c->fop, 2);
    st_le(buf + 8, c->fip, 8);
    st_le(buf + 16, c->fdp, 8);
    for (int i = 0; i < 8; i++) {
        memset(buf + 32 + 16 * i, 0, 16);
        x87_to_ext(ST(c, i), buf + 32 + 16 * i);
    }
}

void x87_fxrstor_regs(x86_cpu *c, const uint8_t *buf)
{
    c->fcw = (uint16_t)ld_le(buf, 2);
    uint16_t sw = (uint16_t)ld_le(buf + 2, 2);
    c->fsw = sw & ~0x3800u;
    c->ftop = (sw >> 11) & 7;
    uint8_t abr = buf[4];
    c->ftw = 0;
    for (int p = 0; p < 8; p++)
        set_tag(c, p, (abr >> p) & 1 ? 0 : 3);
    c->fop = (uint16_t)ld_le(buf + 6, 2);
    c->fip = ld_le(buf + 8, 8);
    c->fdp = ld_le(buf + 16, 8);
    for (int i = 0; i < 8; i++)
        c->st[phys(c, i)] = x87_from_ext(buf + 32 + 16 * i);
}
