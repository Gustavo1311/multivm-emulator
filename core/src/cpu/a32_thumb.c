/* ARMv7: decodificador Thumb (16 bits) e Thumb-2 (32 bits), com blocos IT. */
#include "cpu_arm32.h"

static inline uint32_t R(a32_cpu *c, unsigned n) { return c->r[n]; }

static inline void set_nz(a32_cpu *c, uint32_t r)
{
    c->nzcv = (c->nzcv & 3) | ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2);
}

static inline void set_nzc(a32_cpu *c, uint32_t r, uint32_t carry)
{
    c->nzcv = (c->nzcv & 1) | ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2) | ((carry & 1) << 1);
}

static void load_pc_or_reg(a32_cpu *c, unsigned n, uint32_t v)
{
    if (n == 15)
        a32_write_pc_bx(c, v);
    else
        c->r[n] = v;
}

static inline uint32_t align4(uint32_t v) { return v & ~3u; }

/* ---------------------------------------------------------- 16 bits */

static void t16(a32_cpu *c, uint32_t h, bool in_it)
{
    bool sf = !in_it;
    bool w;
    uint32_t cin = (c->nzcv >> 1) & 1, co;
    switch (h >> 12) {
    case 0x0: case 0x1: {
        unsigned op = BITS(h, 12, 11);
        if (op < 3) { /* deslocamento imediato */
            unsigned imm5 = BITS(h, 10, 6), rm = BITS(h, 5, 3), rd = BITS(h, 2, 0);
            uint32_t r = a32_shift_imm_c(R(c, rm), op, imm5, cin, &co);
            if (op == 0 && imm5 == 0) co = cin;
            c->r[rd] = r;
            if (sf) set_nzc(c, r, co);
            return;
        }
        unsigned rd = BITS(h, 2, 0), rn = BITS(h, 5, 3);
        uint32_t b = BIT(h, 10) ? BITS(h, 8, 6) : R(c, BITS(h, 8, 6));
        c->r[rd] = a32_dp(c, BIT(h, 9) ? 2 : 4, sf, R(c, rn), b, 0, &w);
        return;
    }
    case 0x2: case 0x3: {
        unsigned op = BITS(h, 12, 11), rdn = BITS(h, 10, 8);
        uint32_t imm = h & 0xff;
        switch (op) {
        case 0: c->r[rdn] = imm; if (sf) set_nz(c, imm); break;
        case 1: a32_dp(c, 10, true, R(c, rdn), imm, 0, &w); break;
        case 2: c->r[rdn] = a32_dp(c, 4, sf, R(c, rdn), imm, 0, &w); break;
        default: c->r[rdn] = a32_dp(c, 2, sf, R(c, rdn), imm, 0, &w); break;
        }
        return;
    }
    case 0x4:
        if (!BIT(h, 11)) {
            if (!BIT(h, 10)) { /* processamento de dados */
                unsigned op = BITS(h, 9, 6), rm = BITS(h, 5, 3), rdn = BITS(h, 2, 0);
                uint32_t a = R(c, rdn), b = R(c, rm), r;
                switch (op) {
                case 0: r = a & b; c->r[rdn] = r; if (sf) set_nz(c, r); return;
                case 1: r = a ^ b; c->r[rdn] = r; if (sf) set_nz(c, r); return;
                case 2: case 3: case 4: case 7: {
                    static const unsigned ty[8] = {0, 0, 0, 1, 2, 0, 0, 3};
                    r = a32_shift_c(a, ty[op], b & 0xff, cin, &co);
                    c->r[rdn] = r;
                    if (sf) set_nzc(c, r, co);
                    return;
                }
                case 5: c->r[rdn] = a32_dp(c, 5, sf, a, b, 0, &w); return;
                case 6: c->r[rdn] = a32_dp(c, 6, sf, a, b, 0, &w); return;
                case 8: a32_dp(c, 8, true, a, b, cin, &w); return;
                case 9: c->r[rdn] = a32_dp(c, 3, sf, b, 0, 0, &w); return; /* RSB #0 */
                case 10: a32_dp(c, 10, true, a, b, 0, &w); return;
                case 11: a32_dp(c, 11, true, a, b, 0, &w); return;
                case 12: r = a | b; c->r[rdn] = r; if (sf) set_nz(c, r); return;
                case 13: r = a * b; c->r[rdn] = r; if (sf) set_nz(c, r); return;
                case 14: r = a & ~b; c->r[rdn] = r; if (sf) set_nz(c, r); return;
                default: r = ~b; c->r[rdn] = r; if (sf) set_nz(c, r); return;
                }
            }
            /* dados especiais / BX */
            unsigned op = BITS(h, 9, 8), rm = BITS(h, 6, 3), rdn = (BIT(h, 7) << 3) | BITS(h, 2, 0);
            switch (op) {
            case 0: {
                uint32_t r = R(c, rdn) + R(c, rm);
                if (rdn == 15) c->npc = r & ~1u;
                else c->r[rdn] = r;
                return;
            }
            case 1: a32_dp(c, 10, true, R(c, rdn), R(c, rm), 0, &w); return;
            case 2:
                if (rdn == 15) c->npc = R(c, rm) & ~1u;
                else c->r[rdn] = R(c, rm);
                return;
            default: {
                uint32_t t = R(c, rm);
                if (BIT(h, 7))
                    c->r[14] = c->npc | 1;
                a32_write_pc_bx(c, t);
                return;
            }
            }
        } else { /* LDR literal */
            unsigned rt = BITS(h, 10, 8);
            c->r[rt] = a32_rd(c, align4(c->cur + 4) + (h & 0xff) * 4, 4);
        }
        return;
    case 0x5: {
        unsigned op = BITS(h, 11, 9), rm = BITS(h, 8, 6), rn = BITS(h, 5, 3), rt = BITS(h, 2, 0);
        uint32_t addr = R(c, rn) + R(c, rm);
        switch (op) {
        case 0: a32_wr(c, addr, R(c, rt), 4); break;
        case 1: a32_wr(c, addr, R(c, rt), 2); break;
        case 2: a32_wr(c, addr, R(c, rt), 1); break;
        case 3: c->r[rt] = (uint32_t)(int32_t)(int8_t)a32_rd(c, addr, 1); break;
        case 4: c->r[rt] = a32_rd(c, addr, 4); break;
        case 5: c->r[rt] = a32_rd(c, addr, 2); break;
        case 6: c->r[rt] = a32_rd(c, addr, 1); break;
        default: c->r[rt] = (uint32_t)(int32_t)(int16_t)a32_rd(c, addr, 2); break;
        }
        return;
    }
    case 0x6: case 0x7: case 0x8: {
        unsigned imm5 = BITS(h, 10, 6), rn = BITS(h, 5, 3), rt = BITS(h, 2, 0);
        unsigned sz = (h >> 12) == 6 ? 4 : (h >> 12) == 7 ? 1 : 2;
        uint32_t addr = R(c, rn) + imm5 * sz;
        if (BIT(h, 11)) c->r[rt] = a32_rd(c, addr, sz);
        else a32_wr(c, addr, R(c, rt), sz);
        return;
    }
    case 0x9: {
        unsigned rt = BITS(h, 10, 8);
        uint32_t addr = R(c, 13) + (h & 0xff) * 4;
        if (BIT(h, 11)) c->r[rt] = a32_rd(c, addr, 4);
        else a32_wr(c, addr, R(c, rt), 4);
        return;
    }
    case 0xa: {
        unsigned rd = BITS(h, 10, 8);
        c->r[rd] = (BIT(h, 11) ? R(c, 13) : align4(c->cur + 4)) + (h & 0xff) * 4;
        return;
    }
    case 0xb: {
        unsigned op = BITS(h, 11, 8);
        switch (op) {
        case 0x0:
            if (BIT(h, 7)) c->r[13] -= (h & 0x7f) * 4;
            else c->r[13] += (h & 0x7f) * 4;
            return;
        case 0x1: case 0x3: case 0x9: case 0xb: { /* CBZ/CBNZ */
            unsigned rn = BITS(h, 2, 0);
            uint32_t off = (BIT(h, 9) << 6) | (BITS(h, 7, 3) << 1);
            bool nz = BIT(h, 11);
            if ((R(c, rn) != 0) == nz)
                c->npc = c->cur + 4 + off;
            return;
        }
        case 0x2: {
            unsigned rm = BITS(h, 5, 3), rd = BITS(h, 2, 0);
            static const unsigned t[4] = {3, 2, 7, 6};
            c->r[rd] = a32_extend(t[BITS(h, 7, 6)], R(c, rm), 0, 0);
            return;
        }
        case 0x4: case 0x5: { /* PUSH */
            uint32_t list = (h & 0xff) | (BIT(h, 8) ? 0x4000u : 0);
            a32_ldm_stm(c, false, false, true, 13, true, list, false);
            return;
        }
        case 0x6:
            if (BITS(h, 7, 5) == 3) { /* CPS */
                a32_cps(c, BIT(h, 4) ? 3 : 2, false, BITS(h, 2, 0), 0);
                return;
            }
            if (BITS(h, 7, 5) == 2) return; /* SETEND */
            a32_undef(c);
        case 0xa: {
            unsigned rm = BITS(h, 5, 3), rd = BITS(h, 2, 0);
            uint32_t v = R(c, rm);
            switch (BITS(h, 7, 6)) {
            case 0: c->r[rd] = bswap32(v); return;
            case 1: c->r[rd] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); return;
            case 3: c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff)); return;
            default: a32_undef(c);
            }
        }
        case 0xc: case 0xd: { /* POP */
            uint32_t list = (h & 0xff) | (BIT(h, 8) ? 0x8000u : 0);
            a32_ldm_stm(c, true, true, false, 13, true, list, false);
            return;
        }
        case 0xe:
            a32_bkpt(c);
            return;
        case 0xf:
            if (h & 0xf) { /* IT */
                c->it = h & 0xff;
                return;
            }
            a32_hint(c, BITS(h, 7, 4));
            return;
        default:
            a32_undef(c);
        }
    }
    case 0xc: {
        unsigned rn = BITS(h, 10, 8);
        uint32_t list = h & 0xff;
        bool load = BIT(h, 11);
        a32_ldm_stm(c, load, true, false, rn, !(load && (list & (1u << rn))), list, false);
        return;
    }
    case 0xd: {
        unsigned cond = BITS(h, 11, 8);
        if (cond == 0xe) a32_undef(c);
        if (cond == 0xf) { a32_svc(c); return; }
        if (arm_cond(cond, c->nzcv))
            c->npc = c->cur + 4 + (uint32_t)((int32_t)(h << 24) >> 23);
        return;
    }
    case 0xe:
        c->npc = c->cur + 4 + (uint32_t)((int32_t)(h << 21) >> 20);
        return;
    default:
        a32_undef(c);
    }
}

/* ---------------------------------------------------------- 32 bits */

static uint32_t thumb_expand_imm_c(uint32_t imm12, uint32_t cin, uint32_t *cout)
{
    if (!(imm12 & 0xc00)) {
        uint32_t b = imm12 & 0xff;
        *cout = cin;
        switch ((imm12 >> 8) & 3) {
        case 0: return b;
        case 1: return b | (b << 16);
        case 2: return (b << 8) | (b << 24);
        default: return b | (b << 8) | (b << 16) | (b << 24);
        }
    }
    uint32_t v = ror32(0x80 | (imm12 & 0x7f), (imm12 >> 7) & 0x1f);
    *cout = v >> 31;
    return v;
}

/* operacoes de dados Thumb-2 (registrador deslocado ou imediato modificado) */
static void t32_dp(a32_cpu *c, unsigned op, bool s, unsigned rn, unsigned rd, uint32_t b, uint32_t sc)
{
    bool w;
    uint32_t a = R(c, rn), r;
    switch (op) {
    case 0: /* AND/TST */
        if (rd == 15 && s) { a32_dp(c, 8, true, a, b, sc, &w); return; }
        r = a32_dp(c, 0, s, a, b, sc, &w);
        break;
    case 1: r = a32_dp(c, 14, s, a, b, sc, &w); break;
    case 2: r = a32_dp(c, rn == 15 ? 13 : 12, s, a, b, sc, &w); break;
    case 3: r = rn == 15 ? a32_dp(c, 15, s, a, b, sc, &w) : a32_dp(c, 12, s, a, ~b, sc, &w); break;
    case 4:
        if (rd == 15 && s) { a32_dp(c, 9, true, a, b, sc, &w); return; }
        r = a32_dp(c, 1, s, a, b, sc, &w);
        break;
    case 8:
        if (rd == 15 && s) { a32_dp(c, 11, true, a, b, sc, &w); return; }
        r = a32_dp(c, 4, s, a, b, sc, &w);
        break;
    case 10: r = a32_dp(c, 5, s, a, b, sc, &w); break;
    case 11: r = a32_dp(c, 6, s, a, b, sc, &w); break;
    case 13:
        if (rd == 15 && s) { a32_dp(c, 10, true, a, b, sc, &w); return; }
        r = a32_dp(c, 2, s, a, b, sc, &w);
        break;
    case 14: r = a32_dp(c, 3, s, a, b, sc, &w); break;
    default: a32_undef(c);
    }
    if (rd == 15)
        c->npc = r & ~1u;
    else
        c->r[rd] = r;
}

static void t32_ldst_single(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned sz_code = BITS(h1, 6, 5), rn = BITS(h1, 3, 0), rt = BITS(h2, 15, 12);
    bool load = BIT(h1, 4), sign = BIT(h1, 8);
    unsigned sz = 1u << sz_code;
    if (sz_code == 3)
        a32_undef(c);
    uint32_t addr, wbv = 0;
    bool wb = false;
    int priv = a32_priv(c);
    if (rn == 15) {
        if (!load) a32_undef(c);
        uint32_t base = align4(c->cur + 4);
        uint32_t imm = h2 & 0xfff;
        addr = BIT(h1, 7) ? base + imm : base - imm;
    } else if (BIT(h1, 7)) {
        addr = R(c, rn) + (h2 & 0xfff);
    } else if (BIT(h2, 11)) {
        bool p = BIT(h2, 10), u = BIT(h2, 9), wr = BIT(h2, 8);
        uint32_t imm = h2 & 0xff, base = R(c, rn);
        uint32_t off_addr = u ? base + imm : base - imm;
        addr = p ? off_addr : base;
        if (p && u && !wr)
            priv = 0; /* LDRT/STRT */
        if (wr || !p) {
            wb = true;
            wbv = off_addr;
        }
    } else {
        if (BITS(h2, 10, 6)) a32_undef(c);
        addr = R(c, rn) + (R(c, BITS(h2, 3, 0)) << BITS(h2, 5, 4));
    }
    if (!load) {
        if (priv == a32_priv(c)) a32_wr(c, addr, R(c, rt), sz);
        else a32_write_slow(c, addr, R(c, rt), sz, priv);
        if (wb) c->r[rn] = wbv;
        return;
    }
    if (rt == 15 && sz < 4) /* PLD/PLI */
        return;
    uint32_t v = priv == a32_priv(c) ? a32_rd(c, addr, sz) : a32_read_slow(c, addr, sz, priv);
    if (sign)
        v = sz == 1 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
    if (wb) c->r[rn] = wbv;
    load_pc_or_reg(c, rt, v);
}

static void t32_dual_excl(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned op1 = BITS(h1, 8, 7), op2 = BITS(h1, 5, 4), op3 = BITS(h2, 7, 4);
    unsigned rn = BITS(h1, 3, 0), rt = BITS(h2, 15, 12), rt2 = BITS(h2, 11, 8), rd = BITS(h2, 3, 0);
    if (op1 == 0 && op2 == 0) { /* STREX */
        uint32_t addr = R(c, rn) + (h2 & 0xff) * 4;
        uint32_t st = 1;
        if (c->excl_valid && c->excl_addr == addr && c->excl_size == 4) {
            a32_wr(c, addr, R(c, rt), 4);
            st = 0;
        }
        c->excl_valid = false;
        c->r[rt2] = st;
        return;
    }
    if (op1 == 0 && op2 == 1) { /* LDREX */
        uint32_t addr = R(c, rn) + (h2 & 0xff) * 4;
        uint32_t v = a32_rd(c, addr, 4);
        c->r[rt] = v;
        c->excl_addr = addr;
        c->excl_val = v;
        c->excl_size = 4;
        c->excl_valid = true;
        return;
    }
    if ((op1 & 2) || op2 >= 2) { /* LDRD/STRD */
        bool p = BIT(h1, 8), u = BIT(h1, 7), wr = BIT(h1, 5), l = BIT(h1, 4);
        uint32_t imm = (h2 & 0xff) * 4;
        uint32_t base = rn == 15 ? align4(c->cur + 4) : R(c, rn);
        uint32_t off_addr = u ? base + imm : base - imm;
        uint32_t addr = p ? off_addr : base;
        if (l) {
            uint32_t lo = a32_rd(c, addr, 4), hi = a32_rd(c, addr + 4, 4);
            if (wr) c->r[rn] = off_addr;
            c->r[rt] = lo;
            c->r[rt2] = hi;
        } else {
            a32_wr(c, addr + 4, R(c, rt2), 4);
            a32_wr(c, addr, R(c, rt), 4);
            if (wr) c->r[rn] = off_addr;
        }
        return;
    }
    if (op1 == 1 && op2 == 0) { /* STREXB/H/D */
        unsigned sz = op3 == 4 ? 1 : op3 == 5 ? 2 : op3 == 7 ? 8 : 0;
        if (!sz) a32_undef(c);
        uint32_t addr = R(c, rn), st = 1;
        if (c->excl_valid && c->excl_addr == addr && c->excl_size == (int)sz) {
            if (sz == 8) {
                a32_wr(c, addr, R(c, rt), 4);
                a32_wr(c, addr + 4, R(c, rt2), 4);
            } else {
                a32_wr(c, addr, R(c, rt), sz);
            }
            st = 0;
        }
        c->excl_valid = false;
        c->r[rd] = st;
        return;
    }
    if (op1 == 1 && op2 == 1) {
        if (op3 == 0 || op3 == 1) { /* TBB/TBH */
            uint32_t base = rn == 15 ? c->cur + 4 : R(c, rn);
            uint32_t off = op3 == 0 ? a32_rd(c, base + R(c, rd), 1) : a32_rd(c, base + R(c, rd) * 2, 2);
            c->npc = c->cur + 4 + 2 * off;
            return;
        }
        unsigned sz = op3 == 4 ? 1 : op3 == 5 ? 2 : op3 == 7 ? 8 : 0;
        if (!sz) a32_undef(c);
        uint32_t addr = R(c, rn);
        if (sz == 8) {
            uint32_t lo = a32_rd(c, addr, 4), hi = a32_rd(c, addr + 4, 4);
            c->r[rt] = lo;
            c->r[rt2] = hi;
            c->excl_val = ((uint64_t)hi << 32) | lo;
        } else {
            uint32_t v = a32_rd(c, addr, sz);
            c->r[rt] = v;
            c->excl_val = v;
        }
        c->excl_addr = addr;
        c->excl_size = (int)sz;
        c->excl_valid = true;
        return;
    }
    a32_undef(c);
}

static void t32_branch_misc(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned op1 = BITS(h2, 14, 12), op = BITS(h1, 10, 4);
    uint32_t s = BIT(h1, 10), j1 = BIT(h2, 13), j2 = BIT(h2, 11);
    if ((op1 & 5) == 0) {
        if ((op & 0x38) != 0x38) { /* B<cond>.W */
            uint32_t imm = (s << 20) | (j2 << 19) | (j1 << 18) | (BITS(h1, 5, 0) << 12) | (BITS(h2, 10, 0) << 1);
            int32_t off = (int32_t)(imm << 11) >> 11;
            if (arm_cond(BITS(h1, 9, 6), c->nzcv))
                c->npc = c->cur + 4 + (uint32_t)off;
            return;
        }
        switch (op) {
        case 0x38: case 0x39: /* MSR */
            a32_msr(c, BIT(h1, 4), BITS(h2, 11, 8), R(c, BITS(h1, 3, 0)));
            return;
        case 0x3a: /* hints / CPS */
            if (BITS(h2, 10, 8) == 0) {
                a32_hint(c, h2 & 0xff);
                return;
            }
            a32_cps(c, BITS(h2, 10, 9), BIT(h2, 8), BITS(h2, 7, 5), BITS(h2, 4, 0));
            return;
        case 0x3b: { /* CLREX/DSB/DMB/ISB (CLREX = 2 em Thumb, 1 em ARM) */
            unsigned o = BITS(h2, 7, 4);
            a32_barrier(c, o == 2 ? 1 : o);
            return;
        }
        case 0x3c: /* BXJ */
            a32_write_pc_bx(c, R(c, BITS(h1, 3, 0)));
            return;
        case 0x3d: /* SUBS PC, LR, #imm8 */
            if (!a32_priv(c)) a32_undef(c);
            a32_exc_return(c, R(c, 14) - (h2 & 0xff));
            return;
        case 0x3e: case 0x3f: /* MRS */
            c->r[BITS(h2, 11, 8)] = a32_mrs(c, BIT(h1, 4));
            return;
        default:
            break;
        }
        if (op1 == 0 && (op == 0x7e || op == 0x7f)) { /* HVC/SMC */
            if (!a32_priv(c)) a32_undef(c);
            a32_psci(c);
            return;
        }
        a32_undef(c);
    }
    if (op1 == 2 && op == 0x7f)
        a32_undef(c); /* UDF */
    uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
    uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | (BITS(h1, 9, 0) << 12) | (BITS(h2, 10, 0) << 1);
    int32_t off = (int32_t)(imm << 7) >> 7;
    if (op1 & 4) { /* BL / BLX */
        c->r[14] = c->npc | 1;
        if (op1 & 1) {
            c->npc = c->cur + 4 + (uint32_t)off;
        } else {
            c->thumb = false;
            c->npc = align4(c->cur + 4) + (uint32_t)off;
        }
        return;
    }
    c->npc = c->cur + 4 + (uint32_t)off; /* B.W */
}

static void t32_plain_imm(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned op = BITS(h1, 8, 4), rn = BITS(h1, 3, 0), rd = BITS(h2, 11, 8);
    uint32_t imm12 = (BIT(h1, 10) << 11) | (BITS(h2, 14, 12) << 8) | (h2 & 0xff);
    uint32_t imm16 = (BITS(h1, 3, 0) << 12) | imm12;
    unsigned lsb = (BITS(h2, 14, 12) << 2) | BITS(h2, 7, 6), wm1 = h2 & 0x1f;
    switch (op) {
    case 0x00: c->r[rd] = (rn == 15 ? align4(c->cur + 4) : R(c, rn)) + imm12; return;
    case 0x0a: c->r[rd] = (rn == 15 ? align4(c->cur + 4) : R(c, rn)) - imm12; return;
    case 0x04: c->r[rd] = imm16; return;
    case 0x0c: c->r[rd] = (c->r[rd] & 0xffff) | (imm16 << 16); return;
    case 0x10: case 0x12: case 0x18: case 0x1a: {
        bool uns = op & 8;
        unsigned sat = h2 & 0x1f;
        if ((op & 2) && lsb == 0) { /* SSAT16/USAT16 */
            uint32_t v = R(c, rn), lo, hi;
            if (uns) { lo = a32_sat_u(c, (int16_t)v, sat); hi = a32_sat_u(c, (int16_t)(v >> 16), sat); }
            else { lo = a32_sat_s(c, (int16_t)v, sat + 1); hi = a32_sat_s(c, (int16_t)(v >> 16), sat + 1); }
            c->r[rd] = (lo & 0xffff) | (hi << 16);
            return;
        }
        uint32_t co;
        int32_t v = (int32_t)a32_shift_imm_c(R(c, rn), (op & 2) ? 2 : 0, lsb, 0, &co);
        c->r[rd] = uns ? a32_sat_u(c, v, sat) : a32_sat_s(c, v, sat + 1);
        return;
    }
    case 0x14: { /* SBFX */
        unsigned w = wm1 + 1;
        if (lsb + w > 32) a32_undef(c);
        c->r[rd] = (uint32_t)((int32_t)(R(c, rn) << (32 - lsb - w)) >> (32 - w));
        return;
    }
    case 0x1c: { /* UBFX */
        unsigned w = wm1 + 1;
        if (lsb + w > 32) a32_undef(c);
        c->r[rd] = (R(c, rn) >> lsb) & (w == 32 ? ~0u : ((1u << w) - 1));
        return;
    }
    case 0x16: { /* BFI/BFC */
        unsigned msb = wm1;
        if (msb < lsb) a32_undef(c);
        uint32_t mask = (msb - lsb == 31) ? ~0u : (((1u << (msb - lsb + 1)) - 1) << lsb);
        uint32_t src = rn == 15 ? 0 : R(c, rn) << lsb;
        c->r[rd] = (c->r[rd] & ~mask) | (src & mask);
        return;
    }
    default:
        a32_undef(c);
    }
}

static void t32_dp_reg(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned op1 = BITS(h1, 7, 4), op2 = BITS(h2, 7, 4);
    unsigned rn = BITS(h1, 3, 0), rd = BITS(h2, 11, 8), rm = BITS(h2, 3, 0);
    if (!(op1 & 8) && op2 == 0) { /* deslocamento por registrador */
        uint32_t co, r = a32_shift_c(R(c, rn), BITS(h1, 6, 5), R(c, rm) & 0xff, (c->nzcv >> 1) & 1, &co);
        c->r[rd] = r;
        if (BIT(h1, 4)) set_nzc(c, r, co);
        return;
    }
    if (!(op1 & 8) && (op2 & 8)) { /* extensoes */
        static const int map[6] = {3, 7, 0, 4, 2, 6};
        if (op1 > 5) a32_undef(c);
        c->r[rd] = a32_extend((unsigned)map[op1], R(c, rm), BITS(h2, 5, 4), rn == 15 ? 0 : R(c, rn));
        return;
    }
    if ((op1 & 8) && !(op2 & 8)) { /* soma/subtracao paralelas */
        static const int opmap[8] = {4, 0, 1, -1, 7, 3, 2, -1};
        unsigned kind = op2 & 3;
        int o = opmap[BITS(h1, 6, 4)];
        if (kind != 3 && o >= 0) {
            unsigned pre = ((op2 & 4) ? 4 : 0) | (kind + 1);
            c->r[rd] = a32_parallel(c, pre, (unsigned)o, R(c, rn), R(c, rm));
            return;
        }
        a32_undef(c);
    }
    if ((op1 & 0xc) == 0x8 && (op2 & 0xc) == 0x8) { /* misc */
        unsigned a = BITS(h1, 5, 4), b = BITS(h2, 5, 4);
        uint32_t v = R(c, rm);
        if (a == 0) { c->r[rd] = a32_satop(c, b == 0 ? 0 : b == 1 ? 2 : b == 2 ? 1 : 3, (int32_t)R(c, rm), (int32_t)R(c, rn)); return; }
        if (a == 1) {
            switch (b) {
            case 0: c->r[rd] = bswap32(v); return;
            case 1: c->r[rd] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); return;
            case 2: c->r[rd] = a32_rbit(v); return;
            default: c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff)); return;
            }
        }
        if (a == 2 && b == 0) { c->r[rd] = a32_sel(c, R(c, rn), R(c, rm)); return; }
        if (a == 3 && b == 0) { c->r[rd] = v ? (uint32_t)__builtin_clz(v) : 32; return; }
    }
    a32_undef(c);
}

static void t32_mul(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned op1 = BITS(h1, 6, 4), op2 = BITS(h2, 5, 4);
    unsigned rn = BITS(h1, 3, 0), ra = BITS(h2, 15, 12), rd = BITS(h2, 11, 8), rm = BITS(h2, 3, 0);
    uint32_t n = R(c, rn), m = R(c, rm), a = ra == 15 ? 0 : R(c, ra);
    switch (op1) {
    case 0:
        if (op2 == 0) { c->r[rd] = n * m + a; return; }
        if (op2 == 1) { c->r[rd] = R(c, ra) - n * m; return; }
        break;
    case 1: { /* SMLAxy/SMULxy */
        int32_t x = BIT(h2, 5) ? (int16_t)(n >> 16) : (int16_t)n;
        int32_t y = BIT(h2, 4) ? (int16_t)(m >> 16) : (int16_t)m;
        int64_t r = (int64_t)x * y;
        if (ra != 15) {
            r += (int32_t)R(c, ra);
            if (r != (int32_t)r) c->q = true;
        }
        c->r[rd] = (uint32_t)r;
        return;
    }
    case 2: if (op2 < 2) { c->r[rd] = a32_smlad(c, false, op2 & 1, n, m, a, ra != 15); return; } break;
    case 3: if (op2 < 2) { /* SMLAWy/SMULWy */
        int64_t p = ((int64_t)(int32_t)n * (BIT(h2, 4) ? (int16_t)(m >> 16) : (int16_t)m)) >> 16;
        if (ra != 15) {
            p += (int32_t)R(c, ra);
            if (p != (int32_t)p) c->q = true;
        }
        c->r[rd] = (uint32_t)p;
        return;
    } break;
    case 4: if (op2 < 2) { c->r[rd] = a32_smlad(c, true, op2 & 1, n, m, a, ra != 15); return; } break;
    case 5: if (op2 < 2) { c->r[rd] = a32_smmul(false, op2 & 1, n, m, a, ra != 15); return; } break;
    case 6: if (op2 < 2) { c->r[rd] = a32_smmul(true, op2 & 1, n, m, R(c, ra), true); return; } break;
    case 7: if (op2 == 0) { c->r[rd] = a32_usad8(n, m) + a; return; } break;
    }
    a32_undef(c);
}

static void t32_long_mul(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned op1 = BITS(h1, 6, 4), op2 = BITS(h2, 7, 4);
    unsigned rn = BITS(h1, 3, 0), rlo = BITS(h2, 15, 12), rhi = BITS(h2, 11, 8), rm = BITS(h2, 3, 0);
    uint32_t n = R(c, rn), m = R(c, rm);
    uint64_t acc = ((uint64_t)R(c, rhi) << 32) | R(c, rlo), r;
    switch (op1) {
    case 0: if (op2) break; r = (uint64_t)((int64_t)(int32_t)n * (int32_t)m); goto store;
    case 1: if (op2 != 15) break; c->r[rhi] = a32_sdiv((int32_t)n, (int32_t)m); return;
    case 2: if (op2) break; r = (uint64_t)n * m; goto store;
    case 3: if (op2 != 15) break; c->r[rhi] = m ? n / m : 0; return;
    case 4:
        if (op2 == 0) { r = acc + (uint64_t)((int64_t)(int32_t)n * (int32_t)m); goto store; }
        if ((op2 & 0xc) == 8) {
            int32_t x = BIT(h2, 5) ? (int16_t)(n >> 16) : (int16_t)n;
            int32_t y = BIT(h2, 4) ? (int16_t)(m >> 16) : (int16_t)m;
            r = acc + (uint64_t)(int64_t)(x * y);
            goto store;
        }
        if ((op2 & 0xe) == 0xc) { r = a32_smlald(false, op2 & 1, n, m, acc); goto store; }
        break;
    case 5: if ((op2 & 0xe) == 0xc) { r = a32_smlald(true, op2 & 1, n, m, acc); goto store; } break;
    case 6:
        if (op2 == 0) { r = acc + (uint64_t)n * m; goto store; }
        if (op2 == 6) { r = (uint64_t)n * m + R(c, rlo) + R(c, rhi); goto store; }
        break;
    }
    a32_undef(c);
store:
    c->r[rlo] = (uint32_t)r;
    c->r[rhi] = (uint32_t)(r >> 32);
}

static void t32(a32_cpu *c, uint32_t h1, uint32_t h2)
{
    unsigned op1 = BITS(h1, 12, 11), op2 = BITS(h1, 10, 4);
    if (op1 == 1) {
        if ((op2 & 0x64) == 0x00) { /* LDM/STM/SRS/RFE */
            unsigned op = BITS(h1, 8, 7), rn = BITS(h1, 3, 0);
            bool l = BIT(h1, 4), w = BIT(h1, 5);
            if (op == 1 || op == 2) {
                uint32_t list = h2;
                a32_ldm_stm(c, l, op == 1, op == 2, rn, w && !(l && (list & (1u << rn))), list, false);
                return;
            }
            if (!a32_priv(c)) a32_undef(c);
            if (l) { /* RFE */
                uint32_t base = R(c, rn);
                uint32_t addr = op == 0 ? base - 8 : base;
                uint32_t pc = a32_rd(c, addr, 4), cpsr = a32_rd(c, addr + 4, 4);
                if (w) c->r[rn] = op == 0 ? base - 8 : base + 8;
                a32_set_cpsr(c, cpsr, 0xf);
                c->npc = c->thumb ? pc & ~1u : pc & ~3u;
                return;
            }
            a32_undef(c); /* SRS em Thumb: raro */
        }
        if ((op2 & 0x64) == 0x04) { t32_dual_excl(c, h1, h2); return; }
        if ((op2 & 0x60) == 0x20) {
            unsigned op = BITS(h1, 8, 5), rn = BITS(h1, 3, 0), rd = BITS(h2, 11, 8), rm = BITS(h2, 3, 0);
            unsigned type = BITS(h2, 5, 4), imm5 = (BITS(h2, 14, 12) << 2) | BITS(h2, 7, 6);
            if (op == 6) { /* PKH */
                uint32_t co, sh = a32_shift_imm_c(R(c, rm), type, imm5, 0, &co);
                if (BIT(h2, 5)) c->r[rd] = (R(c, rn) & 0xffff0000u) | (sh & 0xffff);
                else c->r[rd] = (R(c, rn) & 0xffff) | (sh & 0xffff0000u);
                return;
            }
            uint32_t sc, b = a32_shift_imm_c(R(c, rm), type, imm5, (c->nzcv >> 1) & 1, &sc);
            if (type == 0 && imm5 == 0) sc = (c->nzcv >> 1) & 1;
            t32_dp(c, op, BIT(h1, 4), rn, rd, b, sc);
            return;
        }
        goto coproc;
    }
    if (op1 == 2) {
        if (BIT(h2, 15)) { t32_branch_misc(c, h1, h2); return; }
        if (!BIT(op2, 5)) {
            uint32_t imm12 = (BIT(h1, 10) << 11) | (BITS(h2, 14, 12) << 8) | (h2 & 0xff);
            uint32_t sc, b = thumb_expand_imm_c(imm12, (c->nzcv >> 1) & 1, &sc);
            t32_dp(c, BITS(h1, 8, 5), BIT(h1, 4), BITS(h1, 3, 0), BITS(h2, 11, 8), b, sc);
            return;
        }
        t32_plain_imm(c, h1, h2);
        return;
    }
    /* op1 == 3 */
    if ((op2 & 0x71) == 0x00 || (op2 & 0x67) == 0x01 || (op2 & 0x67) == 0x03 || (op2 & 0x67) == 0x05) {
        t32_ldst_single(c, h1, h2);
        return;
    }
    if ((op2 & 0x70) == 0x20) { t32_dp_reg(c, h1, h2); return; }
    if ((op2 & 0x78) == 0x30) { t32_mul(c, h1, h2); return; }
    if ((op2 & 0x78) == 0x38) { t32_long_mul(c, h1, h2); return; }
    if ((op2 & 0x40) == 0x40) goto coproc;
    a32_undef(c);
coproc:
    if (BIT(h1, 12)) /* variantes "2" e NEON: nao suportadas */
        a32_undef(c);
    {
        uint32_t insn = 0xe0000000u | ((h1 & 0xfff) << 16) | h2;
        if (BITS(insn, 27, 24) == 0xf) a32_undef(c);
        a32_coproc(c, insn);
    }
}

void a32_exec_thumb(a32_cpu *c)
{
    uint32_t pc = c->cur;
    uint32_t h1 = a32_fetch16(c, pc);
    bool wide = (h1 >> 11) >= 0x1d;
    uint32_t h2 = 0;
    if (wide) {
        h2 = a32_fetch16(c, pc + 2);
        c->npc = pc + 4;
    } else {
        c->npc = pc + 2;
    }
    c->r[15] = pc + 4;
    c->insn = (h1 << 16) | h2;
    uint32_t it = c->it;
    bool in_it = (it & 0xf) != 0;
    if (in_it) {
        uint32_t nit = (it & 7) == 0 ? 0 : ((it & 0xe0) | ((it << 1) & 0x1f));
        if (!arm_cond(it >> 4, c->nzcv)) {
            c->it = nit;
            return;
        }
        if (wide) t32(c, h1, h2);
        else t16(c, h1, true);
        /* nao avanca se a instrucao alterou o ITSTATE (retorno de excecao) */
        if (c->it == it)
            c->it = nit;
        return;
    }
    if (wide) t32(c, h1, h2);
    else t16(c, h1, false);
}
