/* CPU x86: decodificacao e execucao de instrucoes (inteiras e de sistema). */
#include "x86_priv.h"

/* ------------------------------------------------------ registradores */



#define REX(d) ((d)->rex != 0)
#define REXW(d) (((d)->rex >> 3) & 1)
#define GREG(d, sz) x86_reg_read(c, (d)->reg, sz, REX(d))
#define SREG(d, sz, v) x86_reg_write(c, (d)->reg, sz, v, REX(d))

static inline uint16_t fetch16(x86_cpu *c)
{
    uint16_t v = x86_fetch8(c);
    return (uint16_t)(v | (x86_fetch8(c) << 8));
}

static inline uint64_t fetch64(x86_cpu *c)
{
    uint64_t lo = x86_fetch32(c);
    return lo | ((uint64_t)x86_fetch32(c) << 32);
}

static inline uint64_t fetch_imm(x86_cpu *c, int sz)
{
    switch (sz) {
    case 1: return (uint64_t)(int64_t)(int8_t)x86_fetch8(c);
    case 2: return (uint64_t)(int64_t)(int16_t)fetch16(c);
    default: return (uint64_t)(int64_t)(int32_t)x86_fetch32(c);
    }
}

/* ---------------------------------------------------------- ModRM */

static void decode_modrm(x86_cpu *c, x86_dec *d)
{
    uint8_t m = x86_fetch8(c);
    d->modrm = m;
    d->mod = m >> 6;
    d->reg = (uint8_t)(((m >> 3) & 7) | ((d->rex & 4) << 1));
    unsigned rm = m & 7;
    if (d->mod == 3) {
        d->mem = false;
        d->rm = (uint8_t)(rm | ((d->rex & 1) << 3));
        return;
    }
    d->mem = true;
    d->riprel = false;
    int def_seg = S_DS;
    uint64_t ea = 0;
    if (d->asz == 2) {
        switch (rm) {
        case 0: ea = c->r[R_BX] + c->r[R_SI]; break;
        case 1: ea = c->r[R_BX] + c->r[R_DI]; break;
        case 2: ea = c->r[R_BP] + c->r[R_SI]; def_seg = S_SS; break;
        case 3: ea = c->r[R_BP] + c->r[R_DI]; def_seg = S_SS; break;
        case 4: ea = c->r[R_SI]; break;
        case 5: ea = c->r[R_DI]; break;
        case 6:
            if (d->mod == 0) ea = fetch16(c);
            else { ea = c->r[R_BP]; def_seg = S_SS; }
            break;
        default: ea = c->r[R_BX]; break;
        }
        if (d->mod == 1) ea += (uint64_t)(int64_t)(int8_t)x86_fetch8(c);
        else if (d->mod == 2) ea += fetch16(c);
        ea &= 0xffff;
    } else {
        if (rm == 4) {
            uint8_t sib = x86_fetch8(c);
            unsigned scale = sib >> 6;
            unsigned index = ((sib >> 3) & 7) | ((d->rex & 2) << 2);
            unsigned base = (sib & 7) | ((d->rex & 1) << 3);
            if (index != 4)
                ea = c->r[index] << scale;
            if ((base & 7) == 5 && d->mod == 0) {
                ea += (uint64_t)(int64_t)(int32_t)x86_fetch32(c);
            } else {
                ea += c->r[base];
                if (base == R_SP || base == R_BP)
                    def_seg = S_SS;
            }
        } else if (rm == 5 && d->mod == 0) {
            ea = (uint64_t)(int64_t)(int32_t)x86_fetch32(c);
            if (c->code64)
                d->riprel = true;
        } else {
            unsigned base = rm | ((d->rex & 1) << 3);
            ea = c->r[base];
            if (base == R_BP)
                def_seg = S_SS;
        }
        if (d->mod == 1) ea += (uint64_t)(int64_t)(int8_t)x86_fetch8(c);
        else if (d->mod == 2) ea += (uint64_t)(int64_t)(int32_t)x86_fetch32(c);
        if (d->asz == 4)
            ea = (uint32_t)ea;
    }
    d->ea = ea;
    d->ea_seg = d->seg >= 0 ? d->seg : def_seg;
}


static inline uint64_t ea_off(x86_cpu *c, x86_dec *d)
{
    uint64_t off = d->ea;
    if (d->riprel) {
        off += c->rip;
        if (d->asz == 4)
            off = (uint32_t)off;
    }
    return off;
}



/* ------------------------------------------------------------- ALU */

static uint64_t alu(x86_cpu *c, int op, uint64_t a, uint64_t b, int sz)
{
    uint64_t m = szmask(sz), r, cin;
    a &= m;
    b &= m;
    switch (op) {
    case 0: r = (a + b) & m; set_lazy(c, CC_ADD, sz, r, a, b); break;
    case 1: r = a | b; set_lazy(c, CC_LOGIC, sz, r, 0, 0); break;
    case 2: cin = x86_cf(c) ? 1 : 0; r = (a + b + cin) & m; set_lazy(c, CC_ADC, sz, r, a, b); c->cc_aux = cin; break;
    case 3: cin = x86_cf(c) ? 1 : 0; r = (a - b - cin) & m; set_lazy(c, CC_SBB, sz, r, a, b); c->cc_aux = cin; break;
    case 4: r = a & b; set_lazy(c, CC_LOGIC, sz, r, 0, 0); break;
    case 5: case 7: r = (a - b) & m; set_lazy(c, CC_SUB, sz, r, a, b); break;
    default: r = a ^ b; set_lazy(c, CC_LOGIC, sz, r, 0, 0); break;
    }
    return r;
}

static uint64_t szp(uint64_t r, int sz)
{
    uint64_t f = 0, m = szmask(sz);
    r &= m;
    if (!r) f |= EFL_ZF;
    if ((r >> (sz * 8 - 1)) & 1) f |= EFL_SF;
    if (!__builtin_parity((unsigned)(r & 0xff))) f |= EFL_PF;
    return f;
}

static uint64_t do_shift(x86_cpu *c, int op, uint64_t v, unsigned cnt, int sz)
{
    int bits = sz * 8;
    uint64_t m = szmask(sz), sb = 1ULL << (bits - 1);
    v &= m;
    cnt &= sz == 8 ? 63 : 31;
    if (!cnt)
        return v;
    uint64_t fl = op < 4 ? x86_arith_flags(c) : 0, r; /* SHL/SHR/SAR reescrevem todos os flags */
    bool cf = fl & EFL_CF, of;
    switch (op) {
    case 0: {
        unsigned n = cnt % (unsigned)bits;
        r = n ? ((v << n) | (v >> (bits - n))) & m : v;
        cf = r & 1;
        of = ((r & sb) != 0) ^ cf;
        x86_set_arith_flags(c, (fl & ~(uint64_t)(EFL_CF | EFL_OF)) | (cf ? EFL_CF : 0) | (of ? EFL_OF : 0));
        return r;
    }
    case 1: {
        unsigned n = cnt % (unsigned)bits;
        r = n ? ((v >> n) | (v << (bits - n))) & m : v;
        cf = (r & sb) != 0;
        of = ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1;
        x86_set_arith_flags(c, (fl & ~(uint64_t)(EFL_CF | EFL_OF)) | (cf ? EFL_CF : 0) | (of ? EFL_OF : 0));
        return r;
    }
    case 2: {
        unsigned n = sz == 1 ? cnt % 9 : sz == 2 ? cnt % 17 : cnt;
        r = v;
        for (unsigned i = 0; i < n; i++) {
            bool nc = (r & sb) != 0;
            r = ((r << 1) | (cf ? 1 : 0)) & m;
            cf = nc;
        }
        of = ((r & sb) != 0) ^ cf;
        x86_set_arith_flags(c, (fl & ~(uint64_t)(EFL_CF | EFL_OF)) | (cf ? EFL_CF : 0) | (of ? EFL_OF : 0));
        return r;
    }
    case 3: {
        unsigned n = sz == 1 ? cnt % 9 : sz == 2 ? cnt % 17 : cnt;
        of = ((v & sb) != 0) ^ cf;
        r = v;
        for (unsigned i = 0; i < n; i++) {
            bool nc = r & 1;
            r = (r >> 1) | ((cf ? 1ULL : 0) << (bits - 1));
            cf = nc;
        }
        x86_set_arith_flags(c, (fl & ~(uint64_t)(EFL_CF | EFL_OF)) | (cf ? EFL_CF : 0) | (of ? EFL_OF : 0));
        return r;
    }
    case 4: case 6:
        cf = cnt <= (unsigned)bits ? (v >> (bits - cnt)) & 1 : 0;
        r = cnt >= (unsigned)bits ? 0 : (v << cnt) & m;
        of = ((r & sb) != 0) ^ cf;
        break;
    case 5:
        cf = cnt <= (unsigned)bits ? (v >> (cnt - 1)) & 1 : 0;
        r = cnt >= (unsigned)bits ? 0 : v >> cnt;
        of = (v & sb) != 0;
        break;
    default: {
        int64_t sv = (int64_t)sext_sz(v, sz);
        if (cnt >= (unsigned)bits) {
            cf = sv < 0;
            r = (uint64_t)(sv >> (bits - 1)) & m;
        } else {
            cf = (sv >> (cnt - 1)) & 1;
            r = (uint64_t)(sv >> cnt) & m;
        }
        of = false;
        break;
    }
    }
    set_lazy(c, CC_SZP, sz, r, 0, 0);
    c->cc_aux = (cf ? 1 : 0) | (of ? 2 : 0);
    return r;
}

static uint64_t shld(x86_cpu *c, uint64_t dst, uint64_t src, unsigned cnt, int sz, bool right)
{
    int bits = sz * 8;
    uint64_t m = szmask(sz);
    cnt &= sz == 8 ? 63 : 31;
    if (!cnt)
        return dst & m;
    dst &= m;
    src &= m;
    uint64_t r;
    bool cf;
    if (cnt > (unsigned)bits) { /* indefinido: trata modulo */
        cnt %= (unsigned)bits;
        if (!cnt) return dst;
    }
    if (!right) {
        r = cnt == (unsigned)bits ? src : ((dst << cnt) | (src >> (bits - cnt))) & m;
        cf = (dst >> (bits - cnt)) & 1;
    } else {
        r = cnt == (unsigned)bits ? src : ((dst >> cnt) | (src << (bits - cnt))) & m;
        cf = (dst >> (cnt - 1)) & 1;
    }
    bool of = (((r ^ dst) >> (bits - 1)) & 1);
    set_lazy(c, CC_SZP, sz, r, 0, 0);
    c->cc_aux = (cf ? 1 : 0) | (of ? 2 : 0);
    return r;
}

/* ------------------------------------------------------- utilitarios */

static inline int stack_osz(x86_cpu *c, x86_dec *d)
{
    if (c->code64)
        return d->opsize_prefix && !REXW(d) ? 2 : 8;
    return d->osz;
}

/* Desvios proximos (jmp/call/ret/jcc/loop): no modo de 64 bits a Intel fixa o tamanho em
 * 64 bits e ignora o prefixo 66 (a glibc usa "66 66 48 e8" no call __tls_get_addr do TLS). */
static inline int branch_osz(x86_cpu *c, int sz)
{
    return c->code64 ? 8 : sz;
}

static inline void jmp_to(x86_cpu *c, int bsz, uint64_t t)
{
    if (bsz == 2) t &= 0xffff;
    else if (bsz == 4) t = (uint32_t)t;
    c->rip = t;
}

static inline uint64_t amask(x86_dec *d) { return szmask(d->asz); }

static inline uint64_t get_areg(x86_cpu *c, x86_dec *d, int r) { return c->r[r] & amask(d); }

static inline void set_areg(x86_cpu *c, x86_dec *d, int r, uint64_t v)
{
    uint64_t m = amask(d);
    if (d->asz == 4)
        c->r[r] = (uint32_t)v;
    else
        c->r[r] = (c->r[r] & ~m) | (v & m);
}

static bool io_allowed(x86_cpu *c, uint16_t port, int size)
{
    if (!(c->cr0 & CR0_PE))
        return true;
    if (!(c->eflags & EFL_VM) && c->cpl <= (int)((c->eflags >> 12) & 3))
        return true;
    x86_seg *tr = &c->seg[S_TR];
    if (tr->limit < 0x67)
        return false;
    uint16_t base = (uint16_t)x86_sys_rd(c, tr->base + 0x66, 2);
    uint32_t byte = base + port / 8u;
    if (byte + 1 > tr->limit)
        return false;
    uint16_t bm = (uint16_t)x86_sys_rd(c, tr->base + byte, 2);
    uint16_t mask = (uint16_t)(((1u << size) - 1) << (port & 7));
    return !(bm & mask);
}

static uint64_t io_in(x86_cpu *c, uint16_t port, int size)
{
    if (!io_allowed(c, port, size))
        x86_gp(c, 0);
    return space_read(c->io, port, (unsigned)size);
}

static void io_out(x86_cpu *c, uint16_t port, uint64_t v, int size)
{
    if (!io_allowed(c, port, size))
        x86_gp(c, 0);
    space_write(c->io, port, v, (unsigned)size);
}

uint8_t *x86_host_ptr(x86_cpu *c, uint64_t lin, bool write);

/* ---------------------------------------------------------- strings */

static void string_op(x86_cpu *c, x86_dec *d, int op)
{
    int sz = (op & 1) ? d->osz : 1;
    bool rep = d->rep || d->repne;
    int seg = d->seg >= 0 ? d->seg : S_DS;
    int64_t step = (c->eflags & EFL_DF) ? -(int64_t)sz : sz;
    uint64_t cnt = rep ? get_areg(c, d, R_CX) : 1;
    int iter = 0;

    if (rep && !cnt)
        return;

    /* caminho rapido: REP MOVS/STOS para frente em RAM */
    if (rep && step > 0 && (op == 0xa4 || op == 0xa5 || op == 0xaa || op == 0xab)) {
        while (cnt) {
            uint64_t di = get_areg(c, d, R_DI);
            uint64_t dlin = x86_lin(c, S_ES, di);
            uint64_t bytes = cnt * (uint64_t)sz;
            uint64_t room = 0x1000 - (dlin & 0xfff);
            if (bytes > room) bytes = room;
            if (d->asz != 8) {
                uint64_t lim = szmask(d->asz) - di + 1;
                if (bytes > lim) bytes = lim;
            }
            bytes -= bytes % (uint64_t)sz;
            if (!bytes)
                break;
            uint8_t *dp = x86_host_ptr(c, dlin, true);
            if (!dp)
                break;
            if (op >= 0xaa) {
                uint64_t v = c->r[R_AX];
                if (sz == 1) memset(dp, (int)(v & 0xff), bytes);
                else for (uint64_t i = 0; i < bytes; i += (uint64_t)sz) st_le(dp + i, v, (unsigned)sz);
            } else {
                uint64_t si = get_areg(c, d, R_SI);
                uint64_t slin = x86_lin(c, seg, si);
                uint64_t sroom = 0x1000 - (slin & 0xfff);
                if (bytes > sroom) bytes = sroom - sroom % (uint64_t)sz;
                if (d->asz != 8) {
                    uint64_t lim = szmask(d->asz) - si + 1;
                    if (bytes > lim) bytes = lim - lim % (uint64_t)sz;
                }
                if (!bytes)
                    break;
                uint8_t *sp = x86_host_ptr(c, slin, false);
                if (!sp)
                    break;
                if (sp + bytes <= dp || dp + bytes <= sp) {
                    memcpy(dp, sp, bytes);
                } else {
                    /* sobreposicao: copia na ordem do hardware, elemento a elemento para frente */
                    for (uint64_t i = 0; i < bytes; i += (uint64_t)sz) st_le(dp + i, ld_le(sp + i, (unsigned)sz), (unsigned)sz);
                }
                set_areg(c, d, R_SI, si + bytes);
            }
            set_areg(c, d, R_DI, di + bytes);
            cnt -= bytes / (uint64_t)sz;
            set_areg(c, d, R_CX, cnt);
            if (++iter > 256 && cnt) { /* permite interrupcoes */
                c->rip = c->cur_rip;
                return;
            }
        }
        if (!cnt)
            return;
    }

    for (;;) {
        uint64_t si = get_areg(c, d, R_SI), di = get_areg(c, d, R_DI);
        bool stop = false;
        switch (op) {
        case 0xa4: case 0xa5: {
            uint64_t v = x86_rd_lin(c, x86_lin(c, seg, si), (unsigned)sz);
            x86_wr_lin(c, x86_lin(c, S_ES, di), v, (unsigned)sz);
            set_areg(c, d, R_SI, si + (uint64_t)step);
            set_areg(c, d, R_DI, di + (uint64_t)step);
            break;
        }
        case 0xa6: case 0xa7: {
            uint64_t a = x86_rd_lin(c, x86_lin(c, seg, si), (unsigned)sz);
            uint64_t b = x86_rd_lin(c, x86_lin(c, S_ES, di), (unsigned)sz);
            alu(c, 7, a, b, sz);
            set_areg(c, d, R_SI, si + (uint64_t)step);
            set_areg(c, d, R_DI, di + (uint64_t)step);
            stop = true;
            break;
        }
        case 0xaa: case 0xab:
            x86_wr_lin(c, x86_lin(c, S_ES, di), c->r[R_AX], (unsigned)sz);
            set_areg(c, d, R_DI, di + (uint64_t)step);
            break;
        case 0xac: case 0xad:
            x86_reg_write(c, R_AX, sz, x86_rd_lin(c, x86_lin(c, seg, si), (unsigned)sz), false);
            set_areg(c, d, R_SI, si + (uint64_t)step);
            break;
        case 0xae: case 0xaf: {
            uint64_t b = x86_rd_lin(c, x86_lin(c, S_ES, di), (unsigned)sz);
            alu(c, 7, c->r[R_AX], b, sz);
            set_areg(c, d, R_DI, di + (uint64_t)step);
            stop = true;
            break;
        }
        case 0x6c: case 0x6d: {
            uint64_t v = io_in(c, (uint16_t)c->r[R_DX], sz);
            x86_wr_lin(c, x86_lin(c, S_ES, di), v, (unsigned)sz);
            set_areg(c, d, R_DI, di + (uint64_t)step);
            break;
        }
        default: { /* 6e/6f OUTS */
            uint64_t v = x86_rd_lin(c, x86_lin(c, seg, si), (unsigned)sz);
            io_out(c, (uint16_t)c->r[R_DX], v, sz);
            set_areg(c, d, R_SI, si + (uint64_t)step);
            break;
        }
        }
        if (!rep)
            return;
        cnt--;
        set_areg(c, d, R_CX, cnt);
        if (!cnt)
            return;
        if (stop) {
            bool zf = x86_arith_flags(c) & EFL_ZF;
            if ((d->rep && !zf) || (d->repne && zf))
                return;
        }
        if (++iter >= 16384) {
            c->rip = c->cur_rip; /* continua depois (interrupcoes) */
            return;
        }
    }
}

/* -------------------------------------------------- multiplicacao/divisao */

/* MUL/IMUL/DIV/IDIV (F6/F7 /4-/7) com o operando ja lido */
static void muldiv(x86_cpu *c, int op, uint64_t v0, int sz)
{
    uint64_t m = szmask(sz);
    switch (op) {
    case 4: case 5: {
        uint64_t v = v0, a = c->r[R_AX] & m;
        bool sgn = op == 5;
        uint64_t lo, hi;
        bool ovf;
        if (sz == 8) {
            if (sgn) {
                __int128 p = (__int128)(int64_t)a * (int64_t)v;
                lo = (uint64_t)p;
                hi = (uint64_t)(p >> 64);
                ovf = (int64_t)hi != ((int64_t)lo >> 63);
            } else {
                unsigned __int128 p = (unsigned __int128)a * v;
                lo = (uint64_t)p;
                hi = (uint64_t)(p >> 64);
                ovf = hi != 0;
            }
        } else {
            int bits = sz * 8;
            if (sgn) {
                int64_t p = (int64_t)sext_sz(a, sz) * (int64_t)sext_sz(v, sz);
                lo = (uint64_t)p & m;
                hi = ((uint64_t)p >> bits) & m;
                ovf = p != (int64_t)sext_sz((uint64_t)p & m, sz);
            } else {
                uint64_t p = a * v;
                lo = p & m;
                hi = (p >> bits) & m;
                ovf = hi != 0;
            }
        }
        if (sz == 1) {
            c->r[R_AX] = (c->r[R_AX] & ~0xffffULL) | lo | (hi << 8);
        } else {
            x86_reg_write(c, R_AX, sz, lo, false);
            x86_reg_write(c, R_DX, sz, hi, false);
        }
        set_lazy(c, CC_SZP, sz, lo, 0, 0);
        c->cc_aux = ovf ? 3 : 0;
        return;
    }
    default: { /* DIV/IDIV */
        uint64_t v = v0;
        bool sgn = op == 7;
        if ((v & m) == 0)
            x86_exception(c, EXC_DE, 0, 0);
        if (sz == 8) {
            unsigned __int128 n = ((unsigned __int128)c->r[R_DX] << 64) | c->r[R_AX];
            if (sgn) {
                __int128 sn = (__int128)n, sd = (int64_t)v;
                if (sd == -1 && (unsigned __int128)sn == ((unsigned __int128)1 << 127)) x86_exception(c, EXC_DE, 0, 0);
                __int128 q = sn / sd, r = sn % sd;
                if (q > INT64_MAX || q < INT64_MIN) x86_exception(c, EXC_DE, 0, 0);
                c->r[R_AX] = (uint64_t)q;
                c->r[R_DX] = (uint64_t)r;
            } else {
                unsigned __int128 q = n / v, r = n % v;
                if (q >> 64) x86_exception(c, EXC_DE, 0, 0);
                c->r[R_AX] = (uint64_t)q;
                c->r[R_DX] = (uint64_t)r;
            }
            return;
        }
        int bits = sz * 8;
        uint64_t n = sz == 1 ? c->r[R_AX] & 0xffff : ((c->r[R_DX] & m) << bits) | (c->r[R_AX] & m);
        if (sgn) {
            int64_t sn = sz == 1 ? (int16_t)n : sz == 2 ? (int32_t)n : (int64_t)n;
            int64_t sd = (int64_t)sext_sz(v, sz);
            int64_t q = sn / sd, r = sn % sd;
            if (q != (int64_t)sext_sz((uint64_t)q & m, sz)) x86_exception(c, EXC_DE, 0, 0);
            if (sz == 1) c->r[R_AX] = (c->r[R_AX] & ~0xffffULL) | ((uint64_t)q & 0xff) | (((uint64_t)r & 0xff) << 8);
            else {
                x86_reg_write(c, R_AX, sz, (uint64_t)q, false);
                x86_reg_write(c, R_DX, sz, (uint64_t)r, false);
            }
        } else {
            uint64_t q = n / v, r = n % v;
            if (q > m) x86_exception(c, EXC_DE, 0, 0);
            if (sz == 1) c->r[R_AX] = (c->r[R_AX] & ~0xffffULL) | q | (r << 8);
            else {
                x86_reg_write(c, R_AX, sz, q, false);
                x86_reg_write(c, R_DX, sz, r, false);
            }
        }
        return;
    }
    }
}

static void group3(x86_cpu *c, x86_dec *d, int sz)
{
    uint64_t m = szmask(sz);
    switch (d->reg & 7) {
    case 0: case 1: {
        uint64_t imm = fetch_imm(c, sz == 8 ? 4 : sz);
        alu(c, 4, x86_rm_read(c, d, sz), imm, sz);
        return;
    }
    case 2:
        x86_rm_write(c, d, sz, ~x86_rm_read(c, d, sz) & m);
        return;
    case 3: {
        uint64_t v = x86_rm_read(c, d, sz);
        uint64_t r = alu(c, 5, 0, v, sz);
        x86_rm_write(c, d, sz, r);
        return;
    }
    default:
        muldiv(c, d->reg & 7, x86_rm_read(c, d, sz), sz);
        return;
    }
}

static uint64_t imul2(x86_cpu *c, uint64_t a, uint64_t b, int sz)
{
    uint64_t m = szmask(sz), r;
    bool ovf;
    if (sz == 8) {
        __int128 p = (__int128)(int64_t)a * (int64_t)b;
        r = (uint64_t)p;
        ovf = p != (__int128)(int64_t)r;
    } else {
        int64_t p = (int64_t)sext_sz(a, sz) * (int64_t)sext_sz(b, sz);
        r = (uint64_t)p & m;
        ovf = p != (int64_t)sext_sz(r, sz);
    }
    set_lazy(c, CC_SZP, sz, r, 0, 0);
    c->cc_aux = ovf ? 3 : 0;
    return r;
}

/* usados pelo JIT (semantica identica a do interpretador) */
uint64_t x86_jit_shift(x86_cpu *c, uint64_t opsz, uint64_t v, uint64_t cnt);
uint64_t x86_jit_imul2(x86_cpu *c, uint64_t a, uint64_t b, uint64_t sz);
uint64_t x86_jit_imul2(x86_cpu *c, uint64_t a, uint64_t b, uint64_t sz) { return imul2(c, a, b, (int)sz); }

/* ------------------------------------------------------------ bits */

static uint64_t stack_peek(x86_cpu *c, uint64_t off, int size)
{
    uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
    return x86_rd_lin(c, x86_lin(c, S_SS, (c->r[R_SP] + off) & m), (unsigned)size);
}

static void stack_add(x86_cpu *c, uint64_t n)
{
    uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
    c->r[R_SP] = (c->r[R_SP] & ~m) | ((c->r[R_SP] + n) & m);
}

/* Sonda a escrita de um destino em memoria antes de alterar estado que a propria
 * instrucao consome (CF em ADC/SBB/RCL/RCR, registradores em XADD/CMPXCHG). */
static inline void probe_rm(x86_cpu *c, x86_dec *d, int size)
{
    if (d->mem)
        x86_probe_write(c, x86_ea_lin(c, d), (unsigned)size);
}

static void bt_op(x86_cpu *c, x86_dec *d, int op, uint64_t bitoff, bool imm)
{
    int sz = d->osz, bits = sz * 8;
    uint64_t v, lin = 0;
    unsigned bit;
    if (d->mem) {
        int64_t off = imm ? 0 : (int64_t)sext_sz(bitoff, sz);
        int64_t word = off >> (sz == 8 ? 6 : sz == 4 ? 5 : 4);
        bit = (unsigned)(imm ? bitoff & (uint64_t)(bits - 1) : (uint64_t)off & (uint64_t)(bits - 1));
        lin = x86_ea_lin(c, d) + (uint64_t)(word * sz);
        if (!c->code64) lin = (uint32_t)lin;
        v = x86_rd_lin(c, lin, (unsigned)sz);
    } else {
        bit = (unsigned)(bitoff & (uint64_t)(bits - 1));
        v = x86_reg_read(c, d->rm, sz, REX(d));
    }
    bool cf = (v >> bit) & 1;
    uint64_t fl = x86_arith_flags(c);
    x86_set_arith_flags(c, (fl & ~(uint64_t)EFL_CF) | (cf ? EFL_CF : 0));
    if (op == 0)
        return;
    if (op == 1) v |= 1ULL << bit;
    else if (op == 2) v &= ~(1ULL << bit);
    else v ^= 1ULL << bit;
    if (d->mem) x86_wr_lin(c, lin, v, (unsigned)sz);
    else x86_reg_write(c, d->rm, sz, v, REX(d));
}

/* ------------------------------------------------------ flags / BCD */

static void popf(x86_cpu *c, uint64_t v, int sz)
{
    uint64_t mask = EFL_ARITH | EFL_TF | EFL_DF | EFL_NT | EFL_AC | EFL_ID;
    int iopl = (int)((c->eflags >> 12) & 3);
    if (!(c->cr0 & CR0_PE)) {
        mask |= EFL_IOPL | EFL_IF;
    } else if (c->eflags & EFL_VM) {
        if (iopl < 3)
            x86_gp(c, 0);
        mask |= EFL_IF;
    } else {
        if (c->cpl == 0) mask |= EFL_IOPL;
        if (c->cpl <= iopl) mask |= EFL_IF;
    }
    if (sz == 2) mask &= 0xffff;
    c->eflags = ((c->eflags & ~mask) | (v & mask)) | 2;
    c->cc_op = CC_NONE;
}

static void daa_das(x86_cpu *c, bool sub)
{
    uint64_t fl = x86_arith_flags(c);
    uint8_t al = (uint8_t)c->r[R_AX], old = al;
    bool cf = fl & EFL_CF, af = fl & EFL_AF, ncf = false;
    if ((al & 0xf) > 9 || af) {
        al = sub ? (uint8_t)(al - 6) : (uint8_t)(al + 6);
        ncf = cf || (sub ? old < 6 : old > 0xf9);
        af = true;
    } else {
        af = false;
    }
    if (old > 0x99 || cf) {
        al = sub ? (uint8_t)(al - 0x60) : (uint8_t)(al + 0x60);
        ncf = true;
    }
    x86_reg_write(c, R_AX, 1, al, false);
    x86_set_arith_flags(c, szp(al, 1) | (ncf ? EFL_CF : 0) | (af ? EFL_AF : 0));
}

static void aaa_aas(x86_cpu *c, bool sub)
{
    uint64_t fl = x86_arith_flags(c);
    uint16_t ax = (uint16_t)c->r[R_AX];
    bool f;
    if ((ax & 0xf) > 9 || (fl & EFL_AF)) {
        if (sub) {
            ax = (uint16_t)(ax - 6);
            ax = (uint16_t)((ax & 0xff) | ((((ax >> 8) - 1) & 0xff) << 8));
        } else {
            ax = (uint16_t)(ax + 0x106);
        }
        f = true;
    } else {
        f = false;
    }
    ax &= 0xff0f;
    c->r[R_AX] = (c->r[R_AX] & ~0xffffULL) | ax;
    x86_set_arith_flags(c, (fl & ~(uint64_t)(EFL_CF | EFL_AF)) | (f ? EFL_CF | EFL_AF : 0));
}

/* --------------------------------------------------- grupo 0F 00 / 0F 01 */

static void grp6(x86_cpu *c, x86_dec *d)
{
    if (!(c->cr0 & CR0_PE) || (c->eflags & EFL_VM))
        x86_ud(c);
    switch (d->reg & 7) {
    case 0: x86_rm_write(c, d, d->mem ? 2 : d->osz, c->seg[S_LDTR].sel); return;
    case 1: x86_rm_write(c, d, d->mem ? 2 : d->osz, c->seg[S_TR].sel); return;
    case 2: if (c->cpl) x86_gp(c, 0); x86_load_ldtr(c, (uint16_t)x86_rm_read(c, d, 2)); return;
    case 3: if (c->cpl) x86_gp(c, 0); x86_load_tr(c, (uint16_t)x86_rm_read(c, d, 2)); return;
    case 4: case 5: { /* VERR/VERW */
        uint16_t sel = (uint16_t)x86_rm_read(c, d, 2);
        bool ok = false;
        uint32_t lim = (sel & 4) ? c->seg[S_LDTR].limit : c->gdt_limit;
        if ((sel & ~3u) && (uint32_t)(sel | 7) <= lim) {
            uint64_t desc = x86_read_desc(c, sel, NULL);
            unsigned type = (desc >> 40) & 0x1f;
            if (type & 0x10) {
                if ((d->reg & 7) == 4) ok = !(type & 8) || (type & 2);
                else ok = !(type & 8) && (type & 2);
            }
        }
        uint64_t fl = x86_arith_flags(c);
        x86_set_arith_flags(c, (fl & ~(uint64_t)EFL_ZF) | (ok ? EFL_ZF : 0));
        return;
    }
    default: x86_ud(c);
    }
}

static void grp7(x86_cpu *c, x86_dec *d)
{
    unsigned r = d->reg & 7;
    if (d->mod == 3) {
        switch (d->modrm) {
        case 0xc8: return;                 /* MONITOR */
        case 0xc9: c->halted = !c->intr_line; return; /* MWAIT */
        case 0xd0:                          /* XGETBV */
            x86_ud(c);
        case 0xf8:                          /* SWAPGS */
            if (!c->code64 || c->cpl) x86_ud(c);
            {
                uint64_t t = c->seg[S_GS].base;
                c->seg[S_GS].base = c->kernel_gs_base;
                c->kernel_gs_base = t;
            }
            return;
        case 0xf9: {                        /* RDTSCP */
            if ((c->cr4 & 4) && c->cpl) x86_gp(c, 0);
            uint64_t t = x86_tsc(c);
            c->r[R_AX] = (uint32_t)t;
            c->r[R_DX] = (uint32_t)(t >> 32);
            c->r[R_CX] = (uint32_t)c->tsc_aux;
            return;
        }
        default: break;
        }
        if (r == 4) { x86_rm_write(c, d, d->osz, c->cr0 & 0xffff); return; }
        if (r == 6) {
            if (c->cpl) x86_gp(c, 0);
            uint64_t v = x86_rm_read(c, d, 2);
            x86_write_cr(c, 0, (c->cr0 & ~0xeULL) | (v & 0xf) | (c->cr0 & 1));
            return;
        }
        x86_ud(c);
    }
    uint64_t lin = x86_ea_lin(c, d);
    switch (r) {
    case 0: case 1: {
        uint32_t lim = r ? c->idt_limit : c->gdt_limit;
        uint64_t base = r ? c->idt_base : c->gdt_base;
        x86_wr_lin(c, lin, lim, 2);
        if (c->code64) x86_wr_lin(c, lin + 2, base, 8);
        else x86_wr_lin(c, lin + 2, d->osz == 2 ? base & 0xffffff : (uint32_t)base, 4);
        return;
    }
    case 2: case 3: {
        if (c->cpl) x86_gp(c, 0);
        uint32_t lim = (uint32_t)x86_rd_lin(c, lin, 2);
        uint64_t base = c->code64 ? x86_rd_lin(c, lin + 2, 8) : x86_rd_lin(c, lin + 2, 4);
        if (!c->code64 && d->osz == 2) base &= 0xffffff;
        if (r == 2) { c->gdt_base = base; c->gdt_limit = lim; }
        else { c->idt_base = base; c->idt_limit = lim; }
        return;
    }
    case 4: x86_wr_lin(c, lin, c->cr0 & 0xffff, 2); return;
    case 6: {
        if (c->cpl) x86_gp(c, 0);
        uint64_t v = x86_rd_lin(c, lin, 2);
        x86_write_cr(c, 0, (c->cr0 & ~0xeULL) | (v & 0xf) | (c->cr0 & 1));
        return;
    }
    case 7: /* INVLPG */
        if (c->cpl) x86_gp(c, 0);
        x86_tlb_flush_page(c, lin);
        return;
    default: x86_ud(c);
    }
}

static void lar_lsl(x86_cpu *c, x86_dec *d, bool lsl)
{
    uint16_t sel = (uint16_t)x86_rm_read(c, d, 2);
    bool ok = false;
    uint64_t v = 0;
    uint32_t lim = (sel & 4) ? c->seg[S_LDTR].limit : c->gdt_limit;
    if ((sel & ~3u) && (uint32_t)(sel | 7) <= lim) {
        uint64_t desc = x86_read_desc(c, sel, NULL);
        unsigned dpl = (desc >> 45) & 3;
        unsigned type = (desc >> 40) & 0x1f;
        bool conforming = (type & 0x1c) == 0x1c;
        if (conforming || ((int)dpl >= c->cpl && dpl >= (sel & 3u))) {
            ok = true;
            if (lsl) {
                x86_seg s;
                x86_desc_to_seg(&s, sel, desc);
                v = s.limit;
            } else {
                v = desc >> 32 & 0x00f0ff00;
            }
        }
    }
    uint64_t fl = x86_arith_flags(c);
    x86_set_arith_flags(c, (fl & ~(uint64_t)EFL_ZF) | (ok ? EFL_ZF : 0));
    if (ok)
        SREG(d, d->osz, v);
}

/* ---------------------------------------------------------- 0F xx */

static void exec_0f(x86_cpu *c, x86_dec *d)
{
    int op = x86_fetch8(c);
    int sz = d->osz;
    switch (op) {
    case 0x00: decode_modrm(c, d); grp6(c, d); return;
    case 0x01: decode_modrm(c, d); grp7(c, d); return;
    case 0x02: case 0x03: decode_modrm(c, d); lar_lsl(c, d, op == 3); return;
    case 0x05: x86_syscall(c); return;
    case 0x06: if (c->cpl) x86_gp(c, 0); c->cr0 &= ~(uint64_t)CR0_TS; return;
    case 0x07: x86_sysret(c, REXW(d)); return;
    case 0x08: case 0x09: if (c->cpl) x86_gp(c, 0); return;
    case 0x0b: case 0xb9: case 0xff: x86_ud(c);
    case 0x0d: decode_modrm(c, d); return;
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        decode_modrm(c, d);
        return;
    case 0x20: case 0x22: {
        decode_modrm(c, d);
        if (c->cpl) x86_gp(c, 0);
        int crn = d->reg;
        int rsz = c->code64 ? 8 : 4;
        if (op == 0x20) x86_reg_write(c, d->rm, rsz, x86_read_cr(c, crn), true);
        else x86_write_cr(c, crn, x86_reg_read(c, d->rm, rsz, true));
        return;
    }
    case 0x21: case 0x23: {
        decode_modrm(c, d);
        if (c->cpl) x86_gp(c, 0);
        int drn = d->reg & 7;
        int rsz = c->code64 ? 8 : 4;
        if (op == 0x21) x86_reg_write(c, d->rm, rsz, c->dr[drn], true);
        else c->dr[drn] = x86_reg_read(c, d->rm, rsz, true);
        return;
    }
    case 0x30: if (c->cpl) x86_gp(c, 0); x86_wrmsr(c); return;
    case 0x31: {
        if ((c->cr4 & 4) && c->cpl) x86_gp(c, 0);
        uint64_t t = x86_tsc(c);
        c->r[R_AX] = (uint32_t)t;
        c->r[R_DX] = (uint32_t)(t >> 32);
        return;
    }
    case 0x32: if (c->cpl) x86_gp(c, 0); x86_rdmsr(c); return;
    case 0x33: x86_gp(c, 0);
    case 0x34: x86_sysenter(c); return;
    case 0x35: x86_sysexit(c, REXW(d)); return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
        decode_modrm(c, d);
        uint64_t v = x86_rm_read(c, d, sz);
        if (x86_cond(c, op & 15)) SREG(d, sz, v);
        else if (sz == 4) SREG(d, 4, GREG(d, 4)); /* zera a parte alta em 64 bits */
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f: {
        int bsz = branch_osz(c, sz);
        uint64_t disp = bsz == 2 ? (uint64_t)(int64_t)(int16_t)fetch16(c) : (uint64_t)(int64_t)(int32_t)x86_fetch32(c);
        if (x86_cond(c, op & 15)) jmp_to(c, bsz, c->rip + disp);
        return;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:
        decode_modrm(c, d);
        x86_rm_write(c, d, 1, x86_cond(c, op & 15) ? 1 : 0);
        return;
    case 0xa0: x86_push(c, c->seg[S_FS].sel, stack_osz(c, d)); return;
    case 0xa1: { /* o seletor e carregado antes de mexer no SP (falha sem efeitos) */
        int n = stack_osz(c, d);
        x86_load_seg(c, S_FS, (uint16_t)stack_peek(c, 0, n));
        stack_add(c, (uint64_t)n);
        return;
    }
    case 0xa8: x86_push(c, c->seg[S_GS].sel, stack_osz(c, d)); return;
    case 0xa9: {
        int n = stack_osz(c, d);
        x86_load_seg(c, S_GS, (uint16_t)stack_peek(c, 0, n));
        stack_add(c, (uint64_t)n);
        return;
    }
    case 0xa2: x86_cpuid(c); return;
    case 0xa3: decode_modrm(c, d); bt_op(c, d, 0, GREG(d, sz), false); return;
    case 0xab: decode_modrm(c, d); bt_op(c, d, 1, GREG(d, sz), false); return;
    case 0xb3: decode_modrm(c, d); bt_op(c, d, 2, GREG(d, sz), false); return;
    case 0xbb: decode_modrm(c, d); bt_op(c, d, 3, GREG(d, sz), false); return;
    case 0xba: {
        decode_modrm(c, d);
        uint64_t imm = x86_fetch8(c);
        if ((d->reg & 7) < 4) x86_ud(c);
        bt_op(c, d, (d->reg & 7) - 4, imm, true);
        return;
    }
    case 0xa4: case 0xa5: case 0xac: case 0xad: {
        decode_modrm(c, d);
        unsigned cnt = (op & 1) ? (unsigned)c->r[R_CX] : x86_fetch8(c);
        uint64_t v = x86_rm_read(c, d, sz);
        uint64_t r = shld(c, v, GREG(d, sz), cnt, sz, op >= 0xac);
        if (cnt & (sz == 8 ? 63 : 31)) x86_rm_write(c, d, sz, r);
        return;
    }
    case 0xae: {
        decode_modrm(c, d);
        unsigned r = d->reg & 7;
        if (d->mod == 3) {
            if (r >= 5) return; /* LFENCE/MFENCE/SFENCE */
            x86_ud(c);
        }
        uint64_t lin = x86_ea_lin(c, d);
        switch (r) {
        case 0: sse_fxsave(c, lin, REXW(d)); return;
        case 1: sse_fxrstor(c, lin, REXW(d)); return;
        case 2: {
            uint32_t v = (uint32_t)x86_rd_lin(c, lin, 4);
            if (v & 0xffff0000u) x86_gp(c, 0);
            c->mxcsr = v;
            return;
        }
        case 3: x86_wr_lin(c, lin, c->mxcsr, 4); return;
        case 7: return; /* CLFLUSH */
        default: x86_ud(c);
        }
    }
    case 0xaf: {
        decode_modrm(c, d);
        uint64_t v = x86_rm_read(c, d, sz);
        SREG(d, sz, imul2(c, GREG(d, sz), v, sz));
        return;
    }
    case 0xb0: case 0xb1: {
        decode_modrm(c, d);
        int s = op == 0xb0 ? 1 : sz;
        uint64_t dst = x86_rm_read(c, d, s), acc = c->r[R_AX] & szmask(s);
        alu(c, 7, acc, dst, s);
        if (acc == dst) x86_rm_write(c, d, s, GREG(d, s));
        else {
            /* a escrita (que pode falhar) vem antes da atualizacao do acumulador */
            if (d->mem) x86_rm_write(c, d, s, dst);
            x86_reg_write(c, R_AX, s, dst, false);
        }
        return;
    }
    case 0xb2: case 0xb4: case 0xb5: {
        decode_modrm(c, d);
        if (!d->mem) x86_ud(c);
        uint64_t lin = x86_ea_lin(c, d);
        uint64_t off = x86_rd_lin(c, lin, (unsigned)sz);
        uint16_t sel = (uint16_t)x86_rd_lin(c, lin + (uint64_t)sz, 2);
        x86_load_seg(c, op == 0xb2 ? S_SS : op == 0xb4 ? S_FS : S_GS, sel);
        SREG(d, sz, off);
        return;
    }
    case 0xb6: case 0xb7: case 0xbe: case 0xbf: {
        decode_modrm(c, d);
        int s = (op & 1) ? 2 : 1;
        uint64_t v = x86_rm_read(c, d, s);
        if (op >= 0xbe) v = sext_sz(v, s);
        SREG(d, sz, v & szmask(sz));
        return;
    }
    case 0xb8: {
        if (!d->rep) x86_ud(c);
        decode_modrm(c, d);
        uint64_t v = x86_rm_read(c, d, sz);
        uint64_t r = (uint64_t)__builtin_popcountll(v);
        SREG(d, sz, r);
        x86_set_arith_flags(c, v ? 0 : EFL_ZF);
        return;
    }
    case 0xbc: case 0xbd: {
        decode_modrm(c, d);
        uint64_t v = x86_rm_read(c, d, sz);
        uint64_t fl = x86_arith_flags(c) & ~(uint64_t)EFL_ZF;
        if (!v) {
            x86_set_arith_flags(c, fl | EFL_ZF);
            return;
        }
        uint64_t r = op == 0xbc ? (uint64_t)__builtin_ctzll(v) : (uint64_t)(63 - __builtin_clzll(v));
        x86_set_arith_flags(c, fl);
        SREG(d, sz, r);
        return;
    }
    case 0xc0: case 0xc1: {
        decode_modrm(c, d);
        int s = op == 0xc0 ? 1 : sz;
        uint64_t dst = x86_rm_read(c, d, s), src = GREG(d, s);
        uint64_t r = alu(c, 0, dst, src, s);
        if (d->mem) { /* grava a memoria antes de alterar o registrador fonte */
            x86_rm_write(c, d, s, r);
            SREG(d, s, dst);
        } else {
            SREG(d, s, dst);
            x86_rm_write(c, d, s, r);
        }
        return;
    }
    case 0xc7: {
        decode_modrm(c, d);
        if (((d->reg & 7) == 6 || (d->reg & 7) == 7) && !d->mem) {
            /* RDRAND / RDSEED: numero aleatorio do host, CF=1 (sempre disponivel) */
            x86_rm_write(c, d, sz, x86_host_random());
            x86_set_arith_flags(c, (x86_arith_flags(c) & ~(uint64_t)EFL_ARITH) | EFL_CF);
            return;
        }
        if ((d->reg & 7) != 1 || !d->mem) x86_ud(c);
        uint64_t lin = x86_ea_lin(c, d);
        uint64_t fl = x86_arith_flags(c) & ~(uint64_t)EFL_ZF;
        if (REXW(d)) {
            if (lin & 15)
                x86_gp(c, 0); /* CMPXCHG16B exige alinhamento de 16 bytes */
            x86_probe_write(c, lin, 16);
            uint64_t lo = x86_rd_lin(c, lin, 8), hi = x86_rd_lin(c, lin + 8, 8);
            if (lo == c->r[R_AX] && hi == c->r[R_DX]) {
                x86_wr_lin(c, lin, c->r[R_BX], 8);
                x86_wr_lin(c, lin + 8, c->r[R_CX], 8);
                fl |= EFL_ZF;
            } else {
                c->r[R_AX] = lo;
                c->r[R_DX] = hi;
            }
        } else {
            uint64_t v = x86_rd_lin(c, lin, 8);
            uint64_t cmp = ((c->r[R_DX] & 0xffffffffULL) << 32) | (uint32_t)c->r[R_AX];
            if (v == cmp) {
                x86_wr_lin(c, lin, ((c->r[R_CX] & 0xffffffffULL) << 32) | (uint32_t)c->r[R_BX], 8);
                fl |= EFL_ZF;
            } else {
                c->r[R_AX] = (uint32_t)v;
                c->r[R_DX] = (uint32_t)(v >> 32);
            }
        }
        x86_set_arith_flags(c, fl);
        return;
    }
    case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: {
        int r = (op & 7) | ((d->rex & 1) << 3);
        if (sz == 8) c->r[r] = bswap64(c->r[r]);
        else if (sz == 4) c->r[r] = bswap32((uint32_t)c->r[r]);
        else c->r[r] &= ~0xffffULL; /* indefinido para 16 bits */
        return;
    }
    case 0x38: case 0x3a: {
        int op2 = x86_fetch8(c);
        if (op == 0x38 && (op2 == 0xf0 || op2 == 0xf1) && d->repne) { /* CRC32 (CRC-32C) */
            decode_modrm(c, d);
            int n = op2 == 0xf0 ? 1 : sz;
            uint64_t v = x86_rm_read(c, d, n);
            uint32_t crc = (uint32_t)x86_reg_read(c, d->reg, 4, false);
            for (int i = 0; i < n * 8; i++) {
                crc ^= (uint32_t)(v >> i) & 1;
                crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
            }
            x86_reg_write(c, d->reg, REXW(d) ? 8 : 4, crc, true);
            return;
        }
        int full = (op << 8) | op2 | 0x0f0000;
        if (!sse_exec(c, d, full)) x86_ud(c);
        return;
    }
    default:
        if (!sse_exec(c, d, 0x0f00 | op))
            x86_ud(c);
        return;
    }
}

/* SSE usa a decodificacao de ModRM daqui */
void x86_decode_modrm(x86_cpu *c, x86_dec *d) { decode_modrm(c, d); }

/* ------------------------------------------------------------ 1 byte */

void x86_exec_one(x86_cpu *c)
{
    x86_dec dd;
    x86_dec *d = &dd;
    x86_fetch_begin(c);
    d->seg = -1;
    d->rep = d->repne = d->lock = d->opsize_prefix = false;
    d->rex = 0;
    d->mem = false;
    d->riprel = false;
    bool addr_prefix = false;
    int b;
    for (;;) {
        b = x86_fetch8(c);
        switch (b) {
        case 0x66: d->opsize_prefix = true; d->rex = 0; continue;
        case 0x67: addr_prefix = true; d->rex = 0; continue;
        case 0x26: d->seg = S_ES; d->rex = 0; continue;
        case 0x2e: d->seg = S_CS; d->rex = 0; continue;
        case 0x36: d->seg = S_SS; d->rex = 0; continue;
        case 0x3e: d->seg = S_DS; d->rex = 0; continue;
        case 0x64: d->seg = S_FS; d->rex = 0; continue;
        case 0x65: d->seg = S_GS; d->rex = 0; continue;
        case 0xf0: d->lock = true; d->rex = 0; continue;
        case 0xf2: d->repne = true; d->rep = false; d->rex = 0; continue;
        case 0xf3: d->rep = true; d->repne = false; d->rex = 0; continue;
        default: break;
        }
        if (c->code64 && (b & 0xf0) == 0x40) {
            d->rex = (uint8_t)b;
            continue;
        }
        break;
    }
    if (c->code64) {
        d->osz = REXW(d) ? 8 : d->opsize_prefix ? 2 : 4;
        d->asz = addr_prefix ? 4 : 8;
        if (d->seg >= 0 && d->seg != S_FS && d->seg != S_GS)
            d->seg = -1;
    } else {
        d->osz = (c->csz == 4) != d->opsize_prefix ? 4 : 2;
        d->asz = (c->csz == 4) != addr_prefix ? 4 : 2;
    }
    d->op = b;
    int sz = d->osz;

    /* ALU classico 00-3F */
    if (b < 0x40 && (b & 7) < 6) {
        int op = b >> 3;
        switch (b & 7) {
        case 0: case 1: {
            int s = (b & 1) ? sz : 1;
            decode_modrm(c, d);
            uint64_t a = x86_rm_read(c, d, s), v = GREG(d, s);
            if (op == 2 || op == 3) probe_rm(c, d, s);
            uint64_t r = alu(c, op, a, v, s);
            if (op != 7) x86_rm_write(c, d, s, r);
            return;
        }
        case 2: case 3: {
            int s = (b & 1) ? sz : 1;
            decode_modrm(c, d);
            uint64_t v = x86_rm_read(c, d, s);
            uint64_t r = alu(c, op, GREG(d, s), v, s);
            if (op != 7) SREG(d, s, r);
            return;
        }
        case 4: {
            uint64_t imm = x86_fetch8(c);
            uint64_t r = alu(c, op, c->r[R_AX], imm, 1);
            if (op != 7) x86_reg_write(c, R_AX, 1, r, false);
            return;
        }
        default: {
            uint64_t imm = fetch_imm(c, sz == 8 ? 4 : sz);
            uint64_t r = alu(c, op, c->r[R_AX], imm, sz);
            if (op != 7) x86_reg_write(c, R_AX, sz, r, false);
            return;
        }
        }
    }

    switch (b) {
    case 0x06: case 0x0e: case 0x16: case 0x1e:
        if (c->code64) x86_ud(c);
        x86_push(c, c->seg[b >> 3].sel, sz);
        return;
    case 0x07: case 0x17: case 0x1f: {
        if (c->code64) x86_ud(c);
        x86_load_seg(c, b >> 3, (uint16_t)stack_peek(c, 0, sz));
        stack_add(c, (uint64_t)sz);
        if (b == 0x17) c->irq_inhibit = 1;
        return;
    }
    case 0x0f: exec_0f(c, d); return;
    case 0x27: if (c->code64) x86_ud(c); daa_das(c, false); return;
    case 0x2f: if (c->code64) x86_ud(c); daa_das(c, true); return;
    case 0x37: if (c->code64) x86_ud(c); aaa_aas(c, false); return;
    case 0x3f: if (c->code64) x86_ud(c); aaa_aas(c, true); return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
        int r = b & 7;
        uint64_t v = c->r[r] & szmask(sz), res, cf = x86_cf(c) ? 1 : 0;
        if (b < 0x48) {
            res = (v + 1) & szmask(sz);
            set_lazy(c, CC_INC, sz, res, v, 1);
        } else {
            res = (v - 1) & szmask(sz);
            set_lazy(c, CC_DEC, sz, res, v, 1);
        }
        c->cc_aux = cf;
        x86_reg_write(c, r, sz, res, false);
        return;
    }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        int s = stack_osz(c, d);
        x86_push(c, c->r[(b & 7) | ((d->rex & 1) << 3)] & szmask(s), s);
        return;
    }
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        int s = stack_osz(c, d);
        uint64_t v = x86_pop(c, s);
        x86_reg_write(c, (b & 7) | ((d->rex & 1) << 3), s, v, false);
        return;
    }
    case 0x60: {
        if (c->code64) x86_ud(c);
        uint64_t sp = c->r[R_SP];
        for (int i = 0; i < 8; i++)
            x86_push(c, i == R_SP ? sp : c->r[i], sz);
        return;
    }
    case 0x61: {
        if (c->code64) x86_ud(c);
        uint64_t v[8];
        for (int i = 7; i >= 0; i--) /* le tudo antes: uma falha na pilha nao altera nada */
            v[i] = stack_peek(c, (uint64_t)(7 - i) * (uint64_t)sz, sz);
        for (int i = 7; i >= 0; i--)
            if (i != R_SP) x86_reg_write(c, i, sz, v[i], false);
        stack_add(c, 8 * (uint64_t)sz);
        return;
    }
    case 0x62: { /* BOUND */
        if (c->code64) x86_ud(c);
        decode_modrm(c, d);
        if (!d->mem) x86_ud(c);
        uint64_t lin = x86_ea_lin(c, d);
        int64_t lo = (int64_t)sext_sz(x86_rd_lin(c, lin, (unsigned)sz), sz);
        int64_t hi = (int64_t)sext_sz(x86_rd_lin(c, lin + (uint64_t)sz, (unsigned)sz), sz);
        int64_t v = (int64_t)sext_sz(GREG(d, sz), sz);
        if (v < lo || v > hi) x86_exception(c, EXC_BR, 0, 0);
        return;
    }
    case 0x63:
        decode_modrm(c, d);
        if (c->code64) { /* MOVSXD */
            uint64_t v = x86_rm_read(c, d, 4);
            SREG(d, sz, sz == 8 ? sext_sz(v, 4) : v);
        } else { /* ARPL */
            uint64_t dst = x86_rm_read(c, d, 2), src = GREG(d, 2);
            uint64_t fl = x86_arith_flags(c) & ~(uint64_t)EFL_ZF;
            if ((dst & 3) < (src & 3)) {
                x86_rm_write(c, d, 2, (dst & ~3ULL) | (src & 3));
                fl |= EFL_ZF;
            }
            x86_set_arith_flags(c, fl);
        }
        return;
    case 0x68: x86_push(c, fetch_imm(c, stack_osz(c, d) == 2 ? 2 : 4), stack_osz(c, d)); return;
    case 0x6a: x86_push(c, fetch_imm(c, 1), stack_osz(c, d)); return;
    case 0x69: case 0x6b: {
        decode_modrm(c, d);
        uint64_t imm = fetch_imm(c, b == 0x6b ? 1 : sz == 8 ? 4 : sz);
        uint64_t v = x86_rm_read(c, d, sz);
        SREG(d, sz, imul2(c, v, imm, sz));
        return;
    }
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:
    case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        string_op(c, d, b);
        return;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
        uint64_t disp = (uint64_t)(int64_t)(int8_t)x86_fetch8(c);
        if (x86_cond(c, b & 15)) jmp_to(c, branch_osz(c, sz), c->rip + disp);
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        if (b == 0x82 && c->code64) x86_ud(c);
        int s = (b & 1) ? sz : 1;
        decode_modrm(c, d);
        uint64_t imm = fetch_imm(c, b == 0x81 ? (sz == 8 ? 4 : sz) : 1);
        uint64_t a = x86_rm_read(c, d, s);
        if ((d->reg & 7) == 2 || (d->reg & 7) == 3) probe_rm(c, d, s);
        uint64_t r = alu(c, d->reg & 7, a, imm, s);
        if ((d->reg & 7) != 7) x86_rm_write(c, d, s, r);
        return;
    }
    case 0x84: case 0x85: {
        int s = (b & 1) ? sz : 1;
        decode_modrm(c, d);
        alu(c, 4, x86_rm_read(c, d, s), GREG(d, s), s);
        return;
    }
    case 0x86: case 0x87: {
        int s = (b & 1) ? sz : 1;
        decode_modrm(c, d);
        uint64_t a = x86_rm_read(c, d, s), v = GREG(d, s);
        x86_rm_write(c, d, s, v);
        SREG(d, s, a);
        return;
    }
    case 0x88: case 0x89: {
        int s = (b & 1) ? sz : 1;
        decode_modrm(c, d);
        x86_rm_write(c, d, s, GREG(d, s));
        return;
    }
    case 0x8a: case 0x8b: {
        int s = (b & 1) ? sz : 1;
        decode_modrm(c, d);
        SREG(d, s, x86_rm_read(c, d, s));
        return;
    }
    case 0x8c: {
        decode_modrm(c, d);
        if ((d->reg & 7) > 5) x86_ud(c);
        uint16_t sel = c->seg[d->reg & 7].sel;
        x86_rm_write(c, d, d->mem ? 2 : sz, sel);
        return;
    }
    case 0x8d:
        decode_modrm(c, d);
        if (!d->mem) x86_ud(c);
        SREG(d, sz, ea_off(c, d) & szmask(sz));
        return;
    case 0x8e: {
        decode_modrm(c, d);
        int s = d->reg & 7;
        if (s == S_CS || s > 5) x86_ud(c);
        x86_load_seg(c, s, (uint16_t)x86_rm_read(c, d, 2));
        if (s == S_SS) c->irq_inhibit = 1;
        return;
    }
    case 0x8f: {
        /* o endereco efetivo e calculado apos o incremento de (E/R)SP */
        uint64_t modrm_rip = c->rip;
        const uint8_t *modrm_ptr = c->ip_ptr;
        unsigned modrm_left = c->ip_left;
        decode_modrm(c, d);
        int s = stack_osz(c, d);
        if (!d->mem) {
            uint64_t v = x86_pop(c, s);
            x86_rm_write(c, d, s, v);
            return;
        }
        /* RSP so muda depois da escrita, para a instrucao poder ser reiniciada */
        uint64_t v = stack_peek(c, 0, s);
        uint64_t old_sp = c->r[R_SP];
        stack_add(c, (uint64_t)s);
        uint64_t end_rip = c->rip;
        c->rip = modrm_rip;
        c->ip_ptr = modrm_ptr;
        c->ip_left = modrm_left;
        decode_modrm(c, d);
        c->rip = end_rip;
        uint64_t lin = x86_ea_lin(c, d);
        uint64_t new_sp = c->r[R_SP];
        c->r[R_SP] = old_sp;
        x86_wr_lin(c, lin, v, (unsigned)s);
        c->r[R_SP] = new_sp;
        return;
    }
    case 0x90:
        if (d->rex & 1) {
            uint64_t t = c->r[8];
            x86_reg_write(c, 8, sz, c->r[R_AX], false);
            x86_reg_write(c, R_AX, sz, t, false);
        }
        return; /* NOP / PAUSE */
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        int r = (b & 7) | ((d->rex & 1) << 3);
        uint64_t t = c->r[r];
        x86_reg_write(c, r, sz, c->r[R_AX], false);
        x86_reg_write(c, R_AX, sz, t, false);
        return;
    }
    case 0x98:
        if (sz == 8) c->r[R_AX] = sext_sz(c->r[R_AX], 4);
        else if (sz == 4) c->r[R_AX] = (uint32_t)sext_sz(c->r[R_AX], 2);
        else x86_reg_write(c, R_AX, 2, sext_sz(c->r[R_AX], 1), false);
        return;
    case 0x99: {
        uint64_t sign = (c->r[R_AX] >> (sz * 8 - 1)) & 1;
        x86_reg_write(c, R_DX, sz, sign ? ~0ULL : 0, false);
        return;
    }
    case 0x9a: {
        if (c->code64) x86_ud(c);
        uint64_t off = sz == 2 ? fetch16(c) : x86_fetch32(c);
        uint16_t sel = fetch16(c);
        x86_far_jump(c, sel, off, sz, true);
        return;
    }
    case 0x9b: return; /* WAIT */
    case 0x9c: {
        if ((c->eflags & EFL_VM) && ((c->eflags >> 12) & 3) < 3) x86_gp(c, 0);
        int s = stack_osz(c, d);
        x86_push(c, x86_get_flags(c) & ~(uint64_t)(EFL_VM | EFL_RF), s);
        return;
    }
    case 0x9d: {
        /* em V86 com IOPL < 3 o #GP acontece antes de desempilhar (sem efeitos colaterais:
         * o monitor V86 do NT emula a instrucao e desempilha ele mesmo) */
        if ((c->eflags & EFL_VM) && ((c->eflags >> 12) & 3) < 3) x86_gp(c, 0);
        int s = stack_osz(c, d);
        popf(c, x86_pop(c, s), s);
        return;
    }
    case 0x9e: {
        uint64_t ah = (c->r[R_AX] >> 8) & 0xff;
        uint64_t fl = x86_arith_flags(c) & EFL_OF;
        x86_set_arith_flags(c, fl | (ah & (EFL_SF | EFL_ZF | EFL_AF | EFL_PF | EFL_CF)));
        return;
    }
    case 0x9f: {
        uint64_t fl = x86_get_flags(c) & 0xd7;
        c->r[R_AX] = (c->r[R_AX] & ~0xff00ULL) | ((fl | 2) << 8);
        return;
    }
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
        uint64_t off = d->asz == 8 ? fetch64(c) : d->asz == 4 ? x86_fetch32(c) : fetch16(c);
        int s = (b & 1) ? sz : 1;
        uint64_t lin = x86_lin(c, d->seg >= 0 ? d->seg : S_DS, off);
        if (b < 0xa2) x86_reg_write(c, R_AX, s, x86_rd_lin(c, lin, (unsigned)s), false);
        else x86_wr_lin(c, lin, c->r[R_AX], (unsigned)s);
        return;
    }
    case 0xa8: alu(c, 4, c->r[R_AX], x86_fetch8(c), 1); return;
    case 0xa9: alu(c, 4, c->r[R_AX], fetch_imm(c, sz == 8 ? 4 : sz), sz); return;
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        x86_reg_write(c, (b & 7) | ((d->rex & 1) << 3), 1, x86_fetch8(c), REX(d));
        return;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: {
        uint64_t imm = sz == 8 ? fetch64(c) : sz == 4 ? x86_fetch32(c) : fetch16(c);
        x86_reg_write(c, (b & 7) | ((d->rex & 1) << 3), sz, imm, false);
        return;
    }
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        int s = (b & 1) ? sz : 1;
        decode_modrm(c, d);
        unsigned cnt = b < 0xd0 ? x86_fetch8(c) : b < 0xd2 ? 1 : (unsigned)(c->r[R_CX] & 0xff);
        uint64_t v = x86_rm_read(c, d, s);
        if ((d->reg & 7) == 2 || (d->reg & 7) == 3) probe_rm(c, d, s);
        uint64_t r = do_shift(c, d->reg & 7, v, cnt, s);
        x86_rm_write(c, d, s, r);
        return;
    }
    case 0xc2: case 0xc3: {
        int s = branch_osz(c, stack_osz(c, d));
        uint16_t imm = b == 0xc2 ? fetch16(c) : 0;
        uint64_t t = x86_pop(c, s);
        if (imm) {
            uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
            c->r[R_SP] = (c->r[R_SP] & ~m) | ((c->r[R_SP] + imm) & m);
        }
        jmp_to(c, s, t);
        return;
    }
    case 0xc4: case 0xc5: {
        if (c->code64) x86_ud(c); /* VEX */
        decode_modrm(c, d);
        if (!d->mem) x86_ud(c);
        uint64_t lin = x86_ea_lin(c, d);
        uint64_t off = x86_rd_lin(c, lin, (unsigned)sz);
        uint16_t sel = (uint16_t)x86_rd_lin(c, lin + (uint64_t)sz, 2);
        x86_load_seg(c, b == 0xc4 ? S_ES : S_DS, sel);
        SREG(d, sz, off);
        return;
    }
    case 0xc6: case 0xc7: {
        int s = (b & 1) ? sz : 1;
        decode_modrm(c, d);
        uint64_t imm = fetch_imm(c, s == 8 ? 4 : s);
        x86_rm_write(c, d, s, imm);
        return;
    }
    case 0xc8: {
        uint16_t size = fetch16(c);
        unsigned level = x86_fetch8(c) & 31;
        int s = stack_osz(c, d);
        { /* sonda toda a area que sera escrita antes de mexer em RSP */
            uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
            uint64_t n = (uint64_t)s * (level ? level + 1 : 1);
            x86_probe_write(c, x86_lin(c, S_SS, (c->r[R_SP] - n) & m), (unsigned)n);
        }
        x86_push(c, c->r[R_BP], s);
        uint64_t frame = c->r[R_SP];
        if (level) {
            uint64_t bp = c->r[R_BP];
            for (unsigned i = 1; i < level; i++) {
                bp -= (uint64_t)s;
                uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
                x86_push(c, x86_rd_lin(c, x86_lin(c, S_SS, bp & m), (unsigned)s), s);
            }
            x86_push(c, frame, s);
        }
        x86_reg_write(c, R_BP, c->ssz == 2 ? 2 : s, frame, false);
        uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
        c->r[R_SP] = (c->r[R_SP] & ~m) | ((c->r[R_SP] - size) & m);
        return;
    }
    case 0xc9: {
        uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
        int s = stack_osz(c, d);
        uint64_t v = x86_rd_lin(c, x86_lin(c, S_SS, c->r[R_BP] & m), (unsigned)s); /* pode falhar: nada mudou ainda */
        c->r[R_SP] = (c->r[R_SP] & ~m) | ((c->r[R_BP] + (uint64_t)s) & m);
        x86_reg_write(c, R_BP, s, v, false);
        return;
    }
    case 0xca: case 0xcb: {
        uint16_t imm = b == 0xca ? fetch16(c) : 0;
        x86_far_ret(c, sz, imm);
        return;
    }
    case 0xcc: x86_sw_interrupt(c, EXC_BP, c->rip); return;
    case 0xcd: { int v = x86_fetch8(c); x86_sw_interrupt(c, v, c->rip); return; }
    case 0xce:
        if (c->code64) x86_ud(c);
        if (x86_arith_flags(c) & EFL_OF) x86_sw_interrupt(c, EXC_OF, c->rip);
        return;
    case 0xcf: x86_iret(c, sz); return;
    case 0xd4: { /* AAM */
        if (c->code64) x86_ud(c);
        uint8_t base = x86_fetch8(c);
        if (!base) x86_exception(c, EXC_DE, 0, 0);
        uint8_t al = (uint8_t)c->r[R_AX];
        uint8_t ah = al / base;
        al = al % base;
        c->r[R_AX] = (c->r[R_AX] & ~0xffffULL) | al | ((uint64_t)ah << 8);
        x86_set_arith_flags(c, szp(al, 1));
        return;
    }
    case 0xd5: { /* AAD */
        if (c->code64) x86_ud(c);
        uint8_t base = x86_fetch8(c);
        uint8_t al = (uint8_t)(c->r[R_AX] + ((c->r[R_AX] >> 8) & 0xff) * base);
        c->r[R_AX] = (c->r[R_AX] & ~0xffffULL) | al;
        x86_set_arith_flags(c, szp(al, 1));
        return;
    }
    case 0xd6:
        if (c->code64) x86_ud(c);
        x86_reg_write(c, R_AX, 1, x86_cf(c) ? 0xff : 0, false);
        return;
    case 0xd7: {
        uint64_t off = (get_areg(c, d, R_BX) + (c->r[R_AX] & 0xff)) & amask(d);
        x86_reg_write(c, R_AX, 1, x86_rd_lin(c, x86_lin(c, d->seg >= 0 ? d->seg : S_DS, off), 1), false);
        return;
    }
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
        if (c->cr0 & (CR0_EM | CR0_TS)) x86_exception(c, EXC_NM, 0, 0);
        decode_modrm(c, d);
        x87_exec(c, d, b);
        return;
    case 0xe0: case 0xe1: case 0xe2: {
        uint64_t disp = (uint64_t)(int64_t)(int8_t)x86_fetch8(c);
        uint64_t cnt = (get_areg(c, d, R_CX) - 1) & amask(d);
        set_areg(c, d, R_CX, cnt);
        bool take = cnt != 0;
        if (b == 0xe0) take = take && !(x86_arith_flags(c) & EFL_ZF);
        if (b == 0xe1) take = take && (x86_arith_flags(c) & EFL_ZF);
        if (take) jmp_to(c, branch_osz(c, sz), c->rip + disp);
        return;
    }
    case 0xe3: {
        uint64_t disp = (uint64_t)(int64_t)(int8_t)x86_fetch8(c);
        if (!get_areg(c, d, R_CX)) jmp_to(c, branch_osz(c, sz), c->rip + disp);
        return;
    }
    case 0xe4: case 0xe5: {
        uint16_t port = x86_fetch8(c);
        int s = b == 0xe4 ? 1 : (sz == 8 ? 4 : sz);
        x86_reg_write(c, R_AX, s, io_in(c, port, s), false);
        return;
    }
    case 0xe6: case 0xe7: {
        uint16_t port = x86_fetch8(c);
        int s = b == 0xe6 ? 1 : (sz == 8 ? 4 : sz);
        io_out(c, port, c->r[R_AX] & szmask(s), s);
        return;
    }
    case 0xec: case 0xed: {
        int s = b == 0xec ? 1 : (sz == 8 ? 4 : sz);
        x86_reg_write(c, R_AX, s, io_in(c, (uint16_t)c->r[R_DX], s), false);
        return;
    }
    case 0xee: case 0xef: {
        int s = b == 0xee ? 1 : (sz == 8 ? 4 : sz);
        io_out(c, (uint16_t)c->r[R_DX], c->r[R_AX] & szmask(s), s);
        return;
    }
    case 0xe8: {
        int bsz = branch_osz(c, sz);
        uint64_t disp = fetch_imm(c, bsz == 2 ? 2 : 4);
        x86_push(c, c->rip, bsz);
        jmp_to(c, bsz, c->rip + disp);
        return;
    }
    case 0xe9: {
        int bsz = branch_osz(c, sz);
        uint64_t disp = fetch_imm(c, bsz == 2 ? 2 : 4);
        jmp_to(c, bsz, c->rip + disp);
        return;
    }
    case 0xea: {
        if (c->code64) x86_ud(c);
        uint64_t off = sz == 2 ? fetch16(c) : x86_fetch32(c);
        uint16_t sel = fetch16(c);
        x86_far_jump(c, sel, off, sz, false);
        return;
    }
    case 0xeb: {
        uint64_t disp = (uint64_t)(int64_t)(int8_t)x86_fetch8(c);
        jmp_to(c, branch_osz(c, sz), c->rip + disp);
        return;
    }
    case 0xf1: x86_sw_interrupt(c, EXC_DB, c->rip); return;
    case 0xf4:
        if (c->cpl) x86_gp(c, 0);
        c->halted = true;
        return;
    case 0xf5: {
        uint64_t fl = x86_arith_flags(c);
        x86_set_arith_flags(c, fl ^ EFL_CF);
        return;
    }
    case 0xf6: decode_modrm(c, d); group3(c, d, 1); return;
    case 0xf7: decode_modrm(c, d); group3(c, d, sz); return;
    case 0xf8: x86_set_arith_flags(c, x86_arith_flags(c) & ~(uint64_t)EFL_CF); return;
    case 0xf9: x86_set_arith_flags(c, x86_arith_flags(c) | EFL_CF); return;
    case 0xfa: case 0xfb: {
        if ((c->cr0 & CR0_PE) && c->cpl > (int)((c->eflags >> 12) & 3)) x86_gp(c, 0);
        if (b == 0xfa) c->eflags &= ~(uint64_t)EFL_IF;
        else {
            if (!(c->eflags & EFL_IF)) c->irq_inhibit = 1;
            c->eflags |= EFL_IF;
        }
        return;
    }
    case 0xfc: c->eflags &= ~(uint64_t)EFL_DF; return;
    case 0xfd: c->eflags |= EFL_DF; return;
    case 0xfe: case 0xff: {
        decode_modrm(c, d);
        int r = d->reg & 7;
        int s = b == 0xfe ? 1 : sz;
        if (r <= 1) {
            uint64_t v = x86_rm_read(c, d, s), cf = x86_cf(c) ? 1 : 0, res;
            if (r == 0) { res = (v + 1) & szmask(s); set_lazy(c, CC_INC, s, res, v, 1); }
            else { res = (v - 1) & szmask(s); set_lazy(c, CC_DEC, s, res, v, 1); }
            c->cc_aux = cf;
            x86_rm_write(c, d, s, res);
            return;
        }
        if (b == 0xfe) x86_ud(c);
        switch (r) {
        case 2: { /* CALL near */
            int bsz = branch_osz(c, sz);
            uint64_t t = x86_rm_read(c, d, bsz);
            x86_push(c, c->rip, bsz);
            jmp_to(c, bsz, t);
            return;
        }
        case 3: case 5: { /* CALL/JMP far */
            if (!d->mem) x86_ud(c);
            uint64_t lin = x86_ea_lin(c, d);
            uint64_t off = x86_rd_lin(c, lin, (unsigned)sz);
            uint16_t sel = (uint16_t)x86_rd_lin(c, lin + (uint64_t)sz, 2);
            x86_far_jump(c, sel, off, sz, r == 3);
            return;
        }
        case 4: {
            int bsz = branch_osz(c, sz);
            jmp_to(c, bsz, x86_rm_read(c, d, bsz));
            return;
        }
        case 6: {
            int s2 = stack_osz(c, d);
            x86_push(c, x86_rm_read(c, d, s2), s2);
            return;
        }
        default: x86_ud(c);
        }
    }
    default:
        x86_ud(c);
    }
}

void x86_jit_muldiv(x86_cpu *c, uint64_t op, uint64_t v, uint64_t sz) { muldiv(c, (int)op, v, (int)sz); }

uint64_t x86_jit_shift(x86_cpu *c, uint64_t opsz, uint64_t v, uint64_t cnt) /* opsz = op | tamanho << 8 */
{
    return do_shift(c, (int)(opsz & 0xff), v, (unsigned)cnt, (int)(opsz >> 8));
}
