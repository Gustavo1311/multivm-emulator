/* x86: SSE, SSE2, SSE3, SSSE3, SSE4.1, SSE4.2 e MMX; FXSAVE/FXRSTOR. */
#include "x86_priv.h"

#include <math.h>

void x86_decode_modrm(x86_cpu *c, x86_dec *d);

static inline xmm_reg *XR(x86_cpu *c, int i) { return &c->xmm[i & 15]; }

/* le operando xmm/mem (bytes = 4, 8 ou 16); mem: zera o restante */
static void ld_x(x86_cpu *c, x86_dec *d, xmm_reg *o, int bytes)
{
    if (!d->mem) {
        *o = *XR(c, d->rm);
        return;
    }
    uint64_t lin = x86_ea_lin(c, d);
    o->q[0] = o->q[1] = 0;
    if (bytes == 16) {
        o->q[0] = x86_rd_lin(c, lin, 8);
        o->q[1] = x86_rd_lin(c, lin + 8, 8);
    } else {
        o->q[0] = x86_rd_lin(c, lin, (unsigned)bytes);
    }
}

static void st_mem(x86_cpu *c, x86_dec *d, const xmm_reg *v, int bytes)
{
    uint64_t lin = x86_ea_lin(c, d);
    if (bytes == 16) {
        x86_wr_lin(c, lin + 8, v->q[1], 8);
        x86_wr_lin(c, lin, v->q[0], 8);
    } else {
        x86_wr_lin(c, lin, v->q[0], (unsigned)bytes);
    }
}

static uint64_t ld_mm(x86_cpu *c, x86_dec *d)
{
    if (!d->mem)
        return c->mmx[d->rm & 7];
    return x86_rd_lin(c, x86_ea_lin(c, d), 8);
}

static int mxcsr_rc(x86_cpu *c) { return (int)((c->mxcsr >> 13) & 3); }

static double rnd(double v, int rc)
{
    switch (rc) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static int64_t cvt_i(double v, int rc, int bytes)
{
    double r = rnd(v, rc);
    double lim = bytes == 4 ? 2147483648.0 : 9223372036854775808.0;
    if (isnan(r) || r >= lim || r < -lim)
        return bytes == 4 ? INT32_MIN : INT64_MIN;
    return (int64_t)r;
}

/* ------------------------------------------------------ inteiros */

static inline uint64_t sat_s(int64_t v, int bits)
{
    int64_t max = (1LL << (bits - 1)) - 1, min = -(1LL << (bits - 1));
    return (uint64_t)(v > max ? max : v < min ? min : v);
}

static inline uint64_t sat_u(int64_t v, int bits)
{
    int64_t max = (1LL << bits) - 1;
    return (uint64_t)(v > max ? max : v < 0 ? 0 : v);
}

static uint64_t eget(const xmm_reg *r, int i, int esz)
{
    switch (esz) {
    case 1: return r->b[i];
    case 2: return r->w[i];
    case 4: return r->d[i];
    default: return r->q[i];
    }
}

static int64_t egets(const xmm_reg *r, int i, int esz)
{
    switch (esz) {
    case 1: return r->sb[i];
    case 2: return r->sw[i];
    case 4: return r->sd[i];
    default: return r->sq[i];
    }
}

static void eset(xmm_reg *r, int i, int esz, uint64_t v)
{
    switch (esz) {
    case 1: r->b[i] = (uint8_t)v; break;
    case 2: r->w[i] = (uint16_t)v; break;
    case 4: r->d[i] = (uint32_t)v; break;
    default: r->q[i] = v; break;
    }
}

static void shift_elems(xmm_reg *a, int esz, int kind, uint64_t cnt, int width)
{
    int n = width / esz, bits = esz * 8;
    for (int i = 0; i < n; i++) {
        uint64_t v = eget(a, i, esz), r;
        if (kind == 0) r = cnt >= (uint64_t)bits ? 0 : v >> cnt;              /* SRL */
        else if (kind == 1) r = (uint64_t)(egets(a, i, esz) >> (cnt >= (uint64_t)bits ? bits - 1 : (int)cnt)); /* SRA */
        else r = cnt >= (uint64_t)bits ? 0 : v << cnt;                        /* SLL */
        eset(a, i, esz, r);
    }
}

/* operacao inteira: a = a op b. width = 8 (MMX) ou 16 (XMM). retorna false se desconhecida */
static bool int_op(x86_cpu *c, int op, xmm_reg *a, const xmm_reg *b, int width)
{
    xmm_reg r = {{0}};
    int n;
    (void)c;
    switch (op) {
    case 0x60: case 0x61: case 0x62: case 0x6c: case 0x68: case 0x69: case 0x6a: case 0x6d: {
        int esz = (op == 0x60 || op == 0x68) ? 1 : (op == 0x61 || op == 0x69) ? 2 : (op == 0x62 || op == 0x6a) ? 4 : 8;
        bool high = (op >= 0x68 && op <= 0x6a) || op == 0x6d;
        n = width / esz;
        int base = high ? n / 2 : 0;
        for (int i = 0; i < n / 2; i++) {
            eset(&r, 2 * i, esz, eget(a, base + i, esz));
            eset(&r, 2 * i + 1, esz, eget(b, base + i, esz));
        }
        break;
    }
    case 0x63: case 0x67: case 0x6b: { /* PACKSSWB, PACKUSWB, PACKSSDW */
        int esz = op == 0x6b ? 4 : 2;
        n = width / esz;
        for (int i = 0; i < n; i++) {
            int64_t v = egets(a, i, esz), w = egets(b, i, esz);
            uint64_t sv = op == 0x67 ? sat_u(v, 8) : sat_s(v, esz * 4);
            uint64_t sw = op == 0x67 ? sat_u(w, 8) : sat_s(w, esz * 4);
            eset(&r, i, esz / 2, sv);
            eset(&r, n + i, esz / 2, sw);
        }
        break;
    }
    case 0x64: case 0x65: case 0x66: case 0x74: case 0x75: case 0x76: {
        int esz = (op == 0x64 || op == 0x74) ? 1 : (op == 0x65 || op == 0x75) ? 2 : 4;
        n = width / esz;
        uint64_t all = esz == 4 ? 0xffffffffu : esz == 2 ? 0xffff : 0xff;
        for (int i = 0; i < n; i++) {
            bool t = op >= 0x74 ? eget(a, i, esz) == eget(b, i, esz) : egets(a, i, esz) > egets(b, i, esz);
            eset(&r, i, esz, t ? all : 0);
        }
        break;
    }
    case 0xd1: case 0xd2: case 0xd3: case 0xe1: case 0xe2: case 0xf1: case 0xf2: case 0xf3: {
        r = *a;
        int esz = (op & 3) == 1 ? 2 : (op & 3) == 2 ? 4 : 8;
        int kind = op >= 0xf0 ? 2 : op >= 0xe0 ? 1 : 0;
        shift_elems(&r, esz, kind, b->q[0], width);
        break;
    }
    case 0xd4: case 0xfb: case 0xfc: case 0xfd: case 0xfe: case 0xf8: case 0xf9: case 0xfa: {
        int esz = (op == 0xd4 || op == 0xfb) ? 8 : (op == 0xfc || op == 0xf8) ? 1 : (op == 0xfd || op == 0xf9) ? 2 : 4;
        bool sub = op == 0xfb || (op >= 0xf8 && op <= 0xfa);
        n = width / esz;
        for (int i = 0; i < n; i++)
            eset(&r, i, esz, sub ? eget(a, i, esz) - eget(b, i, esz) : eget(a, i, esz) + eget(b, i, esz));
        break;
    }
    case 0xd5: case 0xe5: case 0xe4: /* PMULLW, PMULHW, PMULHUW */
        n = width / 2;
        for (int i = 0; i < n; i++) {
            int64_t p = op == 0xe4 ? (int64_t)a->w[i] * b->w[i] : (int64_t)a->sw[i] * b->sw[i];
            r.w[i] = (uint16_t)(op == 0xd5 ? (uint64_t)p : (uint64_t)p >> 16);
        }
        break;
    case 0xd8: case 0xd9: case 0xdc: case 0xdd: case 0xe8: case 0xe9: case 0xec: case 0xed: {
        int esz = (op & 1) ? 2 : 1;
        bool uns = op < 0xe0, sub = (op & 0xf) < 0xc;
        n = width / esz;
        for (int i = 0; i < n; i++) {
            int64_t x = uns ? (int64_t)eget(a, i, esz) : egets(a, i, esz);
            int64_t y = uns ? (int64_t)eget(b, i, esz) : egets(b, i, esz);
            int64_t s = sub ? x - y : x + y;
            eset(&r, i, esz, uns ? sat_u(s, esz * 8) : sat_s(s, esz * 8));
        }
        break;
    }
    case 0xda: case 0xde: /* PMINUB/PMAXUB */
        for (int i = 0; i < width; i++)
            r.b[i] = op == 0xda ? (a->b[i] < b->b[i] ? a->b[i] : b->b[i]) : (a->b[i] > b->b[i] ? a->b[i] : b->b[i]);
        break;
    case 0xea: case 0xee: /* PMINSW/PMAXSW */
        for (int i = 0; i < width / 2; i++)
            r.sw[i] = op == 0xea ? (a->sw[i] < b->sw[i] ? a->sw[i] : b->sw[i]) : (a->sw[i] > b->sw[i] ? a->sw[i] : b->sw[i]);
        break;
    case 0xdb: r.q[0] = a->q[0] & b->q[0]; r.q[1] = a->q[1] & b->q[1]; break;
    case 0xdf: r.q[0] = ~a->q[0] & b->q[0]; r.q[1] = ~a->q[1] & b->q[1]; break;
    case 0xeb: r.q[0] = a->q[0] | b->q[0]; r.q[1] = a->q[1] | b->q[1]; break;
    case 0xef: r.q[0] = a->q[0] ^ b->q[0]; r.q[1] = a->q[1] ^ b->q[1]; break;
    case 0xe0: for (int i = 0; i < width; i++) r.b[i] = (uint8_t)((a->b[i] + b->b[i] + 1) >> 1); break;
    case 0xe3: for (int i = 0; i < width / 2; i++) r.w[i] = (uint16_t)((a->w[i] + b->w[i] + 1) >> 1); break;
    case 0xf4: /* PMULUDQ */
        r.q[0] = (uint64_t)a->d[0] * b->d[0];
        if (width == 16) r.q[1] = (uint64_t)a->d[2] * b->d[2];
        break;
    case 0xf5: /* PMADDWD */
        for (int i = 0; i < width / 4; i++)
            r.sd[i] = (int32_t)((int64_t)a->sw[2 * i] * b->sw[2 * i] + (int64_t)a->sw[2 * i + 1] * b->sw[2 * i + 1]);
        break;
    case 0xf6: /* PSADBW */
        for (int h = 0; h < width / 8; h++) {
            unsigned s = 0;
            for (int i = 0; i < 8; i++) {
                int x = a->b[8 * h + i], y = b->b[8 * h + i];
                s += (unsigned)(x > y ? x - y : y - x);
            }
            r.q[h] = s;
        }
        break;
    default:
        return false;
    }
    if (width == 8) r.q[1] = a->q[1];
    *a = r;
    return true;
}

/* ------------------------------------------------------ ponto flutuante */

/* Regras de NaN do x86: propaga o primeiro operando NaN (silenciado); se a
 * operacao gerar NaN de operandos validos, retorna o "real indefinite" (negativo). */
static float nan_f(float r, float a, float b, bool binary)
{
    if (!isnan(r))
        return r;
    uint32_t v;
    if (binary && isnan(a)) memcpy(&v, &a, 4);
    else if (isnan(b)) memcpy(&v, &b, 4);
    else v = 0xffc00000u;
    v |= 0x00400000u;
    memcpy(&r, &v, 4);
    return r;
}

static double nan_d(double r, double a, double b, bool binary)
{
    if (!isnan(r))
        return r;
    uint64_t v;
    if (binary && isnan(a)) memcpy(&v, &a, 8);
    else if (isnan(b)) memcpy(&v, &b, 8);
    else v = 0xfff8000000000000ULL;
    v |= 0x0008000000000000ULL;
    memcpy(&r, &v, 8);
    return r;
}

static double fop(int op, double a, double b)
{
    switch (op) {
    case 0x58: return a + b;
    case 0x59: return a * b;
    case 0x5c: return a - b;
    case 0x5e: return a / b;
    case 0x5d: return a < b ? a : b;
    case 0x5f: return a > b ? a : b;
    default: return 0;
    }
}

static bool fcmp(int pred, double a, double b)
{
    bool un = isnan(a) || isnan(b);
    switch (pred & 7) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    default: return !un;
    }
}

static void comis(x86_cpu *c, double a, double b)
{
    uint64_t f = 0;
    if (isnan(a) || isnan(b)) f = EFL_ZF | EFL_PF | EFL_CF;
    else if (a < b) f = EFL_CF;
    else if (a == b) f = EFL_ZF;
    x86_set_arith_flags(c, f);
}

/* ------------------------------------------------------------ FXSAVE */

void sse_fxsave(x86_cpu *c, uint64_t lin, bool rex_w)
{
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    x87_fxsave_regs(c, buf);
    if (!rex_w) { /* formato de 32 bits: FIP/FCS/FDP/FDS */
        st_le(buf + 12, 0, 4);
        st_le(buf + 20, 0, 4);
    }
    st_le(buf + 24, c->mxcsr, 4);
    st_le(buf + 28, 0xffff, 4);
    int nx = c->code64 ? 16 : 8;
    for (int i = 0; i < nx; i++)
        memcpy(buf + 160 + 16 * i, &c->xmm[i], 16);
    for (int i = 0; i < 512; i += 8)
        x86_wr_lin(c, lin + (uint64_t)i, ld_le(buf + i, 8), 8);
}

void sse_fxrstor(x86_cpu *c, uint64_t lin, bool rex_w)
{
    uint8_t buf[512];
    (void)rex_w;
    for (int i = 0; i < 512; i += 8)
        st_le(buf + i, x86_rd_lin(c, lin + (uint64_t)i, 8), 8);
    uint32_t mx = (uint32_t)ld_le(buf + 24, 4);
    if (mx & 0xffff0000u)
        x86_gp(c, 0);
    x87_fxrstor_regs(c, buf);
    c->mxcsr = mx;
    int nx = c->code64 ? 16 : 8;
    for (int i = 0; i < nx; i++)
        memcpy(&c->xmm[i], buf + 160 + 16 * i, 16);
}

/* ------------------------------------------------- SSSE3 / SSE4.1 / SSE4.2 */

static float quiet_f(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    u |= 0x00400000u;
    memcpy(&v, &u, 4);
    return v;
}

static double quiet_d(double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    u |= 0x0008000000000000ULL;
    memcpy(&v, &u, 8);
    return v;
}

/* ROUNDPS/PD/SS/SD: imm[1:0] modo, imm[2] = usa o modo do MXCSR */
static double round_imm(x86_cpu *c, double v, int imm)
{
    int rc = (imm & 4) ? mxcsr_rc(c) : (imm & 3);
    return rnd(v, rc);
}

/* operacoes inteiras SSSE3/SSE4 do mapa 0F 38 sobre a (destino) e b; width 8 ou 16 */
static bool op38_int(x86_cpu *c, int op, xmm_reg *a, const xmm_reg *b, int width)
{
    xmm_reg r = {{0}};
    int n;
    switch (op) {
    case 0x00: /* PSHUFB */
        for (int i = 0; i < width; i++)
            r.b[i] = (b->b[i] & 0x80) ? 0 : a->b[b->b[i] & (width - 1)];
        break;
    case 0x01: case 0x02: case 0x03: case 0x05: case 0x06: case 0x07: { /* PHADD/PHSUB(S) W/D */
        int esz = (op == 0x02 || op == 0x06) ? 4 : 2;
        bool sub = op >= 0x05, sat = op == 0x03 || op == 0x07;
        n = width / esz;
        for (int i = 0; i < n; i++) {
            const xmm_reg *src = i < n / 2 ? a : b;
            int k = 2 * (i % (n / 2));
            int64_t x = egets(src, k, esz), y = egets(src, k + 1, esz);
            int64_t v = sub ? x - y : x + y;
            eset(&r, i, esz, sat ? sat_s(v, esz * 8) : (uint64_t)v);
        }
        break;
    }
    case 0x04: /* PMADDUBSW */
        for (int i = 0; i < width / 2; i++) {
            int64_t v = (int64_t)a->b[2 * i] * b->sb[2 * i] + (int64_t)a->b[2 * i + 1] * b->sb[2 * i + 1];
            r.w[i] = (uint16_t)sat_s(v, 16);
        }
        break;
    case 0x08: case 0x09: case 0x0a: { /* PSIGNB/W/D */
        int esz = 1 << (op - 0x08);
        n = width / esz;
        for (int i = 0; i < n; i++) {
            int64_t s = egets(b, i, esz), v = egets(a, i, esz);
            eset(&r, i, esz, (uint64_t)(s < 0 ? -v : s == 0 ? 0 : v));
        }
        break;
    }
    case 0x0b: /* PMULHRSW */
        for (int i = 0; i < width / 2; i++) {
            int32_t p = (int32_t)a->sw[i] * b->sw[i];
            r.w[i] = (uint16_t)(((p >> 14) + 1) >> 1);
        }
        break;
    case 0x1c: case 0x1d: case 0x1e: { /* PABSB/W/D */
        int esz = 1 << (op - 0x1c);
        n = width / esz;
        for (int i = 0; i < n; i++) {
            int64_t v = egets(b, i, esz);
            eset(&r, i, esz, (uint64_t)(v < 0 ? -v : v));
        }
        break;
    }
    default:
        (void)c;
        return false;
    }
    *a = r;
    return true;
}

/* PCMPESTRx/PCMPISTRx (SSE4.2): devolve IntRes2 (bits por elemento) e calcula os flags */
static uint32_t pcmpstr(x86_cpu *c, const xmm_reg *a, const xmm_reg *b, int imm, bool expl, bool rexw, int *nout)
{
    bool word = imm & 1, sgn = imm & 2;
    int n = word ? 8 : 16, esz = word ? 2 : 1;
    int la, lb;
    if (expl) {
        int64_t ra = rexw ? (int64_t)c->r[R_AX] : (int64_t)(int32_t)c->r[R_AX];
        int64_t rd = rexw ? (int64_t)c->r[R_DX] : (int64_t)(int32_t)c->r[R_DX];
        ra = ra < 0 ? -ra : ra;
        rd = rd < 0 ? -rd : rd;
        /* |INT_MIN| continua negativo como inteiro: satura */
        la = (ra < 0 || ra > n) ? n : (int)ra;
        lb = (rd < 0 || rd > n) ? n : (int)rd;
    } else {
        la = lb = n;
        for (int i = 0; i < n; i++)
            if (!eget(a, i, esz)) { la = i; break; }
        for (int i = 0; i < n; i++)
            if (!eget(b, i, esz)) { lb = i; break; }
    }
#define EL(r, i) (sgn ? egets(r, i, esz) : (int64_t)eget(r, i, esz))
    uint32_t res = 0;
    int agg = (imm >> 2) & 3;
    for (int j = 0; j < n; j++) {
        bool bit = false;
        switch (agg) {
        case 0: /* igual a qualquer */
            if (j < lb)
                for (int i = 0; i < la; i++)
                    if (EL(a, i) == EL(b, j)) { bit = true; break; }
            break;
        case 1: /* faixas */
            if (j < lb)
                for (int i = 0; i + 1 < la; i += 2)
                    if (EL(a, i) <= EL(b, j) && EL(b, j) <= EL(a, i + 1)) { bit = true; break; }
            break;
        case 2: /* igual elemento a elemento */
            if (j >= la && j >= lb) bit = true;
            else if (j >= la || j >= lb) bit = false;
            else bit = EL(a, j) == EL(b, j);
            break;
        default: /* igual ordenado (substring) */
            bit = true;
            for (int i = 0; i < n - j; i++) {
                int k = i + j;
                bool v;
                if (i >= la) v = true;
                else if (k >= lb) v = false;
                else v = EL(a, i) == EL(b, k);
                if (!v) { bit = false; break; }
            }
            break;
        }
        if (bit)
            res |= 1u << j;
    }
#undef EL
    uint32_t all = (1u << n) - 1;
    switch ((imm >> 4) & 3) {
    case 1: res = ~res & all; break;
    case 3: res ^= ((1u << lb) - 1) & all; break;
    default: break;
    }
    uint64_t f = 0;
    if (res) f |= EFL_CF;
    if (lb < n) f |= EFL_ZF;
    if (la < n) f |= EFL_SF;
    if (res & 1) f |= EFL_OF;
    x86_set_arith_flags(c, f);
    *nout = n;
    return res;
}

/* mapas 0F 38 e 0F 3A (SSSE3, SSE4.1, SSE4.2). map = 0x38 ou 0x3a. */
static bool sse_exec3(x86_cpu *c, x86_dec *d, int map, int op, int pfx)
{
    if (pfx >= 2)
        return false;
    x86_decode_modrm(c, d);
    int imm = map == 0x3a ? x86_fetch8(c) : 0; /* antes de acessar a memoria (RIP-relativo) */
    int rg = d->reg;
    xmm_reg t, *dst = XR(c, rg);
    bool rexw = (d->rex >> 3) & 1;

    if (map == 0x38) {
        bool ssse3 = op <= 0x0b || (op >= 0x1c && op <= 0x1e);
        if (ssse3) {
            if (pfx == 0) { /* forma MMX */
                xmm_reg a = {{0}}, b = {{0}};
                a.q[0] = c->mmx[rg & 7];
                b.q[0] = ld_mm(c, d);
                if (!op38_int(c, op, &a, &b, 8))
                    return false;
                c->mmx[rg & 7] = a.q[0];
                return true;
            }
            ld_x(c, d, &t, 16);
            return op38_int(c, op, dst, &t, 16);
        }
        if (pfx != 1)
            return false;
        xmm_reg r = {{0}};
        const xmm_reg *x0 = XR(c, 0);
        switch (op) {
        case 0x10: /* PBLENDVB */
            ld_x(c, d, &t, 16);
            for (int i = 0; i < 16; i++) r.b[i] = (x0->b[i] & 0x80) ? t.b[i] : dst->b[i];
            *dst = r;
            return true;
        case 0x14: /* BLENDVPS */
            ld_x(c, d, &t, 16);
            for (int i = 0; i < 4; i++) r.d[i] = (x0->d[i] >> 31) ? t.d[i] : dst->d[i];
            *dst = r;
            return true;
        case 0x15: /* BLENDVPD */
            ld_x(c, d, &t, 16);
            for (int i = 0; i < 2; i++) r.q[i] = (x0->q[i] >> 63) ? t.q[i] : dst->q[i];
            *dst = r;
            return true;
        case 0x17: { /* PTEST */
            ld_x(c, d, &t, 16);
            bool zf = !((dst->q[0] & t.q[0]) | (dst->q[1] & t.q[1]));
            bool cf = !((~dst->q[0] & t.q[0]) | (~dst->q[1] & t.q[1]));
            x86_set_arith_flags(c, (zf ? EFL_ZF : 0) | (cf ? EFL_CF : 0));
            return true;
        }
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: { /* PMOVSX/PMOVZX */
            static const int8_t from[6] = {1, 1, 1, 2, 2, 4}, to[6] = {2, 4, 8, 4, 8, 8};
            int k = op & 7, fs = from[k], ts = to[k], cnt = 16 / ts;
            bool sx = op < 0x30;
            ld_x(c, d, &t, cnt * fs);
            for (int i = 0; i < cnt; i++)
                eset(&r, i, ts, sx ? (uint64_t)egets(&t, i, fs) : eget(&t, i, fs));
            *dst = r;
            return true;
        }
        case 0x28: /* PMULDQ */
            ld_x(c, d, &t, 16);
            r.sq[0] = (int64_t)dst->sd[0] * t.sd[0];
            r.sq[1] = (int64_t)dst->sd[2] * t.sd[2];
            *dst = r;
            return true;
        case 0x29: case 0x37: /* PCMPEQQ, PCMPGTQ */
            ld_x(c, d, &t, 16);
            for (int i = 0; i < 2; i++)
                r.q[i] = (op == 0x29 ? dst->q[i] == t.q[i] : dst->sq[i] > t.sq[i]) ? ~0ULL : 0;
            *dst = r;
            return true;
        case 0x2a: /* MOVNTDQA */
            if (!d->mem) return false;
            ld_x(c, d, dst, 16);
            return true;
        case 0x2b: /* PACKUSDW */
            ld_x(c, d, &t, 16);
            for (int i = 0; i < 4; i++) {
                r.w[i] = (uint16_t)sat_u(dst->sd[i], 16);
                r.w[i + 4] = (uint16_t)sat_u(t.sd[i], 16);
            }
            *dst = r;
            return true;
        case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: { /* PMIN/PMAX */
            static const int8_t es[8] = {1, 4, 2, 4, 1, 4, 2, 4};
            static const bool sg[8] = {true, true, false, false, true, true, false, false};
            int k = op & 7, esz = es[k];
            bool mx = op >= 0x3c;
            ld_x(c, d, &t, 16);
            for (int i = 0; i < 16 / esz; i++) {
                bool lt = sg[k] ? egets(dst, i, esz) < egets(&t, i, esz) : eget(dst, i, esz) < eget(&t, i, esz);
                eset(&r, i, esz, (lt != mx) ? eget(dst, i, esz) : eget(&t, i, esz));
            }
            *dst = r;
            return true;
        }
        case 0x40: /* PMULLD */
            ld_x(c, d, &t, 16);
            for (int i = 0; i < 4; i++) r.d[i] = dst->d[i] * t.d[i];
            *dst = r;
            return true;
        case 0x41: { /* PHMINPOSUW */
            ld_x(c, d, &t, 16);
            int best = 0;
            for (int i = 1; i < 8; i++)
                if (t.w[i] < t.w[best]) best = i;
            r.w[0] = t.w[best];
            r.w[1] = (uint16_t)best;
            *dst = r;
            return true;
        }
        default:
            return false;
        }
    }

    /* mapa 0F 3A */
    if (op == 0x0f) { /* PALIGNR (MMX e XMM) */
        int w = pfx ? 16 : 8;
        uint8_t buf[32] = {0};
        if (pfx) {
            ld_x(c, d, &t, 16);
            memcpy(buf, t.b, 16);
            memcpy(buf + 16, dst->b, 16);
        } else {
            uint64_t s = ld_mm(c, d), dd = c->mmx[rg & 7];
            memcpy(buf, &s, 8);
            memcpy(buf + 8, &dd, 8);
        }
        uint8_t out[16] = {0};
        for (int i = 0; i < w; i++)
            out[i] = imm + i < 2 * w ? buf[imm + i] : 0;
        if (pfx)
            memcpy(dst->b, out, 16);
        else
            memcpy(&c->mmx[rg & 7], out, 8);
        return true;
    }
    if (pfx != 1)
        return false;
    xmm_reg r = {{0}};
    switch (op) {
    case 0x08: case 0x09: /* ROUNDPS/PD */
        ld_x(c, d, &t, 16);
        if (op == 0x08)
            for (int i = 0; i < 4; i++) r.f[i] = isnan(t.f[i]) ? quiet_f(t.f[i]) : (float)round_imm(c, t.f[i], imm);
        else
            for (int i = 0; i < 2; i++) r.fd[i] = isnan(t.fd[i]) ? quiet_d(t.fd[i]) : round_imm(c, t.fd[i], imm);
        *dst = r;
        return true;
    case 0x0a: /* ROUNDSS */
        ld_x(c, d, &t, 4);
        dst->f[0] = isnan(t.f[0]) ? quiet_f(t.f[0]) : (float)round_imm(c, t.f[0], imm);
        return true;
    case 0x0b: /* ROUNDSD */
        ld_x(c, d, &t, 8);
        dst->fd[0] = isnan(t.fd[0]) ? quiet_d(t.fd[0]) : round_imm(c, t.fd[0], imm);
        return true;
    case 0x0c: /* BLENDPS */
        ld_x(c, d, &t, 16);
        for (int i = 0; i < 4; i++) if (imm >> i & 1) dst->d[i] = t.d[i];
        return true;
    case 0x0d: /* BLENDPD */
        ld_x(c, d, &t, 16);
        for (int i = 0; i < 2; i++) if (imm >> i & 1) dst->q[i] = t.q[i];
        return true;
    case 0x0e: /* PBLENDW */
        ld_x(c, d, &t, 16);
        for (int i = 0; i < 8; i++) if (imm >> i & 1) dst->w[i] = t.w[i];
        return true;
    case 0x14: case 0x15: case 0x16: case 0x17: { /* PEXTRB/W/D/Q, EXTRACTPS: r/m = destino */
        int esz = op == 0x14 ? 1 : op == 0x15 ? 2 : (op == 0x16 && rexw) ? 8 : 4;
        uint64_t v = eget(dst, imm & (16 / esz - 1), esz);
        if (d->mem)
            x86_wr_lin(c, x86_ea_lin(c, d), v, (unsigned)esz);
        else
            x86_reg_write(c, d->rm, esz == 8 ? 8 : 4, v, true);
        return true;
    }
    case 0x20: { /* PINSRB */
        uint64_t v = d->mem ? x86_rd_lin(c, x86_ea_lin(c, d), 1) : x86_reg_read(c, d->rm, 4, true);
        dst->b[imm & 15] = (uint8_t)v;
        return true;
    }
    case 0x21: { /* INSERTPS */
        uint32_t v = d->mem ? (uint32_t)x86_rd_lin(c, x86_ea_lin(c, d), 4) : XR(c, d->rm)->d[(imm >> 6) & 3];
        dst->d[(imm >> 4) & 3] = v;
        for (int i = 0; i < 4; i++) if (imm >> i & 1) dst->d[i] = 0;
        return true;
    }
    case 0x22: /* PINSRD/PINSRQ */
        if (rexw)
            dst->q[imm & 1] = d->mem ? x86_rd_lin(c, x86_ea_lin(c, d), 8) : x86_reg_read(c, d->rm, 8, true);
        else
            dst->d[imm & 3] = (uint32_t)(d->mem ? x86_rd_lin(c, x86_ea_lin(c, d), 4) : x86_reg_read(c, d->rm, 4, true));
        return true;
    case 0x40: { /* DPPS */
        ld_x(c, d, &t, 16);
        float p[4];
        for (int i = 0; i < 4; i++) /* regras de NaN do x86 em cada operacao */
            p[i] = (imm >> (4 + i) & 1) ? nan_f(dst->f[i] * t.f[i], dst->f[i], t.f[i], true) : 0.0f;
        float s1 = nan_f(p[0] + p[1], p[0], p[1], true), s2 = nan_f(p[2] + p[3], p[2], p[3], true);
        float sum = nan_f(s1 + s2, s1, s2, true);
        for (int i = 0; i < 4; i++) r.f[i] = (imm >> i & 1) ? sum : 0.0f;
        *dst = r;
        return true;
    }
    case 0x41: { /* DPPD */
        ld_x(c, d, &t, 16);
        double p0 = (imm & 0x10) ? nan_d(dst->fd[0] * t.fd[0], dst->fd[0], t.fd[0], true) : 0.0;
        double p1 = (imm & 0x20) ? nan_d(dst->fd[1] * t.fd[1], dst->fd[1], t.fd[1], true) : 0.0;
        double sum = nan_d(p0 + p1, p0, p1, true);
        for (int i = 0; i < 2; i++) r.fd[i] = (imm >> i & 1) ? sum : 0.0;
        *dst = r;
        return true;
    }
    case 0x42: { /* MPSADBW */
        ld_x(c, d, &t, 16);
        int oa = (imm & 4) ? 4 : 0, ob = (imm & 3) * 4;
        for (int i = 0; i < 8; i++) {
            int sum = 0;
            for (int k = 0; k < 4; k++) {
                int x = dst->b[oa + i + k] - t.b[ob + k];
                sum += x < 0 ? -x : x;
            }
            r.w[i] = (uint16_t)sum;
        }
        *dst = r;
        return true;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: { /* PCMPESTRM/I, PCMPISTRM/I */
        ld_x(c, d, &t, 16);
        int n;
        uint32_t res = pcmpstr(c, dst, &t, imm, op <= 0x61, rexw, &n);
        if (op & 1) { /* indice em ECX */
            uint32_t idx = (uint32_t)n;
            if (res)
                idx = (imm & 0x40) ? (uint32_t)(31 - __builtin_clz(res)) : (uint32_t)__builtin_ctz(res);
            c->r[R_CX] = idx;
        } else { /* mascara em XMM0 */
            xmm_reg m = {{0}};
            if (imm & 0x40) {
                int esz = n == 8 ? 2 : 1;
                for (int i = 0; i < n; i++) eset(&m, i, esz, (res >> i & 1) ? ~0ULL : 0);
            } else {
                m.q[0] = res;
            }
            *XR(c, 0) = m;
        }
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------ despacho */

bool sse_exec(x86_cpu *c, x86_dec *d, int op)
{
    int pfx = d->rep ? 3 : d->repne ? 2 : d->opsize_prefix ? 1 : 0;
    if (op > 0xffff) { /* 0F 38 xx / 0F 3A xx */
        if (c->cr0 & CR0_EM)
            x86_ud(c);
        if (c->cr0 & CR0_TS)
            x86_exception(c, EXC_NM, 0, 0);
        return sse_exec3(c, d, (op >> 8) & 0xff, op & 0xff, pfx);
    }
    op &= 0xff;

    /* opcodes que nao sao SSE/MMX */
    bool known = (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2f) || (op >= 0x50 && op <= 0x7f) ||
                 op == 0xc2 || (op >= 0xc3 && op <= 0xc6) || (op >= 0xd0 && op <= 0xfe);
    if (!known)
        return false;
    if (c->cr0 & CR0_EM)
        x86_ud(c);
    if (c->cr0 & CR0_TS)
        x86_exception(c, EXC_NM, 0, 0);
    if (op == 0x77) /* EMMS */
        return true;
    x86_decode_modrm(c, d);
    int rg = d->reg;
    xmm_reg t, *dst = XR(c, rg);
    bool rexw = (d->rex >> 3) & 1;

    switch (op) {
    case 0x10:
        if (pfx == 3) { ld_x(c, d, &t, 4); if (d->mem) *dst = t; else dst->d[0] = t.d[0]; }
        else if (pfx == 2) { ld_x(c, d, &t, 8); if (d->mem) *dst = t; else dst->q[0] = t.q[0]; }
        else { ld_x(c, d, &t, 16); *dst = t; }
        return true;
    case 0x11:
        if (d->mem) st_mem(c, d, dst, pfx == 3 ? 4 : pfx == 2 ? 8 : 16);
        else if (pfx == 3) XR(c, d->rm)->d[0] = dst->d[0];
        else if (pfx == 2) XR(c, d->rm)->q[0] = dst->q[0];
        else *XR(c, d->rm) = *dst;
        return true;
    case 0x12:
        if (pfx == 3) { /* MOVSLDUP */
            ld_x(c, d, &t, 16);
            dst->d[0] = dst->d[1] = t.d[0];
            dst->d[2] = dst->d[3] = t.d[2];
            return true;
        }
        if (pfx == 2) { /* MOVDDUP */
            ld_x(c, d, &t, 8);
            dst->q[0] = dst->q[1] = t.q[0];
            return true;
        }
        if (d->mem) { ld_x(c, d, &t, 8); dst->q[0] = t.q[0]; }
        else dst->q[0] = XR(c, d->rm)->q[1]; /* MOVHLPS */
        return true;
    case 0x13: case 0x17:
        if (!d->mem || pfx >= 2) return false;
        {
            xmm_reg v = {{0}};
            v.q[0] = op == 0x13 ? dst->q[0] : dst->q[1];
            st_mem(c, d, &v, 8);
        }
        return true;
    case 0x14: case 0x15:
        ld_x(c, d, &t, 16);
        if (pfx == 1) {
            if (op == 0x14) dst->q[1] = t.q[0];
            else { dst->q[0] = dst->q[1]; dst->q[1] = t.q[1]; }
        } else if (pfx == 0) {
            xmm_reg r;
            int base = op == 0x14 ? 0 : 2;
            r.d[0] = dst->d[base]; r.d[1] = t.d[base]; r.d[2] = dst->d[base + 1]; r.d[3] = t.d[base + 1];
            *dst = r;
        } else return false;
        return true;
    case 0x16:
        if (pfx == 3) { /* MOVSHDUP */
            ld_x(c, d, &t, 16);
            dst->d[0] = dst->d[1] = t.d[1];
            dst->d[2] = dst->d[3] = t.d[3];
            return true;
        }
        if (pfx >= 2) return false;
        if (d->mem) { ld_x(c, d, &t, 8); dst->q[1] = t.q[0]; }
        else dst->q[1] = XR(c, d->rm)->q[0]; /* MOVLHPS */
        return true;
    case 0x28: if (pfx >= 2) return false; ld_x(c, d, &t, 16); *dst = t; return true;
    case 0x29: case 0x2b:
        if (pfx >= 2) return false;
        if (d->mem) st_mem(c, d, dst, 16);
        else if (op == 0x29) *XR(c, d->rm) = *dst;
        else return false;
        return true;
    case 0x2a:
        if (pfx >= 2) {
            int isz = rexw ? 8 : 4;
            int64_t v = (int64_t)sext_sz(x86_rm_read(c, d, isz), isz);
            if (pfx == 3) dst->f[0] = (float)v;
            else dst->fd[0] = (double)v;
        } else {
            uint64_t m = ld_mm(c, d);
            int32_t lo = (int32_t)m, hi = (int32_t)(m >> 32);
            if (pfx == 0) { dst->f[0] = (float)lo; dst->f[1] = (float)hi; }
            else { dst->fd[0] = lo; dst->fd[1] = hi; }
        }
        return true;
    case 0x2c: case 0x2d: {
        int rc = op == 0x2c ? 3 : mxcsr_rc(c);
        if (pfx >= 2) {
            ld_x(c, d, &t, pfx == 3 ? 4 : 8);
            double v = pfx == 3 ? t.f[0] : t.fd[0];
            int isz = rexw ? 8 : 4;
            x86_reg_write(c, rg, isz, (uint64_t)cvt_i(v, rc, isz), true);
        } else {
            ld_x(c, d, &t, pfx == 0 ? 8 : 16);
            double v0 = pfx == 0 ? t.f[0] : t.fd[0], v1 = pfx == 0 ? t.f[1] : t.fd[1];
            c->mmx[rg & 7] = (uint32_t)cvt_i(v0, rc, 4) | ((uint64_t)(uint32_t)cvt_i(v1, rc, 4) << 32);
        }
        return true;
    }
    case 0x2e: case 0x2f:
        if (pfx >= 2) return false;
        ld_x(c, d, &t, pfx ? 8 : 4);
        comis(c, pfx ? dst->fd[0] : dst->f[0], pfx ? t.fd[0] : t.f[0]);
        return true;
    case 0x50: {
        if (pfx >= 2 || d->mem) return false;
        xmm_reg *s = XR(c, d->rm);
        uint32_t m = 0;
        if (pfx == 0) for (int i = 0; i < 4; i++) m |= (s->d[i] >> 31) << i;
        else for (int i = 0; i < 2; i++) m |= (uint32_t)(s->q[i] >> 63) << i;
        x86_reg_write(c, rg, 4, m, true);
        return true;
    }
    case 0x51: case 0x52: case 0x53:
    case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        bool dbl = pfx == 1 || pfx == 2, scalar = pfx >= 2;
        ld_x(c, d, &t, scalar ? (dbl ? 8 : 4) : 16);
        int n = scalar ? 1 : dbl ? 2 : 4;
        if ((op == 0x52 || op == 0x53) && dbl) return false;
        bool binary = op >= 0x58;
        bool minmax = op == 0x5d || op == 0x5f;
        for (int i = 0; i < n; i++) {
            if (dbl) {
                double a = dst->fd[i], b = t.fd[i], r;
                if (op == 0x51) r = sqrt(b);
                else r = fop(op, a, b);
                dst->fd[i] = minmax ? r : nan_d(r, a, b, binary);
            } else {
                float a = dst->f[i], b = t.f[i], r;
                if (op == 0x51) r = sqrtf(b);
                else if (op == 0x52) r = 1.0f / sqrtf(b);
                else if (op == 0x53) r = 1.0f / b;
                else if (op == 0x58) r = a + b;
                else if (op == 0x59) r = a * b;
                else if (op == 0x5c) r = a - b;
                else if (op == 0x5e) r = a / b;
                else if (op == 0x5d) r = a < b ? a : b; /* sem conversao: preserva SNaN */
                else r = a > b ? a : b;
                dst->f[i] = minmax ? r : nan_f(r, a, b, binary);
            }
        }
        return true;
    }
    case 0x54: case 0x55: case 0x56: case 0x57:
        if (pfx >= 2) return false;
        ld_x(c, d, &t, 16);
        for (int i = 0; i < 2; i++) {
            uint64_t a = dst->q[i], b = t.q[i];
            dst->q[i] = op == 0x54 ? a & b : op == 0x55 ? ~a & b : op == 0x56 ? a | b : a ^ b;
        }
        return true;
    case 0x5a:
        switch (pfx) {
        case 0: ld_x(c, d, &t, 8); { double a = t.f[0], b = t.f[1]; dst->fd[0] = a; dst->fd[1] = b; } break;
        case 1: ld_x(c, d, &t, 16); dst->f[0] = (float)t.fd[0]; dst->f[1] = (float)t.fd[1]; dst->q[1] = 0; break;
        case 3: ld_x(c, d, &t, 4); dst->fd[0] = t.f[0]; break;
        default: ld_x(c, d, &t, 8); dst->f[0] = (float)t.fd[0]; break;
        }
        return true;
    case 0x5b:
        ld_x(c, d, &t, 16);
        if (pfx == 0) for (int i = 0; i < 4; i++) dst->f[i] = (float)t.sd[i];
        else if (pfx == 1 || pfx == 3) {
            int rc = pfx == 3 ? 3 : mxcsr_rc(c);
            for (int i = 0; i < 4; i++) dst->sd[i] = (int32_t)cvt_i(t.f[i], rc, 4);
        } else return false;
        return true;
    case 0xe6:
        if (pfx == 0) return false;
        if (pfx == 3) { /* CVTDQ2PD */
            ld_x(c, d, &t, 8);
            int32_t a = t.sd[0], b = t.sd[1];
            dst->fd[0] = a;
            dst->fd[1] = b;
        } else {
            ld_x(c, d, &t, 16);
            int rc = pfx == 1 ? 3 : mxcsr_rc(c);
            dst->sd[0] = (int32_t)cvt_i(t.fd[0], rc, 4);
            dst->sd[1] = (int32_t)cvt_i(t.fd[1], rc, 4);
            dst->q[1] = 0;
        }
        return true;
    case 0xc2: {
        bool dbl = pfx == 1 || pfx == 2, scalar = pfx >= 2;
        int pred = x86_fetch8(c);
        ld_x(c, d, &t, scalar ? (dbl ? 8 : 4) : 16);
        int n = scalar ? 1 : dbl ? 2 : 4;
        for (int i = 0; i < n; i++) {
            bool r = fcmp(pred, dbl ? dst->fd[i] : dst->f[i], dbl ? t.fd[i] : t.f[i]);
            if (dbl) dst->q[i] = r ? ~0ULL : 0;
            else dst->d[i] = r ? ~0u : 0;
        }
        return true;
    }
    case 0xc3:
        if (pfx || !d->mem) return false;
        x86_rm_write(c, d, rexw ? 8 : 4, c->r[rg]);
        return true;
    case 0xc4: {
        int imm = x86_fetch8(c);
        uint16_t v = (uint16_t)x86_rm_read(c, d, 2);
        if (pfx == 1) dst->w[imm & 7] = v;
        else if (pfx == 0) {
            int s = (imm & 3) * 16;
            c->mmx[rg & 7] = (c->mmx[rg & 7] & ~(0xffffULL << s)) | ((uint64_t)v << s);
        } else return false;
        return true;
    }
    case 0xc5: {
        if (d->mem) return false;
        int imm = x86_fetch8(c);
        uint64_t v;
        if (pfx == 1) v = XR(c, d->rm)->w[imm & 7];
        else if (pfx == 0) v = (c->mmx[d->rm & 7] >> ((imm & 3) * 16)) & 0xffff;
        else return false;
        x86_reg_write(c, rg, 4, v, true);
        return true;
    }
    case 0xc6: {
        int imm = x86_fetch8(c);
        ld_x(c, d, &t, 16);
        xmm_reg r;
        if (pfx == 0) {
            r.d[0] = dst->d[imm & 3];
            r.d[1] = dst->d[(imm >> 2) & 3];
            r.d[2] = t.d[(imm >> 4) & 3];
            r.d[3] = t.d[(imm >> 6) & 3];
        } else if (pfx == 1) {
            r.q[0] = dst->q[imm & 1];
            r.q[1] = t.q[(imm >> 1) & 1];
        } else return false;
        *dst = r;
        return true;
    }
    case 0x6e: {
        int isz = rexw ? 8 : 4;
        uint64_t v = x86_rm_read(c, d, isz);
        if (pfx == 1) { dst->q[0] = v; dst->q[1] = 0; }
        else if (pfx == 0) c->mmx[rg & 7] = v;
        else return false;
        return true;
    }
    case 0x7e: {
        if (pfx == 3) { /* MOVQ xmm, xmm/m64 */
            ld_x(c, d, &t, 8);
            dst->q[0] = t.q[0];
            dst->q[1] = 0;
            return true;
        }
        int isz = rexw ? 8 : 4;
        uint64_t v;
        if (pfx == 1) v = dst->q[0];
        else if (pfx == 0) v = c->mmx[rg & 7];
        else return false;
        if (isz == 4) v = (uint32_t)v;
        if (!d->mem) x86_reg_write(c, d->rm, isz, v, true);
        else x86_wr_lin(c, x86_ea_lin(c, d), v, (unsigned)isz);
        return true;
    }
    case 0x6f:
        if (pfx == 0) { c->mmx[rg & 7] = ld_mm(c, d); return true; }
        if (pfx == 2) return false;
        ld_x(c, d, &t, 16);
        *dst = t;
        return true;
    case 0x7f:
        if (pfx == 0) {
            if (d->mem) x86_wr_lin(c, x86_ea_lin(c, d), c->mmx[rg & 7], 8);
            else c->mmx[d->rm & 7] = c->mmx[rg & 7];
            return true;
        }
        if (pfx == 2) return false;
        if (d->mem) st_mem(c, d, dst, 16);
        else *XR(c, d->rm) = *dst;
        return true;
    case 0xd6:
        if (pfx == 1) {
            if (d->mem) {
                xmm_reg v = {{0}};
                v.q[0] = dst->q[0];
                st_mem(c, d, &v, 8);
            } else {
                XR(c, d->rm)->q[0] = dst->q[0];
                XR(c, d->rm)->q[1] = 0;
            }
            return true;
        }
        if (d->mem) return false;
        if (pfx == 3) { dst->q[0] = c->mmx[d->rm & 7]; dst->q[1] = 0; return true; }
        if (pfx == 2) { c->mmx[rg & 7] = XR(c, d->rm)->q[0]; return true; }
        return false;
    case 0xe7:
        if (!d->mem) return false;
        if (pfx == 0) x86_wr_lin(c, x86_ea_lin(c, d), c->mmx[rg & 7], 8);
        else if (pfx == 1) st_mem(c, d, dst, 16);
        else return false;
        return true;
    case 0xd7: {
        if (d->mem) return false;
        uint32_t m = 0;
        if (pfx == 1) { xmm_reg *s = XR(c, d->rm); for (int i = 0; i < 16; i++) m |= (uint32_t)(s->b[i] >> 7) << i; }
        else if (pfx == 0) { uint64_t s = c->mmx[d->rm & 7]; for (int i = 0; i < 8; i++) m |= (uint32_t)((s >> (8 * i + 7)) & 1) << i; }
        else return false;
        x86_reg_write(c, rg, 4, m, true);
        return true;
    }
    case 0xf7: { /* MASKMOVQ/MASKMOVDQU */
        if (d->mem) return false;
        int seg = d->seg >= 0 ? d->seg : S_DS;
        uint64_t addr = c->r[R_DI] & szmask(d->asz);
        int n = pfx == 1 ? 16 : 8;
        xmm_reg src, mask;
        if (pfx == 1) { src = *dst; mask = *XR(c, d->rm); }
        else { src.q[0] = c->mmx[rg & 7]; mask.q[0] = c->mmx[d->rm & 7]; }
        for (int i = 0; i < n; i++)
            if (mask.b[i] & 0x80)
                x86_wr_lin(c, x86_lin(c, seg, addr + (uint64_t)i), src.b[i], 1);
        return true;
    }
    case 0x70: {
        int imm = x86_fetch8(c);
        if (pfx == 0) t.q[0] = ld_mm(c, d);
        else ld_x(c, d, &t, 16);
        xmm_reg r;
        switch (pfx) {
        case 0: {
            uint64_t s = t.q[0], v = 0;
            for (int i = 0; i < 4; i++) v |= ((s >> (16 * ((imm >> (2 * i)) & 3))) & 0xffff) << (16 * i);
            c->mmx[rg & 7] = v;
            return true;
        }
        case 1: for (int i = 0; i < 4; i++) r.d[i] = t.d[(imm >> (2 * i)) & 3]; break;
        case 3: r.q[0] = t.q[0]; for (int i = 0; i < 4; i++) r.w[4 + i] = t.w[4 + ((imm >> (2 * i)) & 3)]; break;
        default: r.q[1] = t.q[1]; for (int i = 0; i < 4; i++) r.w[i] = t.w[(imm >> (2 * i)) & 3]; break;
        }
        *dst = r;
        return true;
    }
    case 0x71: case 0x72: case 0x73: {
        if (d->mem || pfx >= 2) return false;
        int imm = x86_fetch8(c);
        int sub = d->reg & 7;
        int esz = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
        xmm_reg r;
        int width = pfx == 1 ? 16 : 8;
        if (pfx == 1) r = *XR(c, d->rm);
        else { r.q[0] = c->mmx[d->rm & 7]; r.q[1] = 0; }
        if (op == 0x73 && (sub == 3 || sub == 7)) { /* PSRLDQ / PSLLDQ */
            if (pfx != 1) return false;
            int n = imm > 16 ? 16 : imm;
            xmm_reg o = {{0}};
            for (int i = 0; i < 16; i++) {
                int j = sub == 3 ? i + n : i - n;
                o.b[i] = (j >= 0 && j < 16) ? r.b[j] : 0;
            }
            *XR(c, d->rm) = o;
            return true;
        }
        int kind = sub == 2 ? 0 : sub == 4 ? 1 : sub == 6 ? 2 : -1;
        if (kind < 0 || (kind == 1 && esz == 8)) return false;
        shift_elems(&r, esz, kind, (uint64_t)imm, width);
        if (pfx == 1) *XR(c, d->rm) = r;
        else c->mmx[d->rm & 7] = r.q[0];
        return true;
    }
    default:
        break;
    }

    /* SSE3: HADDPS/PD, HSUBPS/PD, ADDSUBPS/PD, LDDQU */
    if ((op == 0x7c || op == 0x7d || op == 0xd0) && (pfx == 1 || pfx == 2)) {
        ld_x(c, d, &t, 16);
        xmm_reg r = {{0}};
        bool sub = op == 0x7d;
        if (pfx == 1) { /* precisao dupla */
            double x[2] = {dst->fd[0], dst->fd[1]}, y[2] = {t.fd[0], t.fd[1]};
            if (op == 0xd0) {
                r.fd[0] = nan_d(x[0] - y[0], x[0], y[0], true);
                r.fd[1] = nan_d(x[1] + y[1], x[1], y[1], true);
            } else {
                r.fd[0] = nan_d(sub ? x[0] - x[1] : x[0] + x[1], x[0], x[1], true);
                r.fd[1] = nan_d(sub ? y[0] - y[1] : y[0] + y[1], y[0], y[1], true);
            }
        } else {
            float *x = dst->f, *y = t.f;
            if (op == 0xd0) {
                for (int i = 0; i < 4; i++)
                    r.f[i] = nan_f((i & 1) ? x[i] + y[i] : x[i] - y[i], x[i], y[i], true);
            } else {
                for (int i = 0; i < 4; i++) {
                    const float *v = i < 2 ? x : y;
                    int k = 2 * (i & 1);
                    r.f[i] = nan_f(sub ? v[k] - v[k + 1] : v[k] + v[k + 1], v[k], v[k + 1], true);
                }
            }
        }
        *dst = r;
        return true;
    }
    if (op == 0xf0 && pfx == 2) { /* LDDQU */
        if (!d->mem) return false;
        ld_x(c, d, dst, 16);
        return true;
    }

    /* operacoes inteiras genericas */
    if (pfx >= 2)
        return false;
    if ((op == 0x6c || op == 0x6d) && pfx != 1)
        return false;
    if (pfx == 1) {
        ld_x(c, d, &t, 16);
        return int_op(c, op, dst, &t, 16);
    }
    xmm_reg a = {{0}}, b = {{0}};
    a.q[0] = c->mmx[rg & 7];
    b.q[0] = ld_mm(c, d);
    if (!int_op(c, op, &a, &b, 8))
        return false;
    c->mmx[rg & 7] = a.q[0];
    return true;
}
