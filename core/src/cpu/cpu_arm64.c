/*
 * CPU AArch64 (ARMv8.0-A + LSE/CRC32), interpretador.
 * EL0 e EL1 (o kernel inicia em EL1; PSCI e tratado pelo emulador via HVC/SMC).
 * MMU estagio 1 com granulos de 4K/64K, TBI, TLB por software.
 */
#include "cpu_arm64.h"

#include <stdlib.h>

#define XR(n) (c->x[n])
static inline void setx(a64_cpu *c, unsigned n, uint64_t v)
{
    if (n != 31)
        c->x[n] = v;
}
static inline uint64_t *spp(a64_cpu *c) { return &c->sp[(c->el && c->spsel) ? 1 : 0]; }
static inline uint64_t xsp(a64_cpu *c, unsigned n) { return n == 31 ? *spp(c) : c->x[n]; }
static inline void setxsp(a64_cpu *c, unsigned n, uint64_t v)
{
    if (n == 31)
        *spp(c) = v;
    else
        c->x[n] = v;
}

/* ------------------------------------------------------------------ TLB */

static void tlb_flush(a64_cpu *c)
{
    for (int el = 0; el < 2; el++)
        for (unsigned i = 0; i < A64_TLB_SIZE; i++) {
            c->tlb[el][i].tag_r = TLB_INVALID;
            c->tlb[el][i].tag_w = TLB_INVALID;
            c->tlb[el][i].tag_x = TLB_INVALID;
        }
    c->fetch_page = 1;
}

/* ---------------------------------------------------------- excecoes */

static uint32_t get_pstate(a64_cpu *c)
{
    return (c->nzcv << 28) | (c->daif << 6) | ((uint32_t)c->el << 2) | (uint32_t)c->spsel;
}

static void take_exception(a64_cpu *c, int type, uint32_t esr, uint64_t ret)
{
    uint64_t off;
    if (c->el == 1)
        off = c->spsel ? 0x200 : 0x000;
    else
        off = 0x400;
    off += (uint64_t)type * 0x80;
    c->spsr = get_pstate(c);
    c->elr = ret;
    if (type == 0)
        c->esr = esr;
    c->el = 1;
    c->spsel = 1;
    c->daif = 0xf;
    c->excl_valid = false;
    c->pc = c->vbar + off;
    c->fetch_page = 1;
    c->halted = false;
}

static _Noreturn void sync_exception(a64_cpu *c, uint32_t esr, uint64_t ret)
{
    take_exception(c, 0, esr, ret);
    longjmp(c->jb, 1);
}

_Noreturn void a64_undef(a64_cpu *c)
{
    LOGD("a64: instrucao indefinida em 0x%llx (EL%d)", (unsigned long long)c->cur, c->el);
    sync_exception(c, (1u << 25), c->cur);
}

bool a64_fp_enabled(a64_cpu *c)
{
    unsigned fpen = (c->cpacr >> 20) & 3;
    return fpen == 3 || (fpen == 1 && c->el == 1);
}

_Noreturn void a64_fp_trap(a64_cpu *c)
{
    sync_exception(c, (0x07u << 26) | (1u << 25) | (1u << 24) | (0xeu << 20), c->cur);
}

/* ---------------------------------------------------------------- MMU */

#define FSC_TRANS(l) (0x04 | (l))
#define FSC_ACCESS(l) (0x08 | (l))
#define FSC_PERM(l) (0x0c | (l))

static uint64_t phys_rd64(a64_cpu *c, uint64_t pa)
{
    uint8_t *p = space_ram_ptr(c->mem, pa, 8, false);
    if (p)
        return ld_le(p, 8);
    return space_read(c->mem, pa, 8);
}

/* Retorna 0 e preenche pa/perm (bit0 R, bit1 W, bit2 X), ou codigo FSC | (nivel). */
static int translate(a64_cpu *c, uint64_t va, int el, uint64_t *pa_out, int *perm_out)
{
    if (!(c->sctlr & 1)) {
        *pa_out = va & 0x0000ffffffffffffULL;
        *perm_out = 7;
        return 0;
    }
    uint64_t tcr = c->tcr;
    bool hi = (va >> 55) & 1;
    int tsz = hi ? (int)((tcr >> 16) & 63) : (int)(tcr & 63);
    bool tbi = hi ? (tcr >> 38) & 1 : (tcr >> 37) & 1;
    bool epd = hi ? (tcr >> 23) & 1 : (tcr >> 7) & 1;
    unsigned tg = hi ? (unsigned)((tcr >> 30) & 3) : (unsigned)((tcr >> 14) & 3);
    int gbits;
    if (hi)
        gbits = tg == 1 ? 14 : tg == 3 ? 16 : 12;
    else
        gbits = tg == 2 ? 14 : tg == 1 ? 16 : 12;
    if (tsz < 16)
        tsz = 16;
    if (tsz > 39)
        tsz = 39;
    int inputsize = 64 - tsz;
    int top = tbi ? 55 : 63;
    uint64_t topmask = (top == 63 ? ~0ULL : ((1ULL << (top + 1)) - 1)) & ~((1ULL << inputsize) - 1);
    if ((va & topmask) != (hi ? topmask : 0))
        return FSC_TRANS(0);
    if (epd)
        return FSC_TRANS(0);

    uint64_t table = (hi ? c->ttbr1 : c->ttbr0) & 0x0000fffffffffffeULL;
    int stride = gbits - 3;
    int levels = (inputsize - gbits + stride - 1) / stride;
    int start = 4 - levels;
    int level = start;
    unsigned apt = 0, xnt = 0, pxnt = 0;
    table &= ~0x3fULL;
    for (;;) {
        int lsb = gbits + (3 - level) * stride;
        int msb = level == start ? inputsize - 1 : lsb + stride - 1;
        uint64_t idx = (va >> lsb) & ((1ULL << (msb - lsb + 1)) - 1);
        uint64_t desc = phys_rd64(c, table + idx * 8);
        if (!(desc & 1))
            return FSC_TRANS(level);
        if (level < 3 && (desc & 2)) {
            apt |= (unsigned)(desc >> 61) & 3;
            xnt |= (unsigned)(desc >> 60) & 1;
            pxnt |= (unsigned)(desc >> 59) & 1;
            table = desc & 0x0000fffffffff000ULL & ~((1ULL << gbits) - 1);
            level++;
            continue;
        }
        if (level == 3 && !(desc & 2))
            return FSC_TRANS(3);
        if (level < 3) {
            if ((gbits == 12 && level == 0) || (gbits != 12 && level < 2))
                return FSC_TRANS(level);
        }
        uint64_t oamask = 0x0000ffffffffffffULL & ~((1ULL << lsb) - 1);
        uint64_t pa = (desc & oamask) | (va & ((1ULL << lsb) - 1));
        if (!(desc & (1u << 10)))
            return FSC_ACCESS(level);
        unsigned ap = (desc >> 6) & 3;
        bool el0_acc = (ap & 1) && !(apt & 1);
        bool ro = (ap & 2) || (apt & 2);
        bool uxn = ((desc >> 54) & 1) || xnt;
        bool pxn = ((desc >> 53) & 1) || pxnt;
        int perm;
        if (el == 0) {
            perm = el0_acc ? (1 | (ro ? 0 : 2) | (uxn ? 0 : 4)) : 0;
        } else {
            perm = 1 | (ro ? 0 : 2);
            if (!pxn && !(el0_acc && !ro))
                perm |= 4;
        }
        if ((c->sctlr >> 19) & 1) /* WXN */
            if (perm & 2)
                perm &= ~4;
        *pa_out = pa;
        *perm_out = perm;
        (void)level;
        return 0x100 | level; /* sucesso; nivel no byte baixo para faltas de permissao */
    }
}

static _Noreturn void mem_abort(a64_cpu *c, uint64_t va, int fsc, int acc)
{
    uint32_t ec, iss = (uint32_t)fsc;
    if (acc == ACC_EXEC) {
        ec = c->el == 0 ? 0x20 : 0x21;
    } else {
        ec = c->el == 0 ? 0x24 : 0x25;
        if (acc == ACC_WRITE)
            iss |= 1u << 6;
    }
    c->far = va;
    sync_exception(c, (ec << 26) | (1u << 25) | iss, c->cur);
}

static a64_tlbe *tlb_fill(a64_cpu *c, uint64_t va, int el, int acc)
{
    uint64_t pa;
    int perm;
    int r = translate(c, va, el, &pa, &perm);
    if (!(r & 0x100) && (c->sctlr & 1))
        mem_abort(c, va, r, acc);
    int need = acc == ACC_READ ? 1 : acc == ACC_WRITE ? 2 : 4;
    if (!(perm & need))
        mem_abort(c, va, FSC_PERM((c->sctlr & 1) ? (r & 0xff) : 0), acc);

    uint64_t page = va & ~0xfffULL, ppage = pa & ~0xfffULL;
    a64_tlbe *e = &c->tlb[el][(va >> 12) & (A64_TLB_SIZE - 1)];
    mvm_region *reg = space_find(c->mem, ppage);
    uint8_t *host = (reg && reg->host && ppage - reg->base + 0x1000 <= reg->size)
                        ? reg->host + (ppage - reg->base) : NULL;
    uint64_t io = host ? 0 : TLB_IO;
    e->tag_r = (perm & 1) ? page | io : TLB_INVALID;
    e->tag_w = (perm & 2) ? page | ((host && !reg->readonly && !reg->dirty_gen) ? 0 : TLB_IO)
                          : TLB_INVALID;
    e->tag_x = (perm & 4) ? page | io : TLB_INVALID;
    e->addend = host ? (uintptr_t)host - (uintptr_t)page : 0;
    e->pa = ppage;
    return e;
}

static inline bool tag_hit(uint64_t tag, uint64_t page) { return !(tag & TLB_INVALID) && (tag & ~0xfffULL) == page; }

uint64_t a64_read_slow(a64_cpu *c, uint64_t va, unsigned size, int el)
{
    if ((va & 0xfff) + size > 0x1000) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= a64_read_slow(c, va + i, 1, el) << (8 * i);
        return v;
    }
    uint64_t page = va & ~0xfffULL;
    a64_tlbe *e = &c->tlb[el][(va >> 12) & (A64_TLB_SIZE - 1)];
    if (!tag_hit(e->tag_r, page))
        e = tlb_fill(c, va, el, ACC_READ);
    if (!(e->tag_r & TLB_IO))
        return ld_le((const uint8_t *)(e->addend + va), size);
    return space_read(c->mem, e->pa | (va & 0xfff), size);
}

void a64_write_slow(a64_cpu *c, uint64_t va, uint64_t val, unsigned size, int el)
{
    if ((va & 0xfff) + size > 0x1000) {
        /* verifica as duas paginas antes de escrever */
        a64_tlbe *e2 = &c->tlb[el][((va + size - 1) >> 12) & (A64_TLB_SIZE - 1)];
        if (!tag_hit(e2->tag_w, (va + size - 1) & ~0xfffULL))
            tlb_fill(c, va + size - 1, el, ACC_WRITE);
        for (unsigned i = 0; i < size; i++)
            a64_write_slow(c, va + i, (val >> (8 * i)) & 0xff, 1, el);
        return;
    }
    uint64_t page = va & ~0xfffULL;
    a64_tlbe *e = &c->tlb[el][(va >> 12) & (A64_TLB_SIZE - 1)];
    if (!tag_hit(e->tag_w, page))
        e = tlb_fill(c, va, el, ACC_WRITE);
    if (!(e->tag_w & TLB_IO)) {
        st_le((uint8_t *)(e->addend + va), val, size);
        return;
    }
    space_write(c->mem, e->pa | (va & 0xfff), val, size);
}

static uint32_t fetch_slow(a64_cpu *c, uint64_t pc)
{
    uint64_t page = pc & ~0xfffULL;
    a64_tlbe *e = &c->tlb[c->el][(pc >> 12) & (A64_TLB_SIZE - 1)];
    if (!tag_hit(e->tag_x, page))
        e = tlb_fill(c, pc, c->el, ACC_EXEC);
    if (!(e->tag_x & TLB_IO)) {
        c->fetch_page = page;
        c->fetch_host = (uint8_t *)(e->addend + page);
        return (uint32_t)ld_le(c->fetch_host + (pc & 0xfff), 4);
    }
    return (uint32_t)space_read(c->mem, e->pa | (pc & 0xfff), 4);
}

static inline uint32_t fetch(a64_cpu *c, uint64_t pc)
{
    if (likely((pc & ~0xfffULL) == c->fetch_page))
        return (uint32_t)ld_le(c->fetch_host + (pc & 0xfff), 4);
    return fetch_slow(c, pc);
}

/* ------------------------------------------------------- aritmetica */

static inline uint64_t add_flags(a64_cpu *c, uint64_t a, uint64_t b, unsigned carry, bool sf, bool S)
{
    if (sf) {
        uint64_t r = a + b + carry;
        if (S) {
            unsigned __int128 w = (unsigned __int128)a + b + carry;
            uint32_t n = (r >> 63) & 1, z = r == 0, cc = (uint32_t)(w >> 64) & 1;
            uint32_t v = (uint32_t)((((a ^ r) & (b ^ r)) >> 63) & 1);
            c->nzcv = (n << 3) | (z << 2) | (cc << 1) | v;
        }
        return r;
    }
    uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
    uint32_t r = a32 + b32 + carry;
    if (S) {
        uint64_t w = (uint64_t)a32 + b32 + carry;
        uint32_t n = r >> 31, z = r == 0, cc = (uint32_t)(w >> 32) & 1;
        uint32_t v = (((a32 ^ r) & (b32 ^ r)) >> 31) & 1;
        c->nzcv = (n << 3) | (z << 2) | (cc << 1) | v;
    }
    return r;
}

static inline void logic_flags(a64_cpu *c, uint64_t r, bool sf)
{
    uint32_t n = sf ? (uint32_t)(r >> 63) : (uint32_t)(r >> 31) & 1;
    uint32_t z = sf ? r == 0 : (uint32_t)r == 0;
    c->nzcv = (n << 3) | (z << 2);
}

static inline uint64_t shift_reg(uint64_t v, unsigned type, unsigned amt, bool sf)
{
    if (!sf) {
        uint32_t x = (uint32_t)v;
        amt &= 31;
        switch (type) {
        case 0: return (uint32_t)(x << amt);
        case 1: return x >> amt;
        case 2: return (uint32_t)((int32_t)x >> amt);
        default: return amt ? (uint32_t)((x >> amt) | (x << (32 - amt))) : x;
        }
    }
    amt &= 63;
    switch (type) {
    case 0: return v << amt;
    case 1: return v >> amt;
    case 2: return (uint64_t)((int64_t)v >> amt);
    default: return amt ? (v >> amt) | (v << (64 - amt)) : v;
    }
}

static inline uint64_t extend_reg(uint64_t v, unsigned option, unsigned shift)
{
    switch (option) {
    case 0: v = (uint8_t)v; break;
    case 1: v = (uint16_t)v; break;
    case 2: v = (uint32_t)v; break;
    case 3: break;
    case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
    case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
    case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
    default: break;
    }
    return v << shift;
}

static bool decode_bitmasks(unsigned n, unsigned imms, unsigned immr, bool sf, uint64_t *out)
{
    unsigned combined = (n << 6) | (~imms & 0x3f);
    if (!combined)
        return false;
    int len = 31 - __builtin_clz(combined);
    if (len < 1)
        return false;
    unsigned esize = 1u << len;
    unsigned levels = esize - 1;
    unsigned s = imms & levels, r = immr & levels;
    if (s == levels)
        return false;
    if (!sf && n)
        return false;
    uint64_t welem = (s + 1 == 64) ? ~0ULL : ((1ULL << (s + 1)) - 1);
    uint64_t emask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
    if (r)
        welem = ((welem >> r) | (welem << (esize - r))) & emask;
    while (esize < 64) {
        welem |= welem << esize;
        esize *= 2;
    }
    *out = sf ? welem : (uint32_t)welem;
    return true;
}

static inline uint64_t mask64(unsigned bits) { return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1); }

/* -------------------------------------------- processamento c/ imediato */

static void dp_imm(a64_cpu *c, uint32_t insn)
{
    unsigned rd = insn & 31, rn = (insn >> 5) & 31;
    bool sf = insn >> 31;
    switch (BITS(insn, 25, 23)) {
    case 0: case 1: { /* ADR/ADRP */
        uint64_t imm = ((uint64_t)BITS(insn, 23, 5) << 2) | BITS(insn, 30, 29);
        imm = sext64(imm, 21);
        uint64_t v = (insn >> 31) ? (c->cur & ~0xfffULL) + (imm << 12) : c->cur + imm;
        setx(c, rd, v);
        return;
    }
    case 2: { /* ADD/SUB imediato */
        uint64_t imm = BITS(insn, 21, 10);
        if (BIT(insn, 22))
            imm <<= 12;
        bool sub = BIT(insn, 30), S = BIT(insn, 29);
        uint64_t a = xsp(c, rn);
        uint64_t r = sub ? add_flags(c, a, ~imm, 1, sf, S) : add_flags(c, a, imm, 0, sf, S);
        if (!sf)
            r = (uint32_t)r;
        if (S)
            setx(c, rd, r);
        else
            setxsp(c, rd, r);
        return;
    }
    case 4: { /* logico imediato */
        uint64_t imm;
        if (!decode_bitmasks(BIT(insn, 22), BITS(insn, 15, 10), BITS(insn, 21, 16), sf, &imm))
            a64_undef(c);
        uint64_t a = XR(rn), r;
        unsigned opc = BITS(insn, 30, 29);
        switch (opc) {
        case 0: r = a & imm; break;
        case 1: r = a | imm; break;
        case 2: r = a ^ imm; break;
        default: r = a & imm; break;
        }
        if (!sf)
            r = (uint32_t)r;
        if (opc == 3) {
            logic_flags(c, r, sf);
            setx(c, rd, r);
        } else {
            setxsp(c, rd, r);
        }
        return;
    }
    case 5: { /* move wide */
        unsigned opc = BITS(insn, 30, 29), hw = BITS(insn, 22, 21);
        if (opc == 1 || (!sf && hw > 1))
            a64_undef(c);
        unsigned sh = hw * 16;
        uint64_t imm = (uint64_t)BITS(insn, 20, 5) << sh, r;
        if (opc == 0)
            r = ~imm;
        else if (opc == 2)
            r = imm;
        else
            r = (XR(rd) & ~(0xffffULL << sh)) | imm;
        if (!sf)
            r = (uint32_t)r;
        setx(c, rd, r);
        return;
    }
    case 6: { /* bitfield */
        unsigned opc = BITS(insn, 30, 29), immr = BITS(insn, 21, 16), imms = BITS(insn, 15, 10);
        unsigned ds = sf ? 64 : 32;
        if (opc == 3 || BIT(insn, 22) != (unsigned)sf || immr >= ds || imms >= ds)
            a64_undef(c);
        uint64_t src = XR(rn), dst = XR(rd), r;
        if (imms >= immr) {
            unsigned w = imms - immr + 1;
            uint64_t f = (src >> immr) & mask64(w);
            if (opc == 0)
                r = sext64(f, w);
            else if (opc == 2)
                r = f;
            else
                r = (dst & ~mask64(w)) | f;
        } else {
            unsigned w = imms + 1, pos = ds - immr;
            uint64_t f = src & mask64(w);
            if (opc == 0)
                r = sext64(f, w) << pos;
            else if (opc == 2)
                r = f << pos;
            else
                r = (dst & ~(mask64(w) << pos)) | (f << pos);
        }
        if (!sf)
            r = (uint32_t)r;
        setx(c, rd, r);
        return;
    }
    case 7: { /* EXTR */
        unsigned rm = BITS(insn, 20, 16), lsb = BITS(insn, 15, 10);
        if (BIT(insn, 22) != (unsigned)sf || BITS(insn, 30, 29) || (!sf && lsb > 31))
            a64_undef(c);
        uint64_t r;
        if (sf)
            r = lsb ? (XR(rm) >> lsb) | (XR(rn) << (64 - lsb)) : XR(rm);
        else
            r = lsb ? (uint32_t)(((uint32_t)XR(rm) >> lsb) | ((uint32_t)XR(rn) << (32 - lsb)))
                    : (uint32_t)XR(rm);
        setx(c, rd, r);
        return;
    }
    default:
        a64_undef(c);
    }
}

/* ------------------------------------------ processamento c/ registrador */

static uint32_t crc32_update(uint32_t crc, uint64_t val, unsigned bytes, bool castagnoli)
{
    uint32_t poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (unsigned i = 0; i < bytes; i++) {
        crc ^= (uint8_t)(val >> (8 * i));
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
    }
    return crc;
}

static void dp_reg(a64_cpu *c, uint32_t insn)
{
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    bool sf = insn >> 31;
    if (!BIT(insn, 28)) {
        if (!BIT(insn, 24)) { /* logico com deslocamento */
            unsigned opc = BITS(insn, 30, 29), sh = BITS(insn, 23, 22), amt = BITS(insn, 15, 10);
            if (!sf && amt > 31)
                a64_undef(c);
            uint64_t b = shift_reg(XR(rm), sh, amt, sf);
            if (BIT(insn, 21))
                b = ~b;
            uint64_t a = XR(rn), r;
            switch (opc) {
            case 0: r = a & b; break;
            case 1: r = a | b; break;
            case 2: r = a ^ b; break;
            default: r = a & b; break;
            }
            if (!sf)
                r = (uint32_t)r;
            if (opc == 3)
                logic_flags(c, r, sf);
            setx(c, rd, r);
            return;
        }
        bool sub = BIT(insn, 30), S = BIT(insn, 29);
        uint64_t a, b;
        if (!BIT(insn, 21)) { /* add/sub com deslocamento */
            unsigned sh = BITS(insn, 23, 22), amt = BITS(insn, 15, 10);
            if (sh == 3 || (!sf && amt > 31))
                a64_undef(c);
            a = XR(rn);
            b = shift_reg(XR(rm), sh, amt, sf);
        } else { /* add/sub estendido */
            unsigned opt = BITS(insn, 15, 13), imm3 = BITS(insn, 12, 10);
            if (imm3 > 4 || BITS(insn, 23, 22))
                a64_undef(c);
            a = xsp(c, rn);
            b = extend_reg(XR(rm), opt, imm3);
        }
        uint64_t r = sub ? add_flags(c, a, ~b, 1, sf, S) : add_flags(c, a, b, 0, sf, S);
        if (!sf)
            r = (uint32_t)r;
        if (BIT(insn, 21) && !S)
            setxsp(c, rd, r);
        else
            setx(c, rd, r);
        return;
    }

    if (BIT(insn, 24)) { /* 3 fontes */
        unsigned op54 = BITS(insn, 30, 29), op31 = BITS(insn, 23, 21), o0 = BIT(insn, 15);
        unsigned ra = BITS(insn, 14, 10);
        uint64_t n = XR(rn), m = XR(rm), a = XR(ra), r;
        if (op54)
            a64_undef(c);
        switch (op31) {
        case 0:
            r = o0 ? a - n * m : a + n * m;
            if (!sf)
                r = (uint32_t)r;
            break;
        case 1: { /* SMADDL/SMSUBL */
            if (!sf) a64_undef(c);
            int64_t p = (int64_t)(int32_t)n * (int64_t)(int32_t)m;
            r = o0 ? a - (uint64_t)p : a + (uint64_t)p;
            break;
        }
        case 2: /* SMULH */
            if (!sf || o0) a64_undef(c);
            r = (uint64_t)(((__int128)(int64_t)n * (__int128)(int64_t)m) >> 64);
            break;
        case 5: { /* UMADDL/UMSUBL */
            if (!sf) a64_undef(c);
            uint64_t p = (uint64_t)(uint32_t)n * (uint32_t)m;
            r = o0 ? a - p : a + p;
            break;
        }
        case 6: /* UMULH */
            if (!sf || o0) a64_undef(c);
            r = (uint64_t)(((unsigned __int128)n * m) >> 64);
            break;
        default:
            a64_undef(c);
        }
        setx(c, rd, r);
        return;
    }

    switch (BITS(insn, 24, 21)) {
    case 0: { /* ADC/SBC */
        if (BITS(insn, 15, 10))
            a64_undef(c);
        bool sub = BIT(insn, 30), S = BIT(insn, 29);
        uint64_t b = sub ? ~XR(rm) : XR(rm);
        uint64_t r = add_flags(c, XR(rn), b, (c->nzcv >> 1) & 1, sf, S);
        setx(c, rd, sf ? r : (uint32_t)r);
        return;
    }
    case 2: { /* CCMN/CCMP */
        if (BIT(insn, 10) || BIT(insn, 4) || !BIT(insn, 29))
            a64_undef(c);
        unsigned cond = BITS(insn, 15, 12);
        if (arm_cond(cond, c->nzcv)) {
            uint64_t b = BIT(insn, 11) ? rm : XR(rm);
            if (BIT(insn, 30))
                add_flags(c, XR(rn), ~b, 1, sf, true);
            else
                add_flags(c, XR(rn), b, 0, sf, true);
        } else {
            c->nzcv = insn & 0xf;
        }
        return;
    }
    case 4: { /* CSEL/CSINC/CSINV/CSNEG */
        if (BIT(insn, 29) || BIT(insn, 11))
            a64_undef(c);
        unsigned cond = BITS(insn, 15, 12);
        uint64_t r;
        if (arm_cond(cond, c->nzcv)) {
            r = XR(rn);
        } else {
            r = XR(rm);
            unsigned op = (BIT(insn, 30) << 1) | BIT(insn, 10);
            if (op == 1) r = r + 1;
            else if (op == 2) r = ~r;
            else if (op == 3) r = 0 - r;
        }
        setx(c, rd, sf ? r : (uint32_t)r);
        return;
    }
    case 6: {
        if (BIT(insn, 30)) { /* 1 fonte */
            if (BITS(insn, 20, 16) || BIT(insn, 29))
                a64_undef(c);
            uint64_t a = XR(rn), r = 0;
            unsigned opc = BITS(insn, 15, 10);
            switch (opc) {
            case 0: /* RBIT */
                if (sf) {
                    r = 0;
                    for (int i = 0; i < 64; i++)
                        if (a & (1ULL << i)) r |= 1ULL << (63 - i);
                } else {
                    uint32_t v = 0;
                    for (int i = 0; i < 32; i++)
                        if (a & (1ULL << i)) v |= 1u << (31 - i);
                    r = v;
                }
                break;
            case 1: /* REV16 */
                r = ((a & 0x00ff00ff00ff00ffULL) << 8) | ((a >> 8) & 0x00ff00ff00ff00ffULL);
                if (!sf) r = (uint32_t)r;
                break;
            case 2: /* REV32 / REV (32) */
                if (sf)
                    r = ((uint64_t)bswap32((uint32_t)(a >> 32)) << 32) | bswap32((uint32_t)a);
                else
                    r = bswap32((uint32_t)a);
                break;
            case 3:
                if (!sf) a64_undef(c);
                r = bswap64(a);
                break;
            case 4: /* CLZ */
                if (sf) r = a ? (uint64_t)__builtin_clzll(a) : 64;
                else r = (uint32_t)a ? (uint64_t)__builtin_clz((uint32_t)a) : 32;
                break;
            case 5: /* CLS */
                if (sf) {
                    uint64_t x = a ^ (uint64_t)((int64_t)a >> 1);
                    x &= ~(1ULL << 63);
                    r = x ? (uint64_t)__builtin_clzll(x) - 1 : 63;
                } else {
                    uint32_t v = (uint32_t)a;
                    uint32_t x = (v ^ (uint32_t)((int32_t)v >> 1)) & 0x7fffffffu;
                    r = x ? (uint64_t)__builtin_clz(x) - 1 : 31;
                }
                break;
            default:
                a64_undef(c);
            }
            setx(c, rd, r);
            return;
        }
        /* 2 fontes */
        if (BIT(insn, 29))
            a64_undef(c);
        uint64_t a = XR(rn), b = XR(rm), r;
        unsigned opc = BITS(insn, 15, 10);
        switch (opc) {
        case 2: /* UDIV */
            if (sf) r = b ? a / b : 0;
            else r = (uint32_t)b ? (uint32_t)a / (uint32_t)b : 0;
            break;
        case 3: /* SDIV */
            if (sf) {
                int64_t x = (int64_t)a, y = (int64_t)b;
                r = y == 0 ? 0 : (x == INT64_MIN && y == -1) ? (uint64_t)x : (uint64_t)(x / y);
            } else {
                int32_t x = (int32_t)a, y = (int32_t)b;
                r = y == 0 ? 0 : (x == INT32_MIN && y == -1) ? (uint32_t)x : (uint32_t)(x / y);
            }
            break;
        case 8: case 9: case 10: case 11:
            r = shift_reg(a, opc - 8, (unsigned)(b & (sf ? 63 : 31)), sf);
            break;
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: {
            unsigned sz = opc & 3;
            if ((sz == 3) != sf)
                a64_undef(c);
            r = crc32_update((uint32_t)a, b, 1u << sz, opc & 4);
            break;
        }
        default:
            a64_undef(c);
        }
        setx(c, rd, sf ? r : (uint32_t)r);
        return;
    }
    default:
        a64_undef(c);
    }
}

/* ------------------------------------------------------ loads/stores */

static inline uint64_t ld_ext(a64_cpu *c, uint64_t addr, unsigned size, bool sign, bool to64, int el)
{
    uint64_t v = (el == c->el) ? a64_rd(c, addr, size) : a64_read_slow(c, addr, size, el);
    if (sign) {
        v = sext64(v, size * 8);
        if (!to64)
            v = (uint32_t)v;
    }
    return v;
}

static void ldst_exclusive(a64_cpu *c, uint32_t insn)
{
    unsigned size = BITS(insn, 31, 30), o2 = BIT(insn, 23), L = BIT(insn, 22), o1 = BIT(insn, 21);
    unsigned rs = BITS(insn, 20, 16), rt2 = BITS(insn, 14, 10), rn = BITS(insn, 9, 5), rt = insn & 31;
    unsigned bytes = 1u << size;
    uint64_t addr = xsp(c, rn);

    if (o2 && o1) { /* CAS */
        uint64_t cmp = XR(rs), newv = XR(rt);
        uint64_t m = mask64(bytes * 8);
        uint64_t old = a64_rd(c, addr, bytes);
        if (old == (cmp & m))
            a64_wr(c, addr, newv, bytes);
        setx(c, rs, old);
        return;
    }
    if (!o2 && o1 && size < 2) { /* CASP */
        if ((rs & 1) || (rt & 1))
            a64_undef(c);
        unsigned eb = size == 0 ? 4 : 8;
        uint64_t m = mask64(eb * 8);
        uint64_t o0v = a64_rd(c, addr, eb), o1v = a64_rd(c, addr + eb, eb);
        if (o0v == (XR(rs) & m) && o1v == (XR(rs + 1) & m)) {
            a64_wr(c, addr, XR(rt), eb);
            a64_wr(c, addr + eb, XR(rt + 1), eb);
        }
        setx(c, rs, o0v);
        setx(c, rs + 1, o1v);
        return;
    }
    if (o2) { /* LDAR/STLR (e variantes LO) */
        if (L)
            setx(c, rt, a64_rd(c, addr, bytes));
        else
            a64_wr(c, addr, XR(rt), bytes);
        return;
    }
    bool pair = o1;
    if (L) {
        if (pair) {
            unsigned eb = size == 2 ? 4 : 8;
            uint64_t a = a64_rd(c, addr, eb), b = a64_rd(c, addr + eb, eb);
            c->excl_val[0] = a;
            c->excl_val[1] = b;
            c->excl_size = (int)eb * 2;
            setx(c, rt, a);
            setx(c, rt2, b);
        } else {
            uint64_t v = a64_rd(c, addr, bytes);
            c->excl_val[0] = v;
            c->excl_size = (int)bytes;
            setx(c, rt, v);
        }
        c->excl_addr = addr;
        c->excl_valid = true;
        return;
    }
    /* STXR/STXP: Rs = status */
    uint32_t status = 1;
    if (c->excl_valid && c->excl_addr == addr) {
        if (pair) {
            unsigned eb = size == 2 ? 4 : 8;
            if (c->excl_size == (int)eb * 2) {
                a64_wr(c, addr, XR(rt), eb);
                a64_wr(c, addr + eb, XR(rt2), eb);
                status = 0;
            }
        } else if (c->excl_size == (int)bytes) {
            a64_wr(c, addr, XR(rt), bytes);
            status = 0;
        }
    }
    c->excl_valid = false;
    setx(c, rs, status);
}

static void ldst_atomic(a64_cpu *c, uint32_t insn)
{
    unsigned size = BITS(insn, 31, 30), o3 = BIT(insn, 15), opc = BITS(insn, 14, 12);
    unsigned rs = BITS(insn, 20, 16), rn = BITS(insn, 9, 5), rt = insn & 31;
    unsigned bytes = 1u << size, bits = bytes * 8;
    uint64_t addr = xsp(c, rn);
    if (o3) {
        if (opc == 0) { /* SWP */
            uint64_t old = a64_rd(c, addr, bytes);
            a64_wr(c, addr, XR(rs), bytes);
            setx(c, rt, old);
            return;
        }
        if (opc == 4) { /* LDAPR */
            setx(c, rt, a64_rd(c, addr, bytes));
            return;
        }
        a64_undef(c);
    }
    uint64_t old = a64_rd(c, addr, bytes), s = XR(rs) & mask64(bits), r;
    int64_t so = (int64_t)sext64(old, bits), ss = (int64_t)sext64(s, bits);
    switch (opc) {
    case 0: r = old + s; break;
    case 1: r = old & ~s; break;
    case 2: r = old ^ s; break;
    case 3: r = old | s; break;
    case 4: r = so > ss ? old : s; break;
    case 5: r = so < ss ? old : s; break;
    case 6: r = old > s ? old : s; break;
    default: r = old < s ? old : s; break;
    }
    a64_wr(c, addr, r, bytes);
    setx(c, rt, old);
}

static void ldst_reg(a64_cpu *c, uint32_t insn)
{
    unsigned size = BITS(insn, 31, 30), V = BIT(insn, 26), opc = BITS(insn, 23, 22);
    unsigned rn = BITS(insn, 9, 5), rt = insn & 31;
    uint64_t base = xsp(c, rn), addr, wb = 0;
    bool writeback = false, post = false;
    int el = c->el;
    unsigned scale = V ? (size | ((opc & 2) << 1)) : size;
    if (V && scale > 4)
        a64_undef(c);

    if (BIT(insn, 24)) {
        addr = base + ((uint64_t)BITS(insn, 21, 10) << scale);
    } else if (!BIT(insn, 21)) {
        uint64_t imm = sext64(BITS(insn, 20, 12), 9);
        switch (BITS(insn, 11, 10)) {
        case 0: addr = base + imm; break;
        case 1: addr = base; wb = base + imm; writeback = post = true; break;
        case 2:
            addr = base + imm;
            if (V)
                a64_undef(c);
            el = 0; /* LDTR/STTR */
            break;
        default: addr = base + imm; wb = addr; writeback = true; break;
        }
    } else {
        unsigned kind = BITS(insn, 11, 10);
        if (kind == 0) {
            if (V)
                a64_undef(c);
            ldst_atomic(c, insn);
            return;
        }
        if (kind != 2)
            a64_undef(c); /* LDRAA/LDRAB (PAuth) */
        unsigned rm = BITS(insn, 20, 16), opt = BITS(insn, 15, 13);
        if (!(opt & 2))
            a64_undef(c);
        addr = base + extend_reg(XR(rm), opt, BIT(insn, 12) ? scale : 0);
    }

    if (V) {
        if (!a64_fp_enabled(c))
            a64_fp_trap(c);
        unsigned bytes = 1u << scale;
        a64_vreg *v = &c->v[rt];
        if (opc & 1) {
            uint64_t lo, hi = 0;
            if (bytes == 16) {
                lo = a64_rd(c, addr, 8);
                hi = a64_rd(c, addr + 8, 8);
            } else {
                lo = a64_rd(c, addr, bytes);
            }
            v->d[0] = lo;
            v->d[1] = hi;
        } else {
            if (bytes == 16) {
                /* sonda as duas metades antes de escrever (atomicidade de falta) */
                a64_wr(c, addr + 8, v->d[1], 8);
                a64_wr(c, addr, v->d[0], 8);
            } else {
                a64_wr(c, addr, v->d[0], bytes);
            }
        }
    } else {
        unsigned bytes = 1u << size;
        switch (opc) {
        case 0: /* STR */
            if (el == c->el)
                a64_wr(c, addr, XR(rt), bytes);
            else
                a64_write_slow(c, addr, XR(rt), bytes, el);
            break;
        case 1: /* LDR */
            setx(c, rt, ld_ext(c, addr, bytes, false, true, el));
            break;
        case 2:
            if (size == 3) /* PRFM */
                break;
            setx(c, rt, ld_ext(c, addr, bytes, true, true, el));
            break;
        default:
            if (size >= 2)
                a64_undef(c);
            setx(c, rt, ld_ext(c, addr, bytes, true, false, el));
            break;
        }
    }
    if (writeback)
        setxsp(c, rn, post ? wb : addr);
}

static void ldst_pair(a64_cpu *c, uint32_t insn)
{
    unsigned opc = BITS(insn, 31, 30), V = BIT(insn, 26), mode = BITS(insn, 24, 23), L = BIT(insn, 22);
    unsigned rt2 = BITS(insn, 14, 10), rn = BITS(insn, 9, 5), rt = insn & 31;
    int64_t imm = (int64_t)sext64(BITS(insn, 21, 15), 7);
    unsigned scale;
    if (V) {
        if (opc == 3)
            a64_undef(c);
        scale = 2 + opc;
    } else {
        if (opc == 3 || (opc == 1 && !L))
            a64_undef(c);
        scale = 2 + (opc >> 1);
    }
    uint64_t base = xsp(c, rn);
    uint64_t off = (uint64_t)imm << scale;
    uint64_t addr = mode == 1 ? base : base + off;
    unsigned bytes = 1u << scale;

    if (V) {
        if (!a64_fp_enabled(c))
            a64_fp_trap(c);
        if (L) {
            uint64_t a0, a1 = 0, b0, b1 = 0;
            if (bytes == 16) {
                a0 = a64_rd(c, addr, 8); a1 = a64_rd(c, addr + 8, 8);
                b0 = a64_rd(c, addr + 16, 8); b1 = a64_rd(c, addr + 24, 8);
            } else {
                a0 = a64_rd(c, addr, bytes);
                b0 = a64_rd(c, addr + bytes, bytes);
            }
            c->v[rt].d[0] = a0; c->v[rt].d[1] = a1;
            c->v[rt2].d[0] = b0; c->v[rt2].d[1] = b1;
        } else {
            if (bytes == 16) {
                a64_wr(c, addr + 24, c->v[rt2].d[1], 8);
                a64_wr(c, addr, c->v[rt].d[0], 8);
                a64_wr(c, addr + 8, c->v[rt].d[1], 8);
                a64_wr(c, addr + 16, c->v[rt2].d[0], 8);
            } else {
                a64_wr(c, addr + bytes, c->v[rt2].d[0], bytes);
                a64_wr(c, addr, c->v[rt].d[0], bytes);
            }
        }
    } else {
        if (L) {
            uint64_t a = a64_rd(c, addr, bytes), b = a64_rd(c, addr + bytes, bytes);
            if (opc == 1) {
                a = sext64(a, 32);
                b = sext64(b, 32);
            }
            setx(c, rt, a);
            setx(c, rt2, b);
        } else {
            uint64_t a = XR(rt), b = XR(rt2);
            a64_wr(c, addr + bytes, b, bytes);
            a64_wr(c, addr, a, bytes);
        }
    }
    if (mode == 1)
        setxsp(c, rn, base + off);
    else if (mode == 3)
        setxsp(c, rn, addr);
}

static void ldst(a64_cpu *c, uint32_t insn)
{
    if ((insn & 0x3f000000) == 0x08000000) {
        ldst_exclusive(c, insn);
    } else if ((insn & 0x3b000000) == 0x18000000) { /* literal */
        unsigned opc = BITS(insn, 31, 30), rt = insn & 31;
        uint64_t addr = c->cur + (sext64(BITS(insn, 23, 5), 19) << 2);
        if (BIT(insn, 26)) {
            if (!a64_fp_enabled(c))
                a64_fp_trap(c);
            if (opc == 3)
                a64_undef(c);
            unsigned bytes = 4u << opc;
            c->v[rt].d[0] = bytes == 16 ? a64_rd(c, addr, 8) : a64_rd(c, addr, bytes);
            c->v[rt].d[1] = bytes == 16 ? a64_rd(c, addr + 8, 8) : 0;
        } else {
            if (opc == 0)
                setx(c, rt, a64_rd(c, addr, 4));
            else if (opc == 1)
                setx(c, rt, a64_rd(c, addr, 8));
            else if (opc == 2)
                setx(c, rt, sext64(a64_rd(c, addr, 4), 32));
            /* opc 3: PRFM literal */
        }
    } else if ((insn & 0x3a000000) == 0x28000000) {
        ldst_pair(c, insn);
    } else if ((insn & 0x3a000000) == 0x38000000) {
        ldst_reg(c, insn);
    } else if ((insn & 0xbe000000) == 0x0c000000) {
        if (!a64_fp_enabled(c))
            a64_fp_trap(c);
        a64_simd_ldst(c, insn);
    } else {
        a64_undef(c);
    }
}

/* ---------------------------------------------- registradores de sistema */

#define SR(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))

static void cache_zero(a64_cpu *c, uint64_t va)
{
    va &= ~63ULL;
    for (int i = 0; i < 64; i += 8)
        a64_wr(c, va + (uint64_t)i, 0, 8);
}

static void at_insn(a64_cpu *c, uint64_t va, int el, int acc)
{
    uint64_t pa;
    int perm;
    int r = translate(c, va, el, &pa, &perm);
    int need = acc == ACC_WRITE ? 2 : 1;
    if ((r & 0x100) || !(c->sctlr & 1)) {
        if (perm & need) {
            c->par = (pa & 0x0000fffffffff000ULL) | (0xffULL << 56);
            return;
        }
        r = FSC_PERM(r & 3);
    }
    c->par = 1 | ((uint64_t)(r & 0x3f) << 1);
}

static bool el0_sysreg_ok(a64_cpu *c, uint32_t key, bool write)
{
    switch (key) {
    case SR(3, 3, 4, 2, 0): /* NZCV */
    case SR(3, 3, 4, 4, 0): /* FPCR */
    case SR(3, 3, 4, 4, 1): /* FPSR */
    case SR(3, 3, 13, 0, 2): /* TPIDR_EL0 */
        return true;
    case SR(3, 3, 13, 0, 3): /* TPIDRRO_EL0 */
    case SR(3, 3, 0, 0, 7):  /* DCZID */
        return !write;
    case SR(3, 3, 0, 0, 1): /* CTR */
        return !write && ((c->sctlr >> 15) & 1);
    case SR(3, 3, 4, 2, 1): /* DAIF */
        return (c->sctlr >> 9) & 1;
    case SR(3, 3, 14, 0, 0): /* CNTFRQ */
        return !write && (c->gt.cntkctl & 3);
    case SR(3, 3, 14, 0, 1):
        return !write && (c->gt.cntkctl & 1);
    case SR(3, 3, 14, 0, 2):
        return !write && (c->gt.cntkctl & 2);
    case SR(3, 3, 14, 3, 0): case SR(3, 3, 14, 3, 1): case SR(3, 3, 14, 3, 2):
        return (c->gt.cntkctl >> 8) & 1;
    case SR(3, 3, 14, 2, 0): case SR(3, 3, 14, 2, 1): case SR(3, 3, 14, 2, 2):
        return (c->gt.cntkctl >> 9) & 1;
    default:
        return false;
    }
}

static uint64_t sysreg_read(a64_cpu *c, uint32_t key)
{
    switch (key) {
    case SR(3, 0, 0, 0, 0): return 0x410fd034; /* MIDR: Cortex-A53 */
    case SR(3, 0, 0, 0, 5): return 0x80000000; /* MPIDR */
    case SR(3, 0, 0, 0, 6): return 0;          /* REVIDR */
    case SR(3, 0, 0, 4, 0): return 0x11;       /* ID_AA64PFR0: EL0/EL1 AArch64, FP+AdvSIMD */
    case SR(3, 0, 0, 5, 0): return 0x6;        /* ID_AA64DFR0 */
    case SR(3, 0, 0, 6, 0): return 1ULL << 16; /* ID_AA64ISAR0: CRC32 */
    case SR(3, 0, 0, 7, 0): return 0x0f000025; /* ID_AA64MMFR0: 48 bits PA, ASID 16, sem 16K */
    case SR(3, 3, 0, 0, 1): return 0x8444c004; /* CTR_EL0 */
    case SR(3, 3, 0, 0, 7): return 4;          /* DCZID_EL0: blocos de 64 bytes */
    case SR(3, 1, 0, 0, 1): return 0x0a200023; /* CLIDR */
    case SR(3, 1, 0, 0, 0): return 0x700fe01a; /* CCSIDR */
    case SR(3, 2, 0, 0, 0): return c->csselr;
    case SR(3, 0, 1, 0, 0): return c->sctlr;
    case SR(3, 0, 1, 0, 1): return c->actlr;
    case SR(3, 0, 1, 0, 2): return c->cpacr;
    case SR(3, 0, 2, 0, 0): return c->ttbr0;
    case SR(3, 0, 2, 0, 1): return c->ttbr1;
    case SR(3, 0, 2, 0, 2): return c->tcr;
    case SR(3, 0, 4, 0, 0): return c->spsr;
    case SR(3, 0, 4, 0, 1): return c->elr;
    case SR(3, 0, 4, 1, 0): return c->sp[0];
    case SR(3, 0, 4, 2, 0): return (uint64_t)c->spsel;
    case SR(3, 0, 4, 2, 2): return (uint64_t)c->el << 2;
    case SR(3, 3, 4, 2, 0): return (uint64_t)c->nzcv << 28;
    case SR(3, 3, 4, 2, 1): return (uint64_t)c->daif << 6;
    case SR(3, 3, 4, 4, 0): return c->fpcr;
    case SR(3, 3, 4, 4, 1): return c->fpsr;
    case SR(3, 0, 5, 1, 0): return c->afsr0;
    case SR(3, 0, 5, 1, 1): return c->afsr1;
    case SR(3, 0, 5, 2, 0): return c->esr;
    case SR(3, 0, 6, 0, 0): return c->far;
    case SR(3, 0, 7, 4, 0): return c->par;
    case SR(3, 0, 10, 2, 0): return c->mair;
    case SR(3, 0, 10, 3, 0): return c->amair;
    case SR(3, 0, 12, 0, 0): return c->vbar;
    case SR(3, 0, 12, 1, 0): return (uint64_t)((c->irq_line ? 0x80 : 0) | (c->fiq_line ? 0x40 : 0));
    case SR(3, 0, 13, 0, 1): return c->contextidr;
    case SR(3, 0, 13, 0, 4): return c->tpidr1;
    case SR(3, 3, 13, 0, 2): return c->tpidr0;
    case SR(3, 3, 13, 0, 3): return c->tpidrro0;
    case SR(3, 0, 14, 1, 0): return c->gt.cntkctl;
    case SR(3, 3, 14, 0, 0): return GT_FREQ;
    case SR(3, 3, 14, 0, 1): return gt_cntpct(&c->gt);
    case SR(3, 3, 14, 0, 2): return gt_cntvct(&c->gt);
    case SR(3, 3, 14, 2, 0): return gt_tval_read(&c->gt, GT_PHYS);
    case SR(3, 3, 14, 2, 1): return gt_ctl_read(&c->gt, GT_PHYS);
    case SR(3, 3, 14, 2, 2): return c->gt.cval[GT_PHYS];
    case SR(3, 3, 14, 3, 0): return gt_tval_read(&c->gt, GT_VIRT);
    case SR(3, 3, 14, 3, 1): return gt_ctl_read(&c->gt, GT_VIRT);
    case SR(3, 3, 14, 3, 2): return c->gt.cval[GT_VIRT];
    case SR(2, 0, 0, 2, 2): return c->mdscr;
    case SR(2, 0, 1, 1, 4): return c->oslsr;
    case SR(3, 3, 9, 14, 0): return c->pmuserenr;
    default:
        if (key >> 14 == 2 || (key >> 14 == 3 && ((key >> 11) & 7) == 0 && ((key >> 7) & 15) == 0))
            return 0; /* debug e ID nao listados: RAZ */
        LOGD("a64: MRS desconhecido op0=%u op1=%u CRn=%u CRm=%u op2=%u", key >> 14, (key >> 11) & 7,
             (key >> 7) & 15, (key >> 3) & 15, key & 7);
        return 0;
    }
}

static void sysreg_write(a64_cpu *c, uint32_t key, uint64_t v)
{
    switch (key) {
    case SR(3, 0, 1, 0, 0):
        c->sctlr = v;
        tlb_flush(c);
        break;
    case SR(3, 0, 1, 0, 1): c->actlr = v; break;
    case SR(3, 0, 1, 0, 2): c->cpacr = v; break;
    case SR(3, 0, 2, 0, 0): c->ttbr0 = v; tlb_flush(c); break;
    case SR(3, 0, 2, 0, 1): c->ttbr1 = v; tlb_flush(c); break;
    case SR(3, 0, 2, 0, 2): c->tcr = v; tlb_flush(c); break;
    case SR(3, 2, 0, 0, 0): c->csselr = v; break;
    case SR(3, 0, 4, 0, 0): c->spsr = v; break;
    case SR(3, 0, 4, 0, 1): c->elr = v; break;
    case SR(3, 0, 4, 1, 0): c->sp[0] = v; break;
    case SR(3, 0, 4, 2, 0): c->spsel = v & 1; break;
    case SR(3, 3, 4, 2, 0): c->nzcv = (uint32_t)(v >> 28) & 0xf; break;
    case SR(3, 3, 4, 2, 1): c->daif = (uint32_t)(v >> 6) & 0xf; break;
    case SR(3, 3, 4, 4, 0): c->fpcr = (uint32_t)v; break;
    case SR(3, 3, 4, 4, 1): c->fpsr = (uint32_t)v; break;
    case SR(3, 0, 5, 1, 0): c->afsr0 = v; break;
    case SR(3, 0, 5, 1, 1): c->afsr1 = v; break;
    case SR(3, 0, 5, 2, 0): c->esr = v; break;
    case SR(3, 0, 6, 0, 0): c->far = v; break;
    case SR(3, 0, 7, 4, 0): c->par = v; break;
    case SR(3, 0, 10, 2, 0): c->mair = v; break;
    case SR(3, 0, 10, 3, 0): c->amair = v; break;
    case SR(3, 0, 12, 0, 0): c->vbar = v; break;
    case SR(3, 0, 13, 0, 1): c->contextidr = v; break;
    case SR(3, 0, 13, 0, 4): c->tpidr1 = v; break;
    case SR(3, 3, 13, 0, 2): c->tpidr0 = v; break;
    case SR(3, 3, 13, 0, 3): c->tpidrro0 = v; break;
    case SR(3, 0, 14, 1, 0): c->gt.cntkctl = (uint32_t)v; break;
    case SR(3, 3, 14, 2, 0): gt_tval_write(&c->gt, GT_PHYS, (uint32_t)v); break;
    case SR(3, 3, 14, 2, 1): gt_ctl_write(&c->gt, GT_PHYS, (uint32_t)v); break;
    case SR(3, 3, 14, 2, 2): gt_cval_write(&c->gt, GT_PHYS, v); break;
    case SR(3, 3, 14, 3, 0): gt_tval_write(&c->gt, GT_VIRT, (uint32_t)v); break;
    case SR(3, 3, 14, 3, 1): gt_ctl_write(&c->gt, GT_VIRT, (uint32_t)v); break;
    case SR(3, 3, 14, 3, 2): gt_cval_write(&c->gt, GT_VIRT, v); break;
    case SR(2, 0, 0, 2, 2): c->mdscr = v; break;
    case SR(2, 0, 1, 0, 4): c->oslsr = (c->oslsr & ~2ULL) | ((v & 1) << 1); break; /* OSLAR */
    case SR(3, 3, 9, 14, 0): c->pmuserenr = v; break;
    default:
        LOGD("a64: MSR ignorado op0=%u op1=%u CRn=%u CRm=%u op2=%u = 0x%llx", key >> 14,
             (key >> 11) & 7, (key >> 7) & 15, (key >> 3) & 15, key & 7, (unsigned long long)v);
        break;
    }
}

static void system_insn(a64_cpu *c, uint32_t insn)
{
    unsigned L = BIT(insn, 21), op0 = BITS(insn, 20, 19), op1 = BITS(insn, 18, 16);
    unsigned crn = BITS(insn, 15, 12), crm = BITS(insn, 11, 8), op2 = BITS(insn, 7, 5), rt = insn & 31;

    if (op0 == 0) {
        if (L)
            a64_undef(c);
        if (crn == 2 && op1 == 3) { /* hints */
            unsigned hint = (crm << 3) | op2;
            if (hint == 3) { /* WFI */
                if (!c->irq_line && !c->fiq_line)
                    c->halted = true;
            }
            return; /* NOP, YIELD, WFE, SEV, BTI, PAC*SP, ... */
        }
        if (crn == 3 && op1 == 3) { /* barreiras */
            if (op2 == 2)
                c->excl_valid = false; /* CLREX */
            else if (op2 == 6)
                c->fetch_page = 1; /* ISB */
            return;
        }
        if (crn == 4) { /* MSR (imediato) PSTATE */
            if (c->el == 0 && !(op1 == 3 && (op2 == 6 || op2 == 7) && ((c->sctlr >> 9) & 1)))
                a64_undef(c);
            if (op1 == 0 && op2 == 5) {
                c->spsel = crm & 1;
            } else if (op1 == 3 && op2 == 6) {
                c->daif |= crm;
            } else if (op1 == 3 && op2 == 7) {
                c->daif &= ~crm;
            }
            /* PAN/UAO/SSBS/DIT: ignorados */
            return;
        }
        a64_undef(c);
    }

    uint32_t key = SR(op0, op1, crn, crm, op2);
    if (op0 == 1) { /* SYS / SYSL */
        if (L)
            a64_undef(c);
        uint64_t v = XR(rt);
        if (crn == 7) {
            if (op1 == 3 && crm == 4 && op2 == 1) { /* DC ZVA */
                cache_zero(c, v);
                return;
            }
            if (crm == 8 && op1 == 0) {
                if (c->el == 0)
                    a64_undef(c);
                at_insn(c, v, op2 >= 2 ? 0 : 1, (op2 & 1) ? ACC_WRITE : ACC_READ);
                return;
            }
            if (c->el == 0 && op1 != 3)
                a64_undef(c);
            if (crm == 5 || crm == 1)
                c->fetch_page = 1; /* IC */
            return; /* DC/IC restantes: sem cache a manter */
        }
        if (crn == 8) { /* TLBI */
            if (c->el == 0)
                a64_undef(c);
            tlb_flush(c);
            return;
        }
        return;
    }

    if (c->el == 0 && !el0_sysreg_ok(c, key, !L))
        a64_undef(c);
    if (L)
        setx(c, rt, sysreg_read(c, key));
    else
        sysreg_write(c, key, XR(rt));
}

/* ---------------------------------------------------------- desvios */

static void do_eret(a64_cpu *c)
{
    uint64_t spsr = c->spsr;
    c->pc = c->elr;
    c->nzcv = (uint32_t)(spsr >> 28) & 0xf;
    c->daif = (uint32_t)(spsr >> 6) & 0xf;
    unsigned m = spsr & 0x1f;
    if (m & 0x10) {
        LOGW("a64: ERET para AArch32 nao suportado");
        m = 0;
    }
    int el = (m >> 2) & 3;
    if (el > 1)
        el = 1;
    c->el = el;
    c->spsel = m & 1;
    c->excl_valid = false;
    c->fetch_page = 1;
}

static void branch_sys(a64_cpu *c, uint32_t insn)
{
    uint32_t top = insn >> 25;
    if ((insn & 0x7c000000) == 0x14000000) { /* B / BL */
        uint64_t off = sext64(insn & 0x3ffffff, 26) << 2;
        if (insn >> 31)
            c->x[30] = c->cur + 4;
        c->pc = c->cur + off;
        return;
    }
    if ((insn & 0x7e000000) == 0x34000000) { /* CBZ/CBNZ */
        bool sf = insn >> 31;
        uint64_t v = XR(insn & 31);
        if (!sf)
            v = (uint32_t)v;
        if ((v == 0) != (bool)BIT(insn, 24))
            c->pc = c->cur + (sext64(BITS(insn, 23, 5), 19) << 2);
        return;
    }
    if ((insn & 0x7e000000) == 0x36000000) { /* TBZ/TBNZ */
        unsigned bit = (BIT(insn, 31) << 5) | BITS(insn, 23, 19);
        bool set = (XR(insn & 31) >> bit) & 1;
        if (set == (bool)BIT(insn, 24))
            c->pc = c->cur + (sext64(BITS(insn, 18, 5), 14) << 2);
        return;
    }
    if ((insn & 0xfe000000) == 0x54000000) { /* B.cond */
        if (BIT(insn, 4))
            a64_undef(c);
        if (arm_cond(insn & 0xf, c->nzcv))
            c->pc = c->cur + (sext64(BITS(insn, 23, 5), 19) << 2);
        return;
    }
    if ((insn & 0xff000000) == 0xd4000000) { /* excecoes */
        unsigned opc = BITS(insn, 23, 21), ll = insn & 3;
        uint32_t imm16 = BITS(insn, 20, 5);
        if (opc == 0 && ll == 1) { /* SVC */
            sync_exception(c, (0x15u << 26) | (1u << 25) | imm16, c->pc);
        }
        if (opc == 0 && (ll == 2 || ll == 3)) { /* HVC/SMC -> PSCI */
            if (c->el == 0)
                a64_undef(c);
            bool handled;
            int64_t r = arm_psci_call(c->vm, (uint32_t)c->x[0], c->x[1], &handled);
            if (!handled)
                LOGD("a64: chamada %s 0x%llx desconhecida", ll == 2 ? "HVC" : "SMC",
                     (unsigned long long)c->x[0]);
            c->x[0] = (uint64_t)r;
            return;
        }
        if (opc == 1) /* BRK */
            sync_exception(c, (0x3cu << 26) | (1u << 25) | imm16, c->cur);
        if (opc == 2) { /* HLT: semihosting nao suportado */
            a64_undef(c);
        }
        a64_undef(c);
    }
    if ((insn & 0xffc00000) == 0xd5000000) {
        system_insn(c, insn);
        return;
    }
    if (top == 0x6b) { /* branch por registrador */
        unsigned opc = BITS(insn, 24, 21), op2 = BITS(insn, 20, 16), rn = BITS(insn, 9, 5);
        if (op2 != 31)
            a64_undef(c);
        uint64_t target = XR(rn);
        switch (opc) {
        case 0: /* BR (e BRAA*) */
        case 8:
            c->pc = target;
            return;
        case 1: /* BLR */
        case 9:
            c->x[30] = c->cur + 4;
            c->pc = target;
            return;
        case 2: /* RET */
            c->pc = target;
            return;
        case 4: /* ERET */
            if (c->el == 0)
                a64_undef(c);
            do_eret(c);
            return;
        default:
            a64_undef(c);
        }
    }
    a64_undef(c);
}

/* ---------------------------------------------------------------- run */

static void a64_exec(a64_cpu *c, uint32_t insn)
{
    switch ((insn >> 25) & 0xf) {
    case 0x8: case 0x9:
        dp_imm(c, insn);
        break;
    case 0xa: case 0xb:
        branch_sys(c, insn);
        break;
    case 0x4: case 0x6: case 0xc: case 0xe:
        ldst(c, insn);
        break;
    case 0x5: case 0xd:
        dp_reg(c, insn);
        break;
    case 0x7: case 0xf:
        if (!a64_fp_enabled(c))
            a64_fp_trap(c);
        a64_simd_fp(c, insn);
        break;
    default:
        a64_undef(c);
    }
}

static int64_t a64_run(void *opaque, int64_t budget)
{
    a64_cpu *c = opaque;
    volatile int64_t n = 0;
    if (c->halted) {
        if (!c->irq_line && !c->fiq_line)
            return 0;
        c->halted = false;
    }
    setjmp(c->jb);
    while (n < budget) {
        if (unlikely((c->irq_line && !(c->daif & 2)) || (c->fiq_line && !(c->daif & 1)))) {
            take_exception(c, (c->fiq_line && !(c->daif & 1)) ? 2 : 1, 0, c->pc);
        }
        if (unlikely(c->halted || atomic_load_explicit(&c->vm->cpu_exit, memory_order_relaxed)))
            break;
        uint64_t pc = c->pc;
        c->cur = pc;
        uint32_t insn = fetch(c, pc);
        c->pc = pc + 4;
        n++;
        a64_exec(c, insn);
    }
    return n;
}

static bool a64_halted(void *opaque)
{
    a64_cpu *c = opaque;
    return c->halted && !c->irq_line && !c->fiq_line;
}

static void a64_reset(void *opaque)
{
    a64_cpu *c = opaque;
    memset(c->x, 0, sizeof(c->x));
    memset(c->v, 0, sizeof(c->v));
    c->sp[0] = c->sp[1] = 0;
    c->pc = 0;
    c->nzcv = 0;
    c->daif = 0xf;
    c->el = 1;
    c->spsel = 1;
    c->fpcr = c->fpsr = 0;
    c->sctlr = 0x30d00800; /* RES1 bits, MMU desligada */
    c->tcr = c->ttbr0 = c->ttbr1 = c->mair = c->amair = c->vbar = 0;
    c->elr = c->spsr = c->esr = c->far = c->par = 0;
    c->tpidr0 = c->tpidrro0 = c->tpidr1 = c->contextidr = 0;
    c->cpacr = 0;
    c->mdscr = c->afsr0 = c->afsr1 = c->actlr = c->csselr = 0;
    c->oslsr = 0x8; /* OSLM */
    c->pmuserenr = 0;
    c->excl_valid = false;
    c->halted = false;
    gt_reset(&c->gt);
    tlb_flush(c);
}

static void a64_dump(void *opaque, FILE *f)
{
    a64_cpu *c = opaque;
    fprintf(f, "PC=%016llx EL%d SPSel=%d NZCV=%x DAIF=%x\n", (unsigned long long)c->cur, c->el,
            c->spsel, c->nzcv, c->daif);
    for (int i = 0; i < 31; i++)
        fprintf(f, "x%-2d=%016llx%s", i, (unsigned long long)c->x[i], (i % 4 == 3) ? "\n" : " ");
    fprintf(f, "\nSP_EL0=%016llx SP_EL1=%016llx\n", (unsigned long long)c->sp[0], (unsigned long long)c->sp[1]);
    fprintf(f, "SCTLR=%llx TCR=%llx TTBR0=%llx TTBR1=%llx VBAR=%llx\n", (unsigned long long)c->sctlr,
            (unsigned long long)c->tcr, (unsigned long long)c->ttbr0, (unsigned long long)c->ttbr1,
            (unsigned long long)c->vbar);
    fprintf(f, "ELR=%llx SPSR=%llx ESR=%llx FAR=%llx\n", (unsigned long long)c->elr,
            (unsigned long long)c->spsr, (unsigned long long)c->esr, (unsigned long long)c->far);
}

static void a64_destroy(void *opaque)
{
    a64_cpu *c = opaque;
    c->gt.irq = NULL; /* a maquina (GIC) ja pode ter sido liberada */
    gt_reset(&c->gt);
    free(c);
}

static void cpu_tlb_flush_cb(void *cpu) { tlb_flush((a64_cpu *)cpu); }

const mvm_cpu_ops arm64_cpu_ops = {
    .reset = a64_reset,
    .run = a64_run,
    .halted = a64_halted,
    .dump = a64_dump,
    .destroy = a64_destroy,
    .tlb_flush = cpu_tlb_flush_cb,
};

void *arm64_cpu_new(mvm_vm *vm, const arm_hooks *hooks)
{
    a64_cpu *c = calloc(1, sizeof(*c));
    c->vm = vm;
    c->mem = &vm->mem;
    c->hooks = *hooks;
    gt_init(&c->gt, vm);
    c->gt.irq = hooks->timer_irq;
    c->gt.opaque = hooks->opaque;
    a64_reset(c);
    return c;
}

void arm64_set_irq(void *opaque, int line, int level)
{
    a64_cpu *c = opaque;
    if (line == 0)
        c->irq_line = level;
    else
        c->fiq_line = level;
}

void arm64_set_entry(void *opaque, uint64_t pc, uint64_t x0)
{
    a64_cpu *c = opaque;
    c->pc = pc;
    c->x[0] = x0;
    c->el = 1;
    c->spsel = 1;
    c->daif = 0xf;
}
