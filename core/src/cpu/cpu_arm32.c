/*
 * CPU ARMv7-A (AArch32), interpretador: conjuntos ARM e Thumb-2, VFPv3,
 * MMU com descritores curtos, CP15, timer generico. Sem extensoes de
 * seguranca/virtualizacao (HVC/SMC sao tratados como chamadas PSCI).
 */
#include "cpu_arm32.h"

#include <stdlib.h>

/* ------------------------------------------------------ modos e bancos */

static int bank_of(uint32_t mode)
{
    switch (mode) {
    case M_FIQ: return B_FIQ;
    case M_IRQ: return B_IRQ;
    case M_SVC: return B_SVC;
    case M_ABT: return B_ABT;
    case M_UND: return B_UND;
    default: return B_USR;
    }
}

static bool valid_mode(uint32_t m)
{
    return m == M_USR || m == M_FIQ || m == M_IRQ || m == M_SVC || m == M_ABT || m == M_UND || m == M_SYS;
}

static void switch_mode(a32_cpu *c, uint32_t nm)
{
    uint32_t om = c->mode;
    if (om == nm)
        return;
    int ob = bank_of(om), nb = bank_of(nm);
    if (ob != nb) {
        c->bank_r13[ob] = c->r[13];
        c->bank_r14[ob] = c->r[14];
        if (om == M_FIQ) {
            memcpy(c->fiq_r8_12, &c->r[8], 20);
            memcpy(&c->r[8], c->usr_r8_12, 20);
        }
        if (nm == M_FIQ) {
            memcpy(c->usr_r8_12, &c->r[8], 20);
            memcpy(&c->r[8], c->fiq_r8_12, 20);
        }
        c->r[13] = c->bank_r13[nb];
        c->r[14] = c->bank_r14[nb];
    }
    c->mode = nm;
    c->fetch_page = 1;
}

uint32_t a32_get_cpsr(a32_cpu *c)
{
    return (c->nzcv << 28) | ((uint32_t)c->q << 27) | ((c->it & 3) << 25) | (c->ge << 16) |
           (((c->it >> 2) & 0x3f) << 10) | ((uint32_t)c->big_endian << 9) | ((uint32_t)c->abt_dis << 8) |
           ((uint32_t)c->irq_dis << 7) | ((uint32_t)c->fiq_dis << 6) | ((uint32_t)c->thumb << 5) | c->mode;
}

/* escrita completa (retorno de excecao) */
static void set_cpsr_full(a32_cpu *c, uint32_t v)
{
    c->nzcv = v >> 28;
    c->q = (v >> 27) & 1;
    c->it = ((v >> 25) & 3) | (((v >> 10) & 0x3f) << 2);
    c->ge = (v >> 16) & 0xf;
    c->big_endian = (v >> 9) & 1;
    c->abt_dis = (v >> 8) & 1;
    c->irq_dis = (v >> 7) & 1;
    c->fiq_dis = (v >> 6) & 1;
    c->thumb = (v >> 5) & 1;
    uint32_t m = v & 0x1f;
    if (valid_mode(m))
        switch_mode(c, m);
}

void a32_set_cpsr(a32_cpu *c, uint32_t v, uint32_t mask) { (void)mask; set_cpsr_full(c, v); }

static uint32_t *cur_spsr(a32_cpu *c)
{
    int b = bank_of(c->mode);
    return b == B_USR ? NULL : &c->bank_spsr[b];
}

bool a32_msr(a32_cpu *c, bool spsr, uint32_t mask, uint32_t val)
{
    uint32_t bm = 0;
    if (mask & 8) bm |= 0xff000000;
    if (mask & 4) bm |= 0x00ff0000;
    if (mask & 2) bm |= 0x0000ff00;
    if (mask & 1) bm |= 0x000000ff;
    if (spsr) {
        uint32_t *s = cur_spsr(c);
        if (s)
            *s = (*s & ~bm) | (val & bm);
        return true;
    }
    if (mask & 8) {
        c->nzcv = val >> 28;
        c->q = (val >> 27) & 1;
    }
    if (mask & 4)
        c->ge = (val >> 16) & 0xf;
    if (!a32_priv(c))
        return true;
    if (mask & 2) {
        c->big_endian = (val >> 9) & 1;
        c->abt_dis = (val >> 8) & 1;
    }
    if (mask & 1) {
        c->irq_dis = (val >> 7) & 1;
        c->fiq_dis = (val >> 6) & 1;
        uint32_t m = val & 0x1f;
        if (valid_mode(m))
            switch_mode(c, m);
    }
    return true;
}

uint32_t a32_mrs(a32_cpu *c, bool spsr)
{
    if (spsr) {
        uint32_t *s = cur_spsr(c);
        return s ? *s : 0;
    }
    return a32_get_cpsr(c) & ~0x06000000u & ~0x0000fc00u & ~0x20u;
}

void a32_cps(a32_cpu *c, unsigned imod, bool m, unsigned aif, unsigned mode)
{
    if (!a32_priv(c))
        return;
    if (imod & 2) {
        bool dis = imod & 1;
        if (aif & 4) c->abt_dis = dis;
        if (aif & 2) c->irq_dis = dis;
        if (aif & 1) c->fiq_dis = dis;
    }
    if (m && valid_mode(mode))
        switch_mode(c, mode);
}

void a32_write_pc_bx(a32_cpu *c, uint32_t v)
{
    if (v & 1) {
        c->thumb = true;
        c->npc = v & ~1u;
    } else {
        c->thumb = false;
        c->npc = v & ~3u;
    }
}

void a32_write_pc_alu(a32_cpu *c, uint32_t v)
{
    if (!c->thumb)
        a32_write_pc_bx(c, v);
    else
        c->npc = v & ~1u;
}

/* ------------------------------------------------------------ TLB/MMU */

static void tlb_flush(a32_cpu *c)
{
    for (int p = 0; p < 2; p++)
        for (unsigned i = 0; i < A32_TLB_SIZE; i++)
            c->tlb[p][i].tag_r = c->tlb[p][i].tag_w = c->tlb[p][i].tag_x = A32_TLB_INVALID;
    c->fetch_page = 1;
}

static uint32_t phys_rd32(a32_cpu *c, uint32_t pa)
{
    uint8_t *p = space_ram_ptr(c->mem, pa, 4, false);
    return p ? (uint32_t)ld_le(p, 4) : (uint32_t)space_read(c->mem, pa, 4);
}

/* Retorna 0 em sucesso; senao FSR (FS + dominio em 7:4). */
static uint32_t translate(a32_cpu *c, uint32_t va, int priv, uint32_t *pa_out, int *perm_out)
{
    if (!(c->sctlr & 1)) {
        *pa_out = va;
        *perm_out = 7;
        return 0;
    }
    uint32_t n = c->ttbcr & 7, table;
    if (n && (va >> (32 - n)))
        table = c->ttbr1 & 0xffffc000u;
    else
        table = c->ttbr0 & (0xffffc000u >> n);
    uint32_t d1 = phys_rd32(c, table | ((va >> 20) << 2));
    uint32_t type = d1 & 3;
    uint32_t dom = (d1 >> 5) & 15;
    uint32_t pa, ap, xn = 0, pxn = 0;
    bool page;
    if (type == 0)
        return 0x5 | 0x10000;
    if (type == 1) {
        pxn = (d1 >> 2) & 1;
        uint32_t d2 = phys_rd32(c, (d1 & 0xfffffc00u) | (((va >> 12) & 0xff) << 2));
        if (!(d2 & 3))
            return 0x7 | (dom << 4) | 0x10000;
        if ((d2 & 3) == 1) {
            pa = (d2 & 0xffff0000u) | (va & 0xffff);
            xn = (d2 >> 15) & 1;
        } else {
            pa = (d2 & 0xfffff000u) | (va & 0xfff);
            xn = d2 & 1;
        }
        ap = ((d2 >> 4) & 3) | (((d2 >> 9) & 1) << 2);
        page = true;
    } else {
        if ((d1 >> 18) & 1) {
            pa = (d1 & 0xff000000u) | (va & 0x00ffffff);
            dom = 0;
        } else {
            pa = (d1 & 0xfff00000u) | (va & 0x000fffff);
        }
        xn = (d1 >> 4) & 1;
        pxn = d1 & 1 && type == 3 ? 1 : 0;
        ap = ((d1 >> 10) & 3) | (((d1 >> 15) & 1) << 2);
        page = false;
    }
    uint32_t dac = (c->dacr >> (2 * dom)) & 3;
    if (dac == 0 || dac == 2)
        return (page ? 0xb : 0x9) | (dom << 4) | 0x10000;
    int perm;
    if (dac == 3) {
        perm = 7;
    } else {
        if ((c->sctlr >> 29) & 1) { /* AFE: AP[0] e o flag de acesso */
            if (!(ap & 1))
                return (page ? 0x6 : 0x3) | (dom << 4) | 0x10000;
            unsigned a21 = ap >> 1;
            bool r, w;
            if (priv) {
                r = true;
                w = !(a21 & 2);
            } else {
                r = a21 & 1;
                w = a21 == 1;
            }
            perm = (r ? 1 : 0) | (w ? 2 : 0);
        } else {
            static const uint8_t pr[8] = {0, 3, 3, 3, 0, 1, 1, 1};
            static const uint8_t us[8] = {0, 0, 1, 3, 0, 0, 1, 1};
            perm = priv ? pr[ap] : us[ap];
        }
        if ((perm & 1) && !xn && !(priv && pxn))
            perm |= 4;
    }
    *pa_out = pa;
    *perm_out = perm | ((page ? 1 : 0) << 8) | ((int)dom << 4 << 8);
    return 0;
}

static void exception(a32_cpu *c, uint32_t off, uint32_t mode, uint32_t lr)
{
    uint32_t cpsr = a32_get_cpsr(c);
    switch_mode(c, mode);
    c->bank_spsr[bank_of(mode)] = cpsr;
    c->r[14] = lr;
    c->irq_dis = true;
    if (mode == M_FIQ)
        c->fiq_dis = true;
    if (mode == M_ABT || mode == M_IRQ || mode == M_FIQ)
        c->abt_dis = true;
    c->thumb = (c->sctlr >> 30) & 1;
    c->it = 0;
    uint32_t base = (c->sctlr & (1u << 13)) ? 0xffff0000u : c->vbar;
    c->npc = base + off;
    c->excl_valid = false;
    c->fetch_page = 1;
    c->halted = false;
}

static _Noreturn void raise_exc(a32_cpu *c, uint32_t off, uint32_t mode, uint32_t lr)
{
    exception(c, off, mode, lr);
    longjmp(c->jb, 1);
}

_Noreturn void a32_undef(a32_cpu *c)
{
    LOGD("a32: indefinida em 0x%08x (%s) insn=%08x", c->cur, c->thumb ? "thumb" : "arm", c->insn);
    /* LR_und = instrucao + 4 (ARM) ou + 2 (Thumb, inclusive para instrucoes de 32 bits) */
    raise_exc(c, 0x04, M_UND, c->cur + (c->thumb ? 2 : 4));
}

void a32_svc(a32_cpu *c) { exception(c, 0x08, M_SVC, c->npc); }

void a32_bkpt(a32_cpu *c)
{
    c->ifsr = 0x2; /* evento de debug */
    raise_exc(c, 0x0c, M_ABT, c->cur + 4);
}

void a32_exc_return(a32_cpu *c, uint32_t pc)
{
    uint32_t *s = cur_spsr(c);
    if (s)
        set_cpsr_full(c, *s);
    c->npc = c->thumb ? pc & ~1u : pc & ~3u;
    c->excl_valid = false;
    c->fetch_page = 1;
}

static _Noreturn void mem_abort(a32_cpu *c, uint32_t va, uint32_t fsr, int acc)
{
    uint32_t fs = fsr & 0xf, dom = (fsr >> 4) & 0xf;
    uint32_t v = fs | (dom << 4); /* nenhuma das nossas faltas usa FS[4] */
    if (acc == 2) {
        c->ifsr = v & ~0xf0u;
        c->ifar = va;
        raise_exc(c, 0x0c, M_ABT, c->cur + 4);
    }
    if (acc == 1)
        v |= 1u << 11;
    c->dfsr = v;
    c->dfar = va;
    raise_exc(c, 0x10, M_ABT, c->cur + 8);
}

static a32_tlbe *tlb_fill(a32_cpu *c, uint32_t va, int priv, int acc)
{
    uint32_t pa;
    int perm;
    uint32_t f = translate(c, va, priv, &pa, &perm);
    if (f)
        mem_abort(c, va, f & 0xffff, acc);
    int need = acc == 0 ? 1 : acc == 1 ? 2 : 4;
    if (!(perm & need)) {
        bool page = (perm >> 8) & 1;
        uint32_t dom = (uint32_t)(perm >> 12) & 15;
        mem_abort(c, va, (page ? 0xf : 0xd) | (dom << 4), acc);
    }
    uint32_t page = va & ~0xfffu, ppage = pa & ~0xfffu;
    a32_tlbe *e = &c->tlb[priv][(va >> 12) & (A32_TLB_SIZE - 1)];
    mvm_region *reg = space_find(c->mem, ppage);
    uint8_t *host = (reg && reg->host && ppage - reg->base + 0x1000 <= reg->size) ? reg->host + (ppage - reg->base) : NULL;
    uint32_t io = host ? 0 : A32_TLB_IO;
    e->tag_r = (perm & 1) ? page | io : A32_TLB_INVALID;
    e->tag_w = (perm & 2) ? page | ((host && !reg->readonly && !reg->dirty_gen) ? 0 : A32_TLB_IO) : A32_TLB_INVALID;
    e->tag_x = (perm & 4) ? page | io : A32_TLB_INVALID;
    e->addend = host ? (uintptr_t)host - (uintptr_t)page : 0;
    e->pa = ppage;
    return e;
}

static inline bool hit(uint32_t tag, uint32_t page) { return !(tag & A32_TLB_INVALID) && (tag & ~0xfffu) == page; }

uint32_t a32_read_slow(a32_cpu *c, uint32_t va, unsigned size, int priv)
{
    if ((va & 0xfff) + size > 0x1000) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= a32_read_slow(c, va + i, 1, priv) << (8 * i);
        return v;
    }
    a32_tlbe *e = &c->tlb[priv][(va >> 12) & (A32_TLB_SIZE - 1)];
    if (!hit(e->tag_r, va & ~0xfffu))
        e = tlb_fill(c, va, priv, 0);
    if (!(e->tag_r & A32_TLB_IO))
        return (uint32_t)ld_le((const uint8_t *)(e->addend + va), size);
    return (uint32_t)space_read(c->mem, e->pa | (va & 0xfff), size);
}

void a32_write_slow(a32_cpu *c, uint32_t va, uint32_t val, unsigned size, int priv)
{
    if ((va & 0xfff) + size > 0x1000) {
        uint32_t last = va + size - 1;
        a32_tlbe *e2 = &c->tlb[priv][(last >> 12) & (A32_TLB_SIZE - 1)];
        if (!hit(e2->tag_w, last & ~0xfffu))
            tlb_fill(c, last, priv, 1);
        for (unsigned i = 0; i < size; i++)
            a32_write_slow(c, va + i, (val >> (8 * i)) & 0xff, 1, priv);
        return;
    }
    a32_tlbe *e = &c->tlb[priv][(va >> 12) & (A32_TLB_SIZE - 1)];
    if (!hit(e->tag_w, va & ~0xfffu))
        e = tlb_fill(c, va, priv, 1);
    if (!(e->tag_w & A32_TLB_IO)) {
        st_le((uint8_t *)(e->addend + va), val, size);
        return;
    }
    space_write(c->mem, e->pa | (va & 0xfff), val, size);
}

static uint32_t fetch_slow(a32_cpu *c, uint32_t pc, unsigned size)
{
    int priv = a32_priv(c);
    if ((pc & 0xfff) + size > 0x1000) {
        /* instrucao Thumb-32 atravessando pagina */
        uint32_t lo = fetch_slow(c, pc, 2), hi = fetch_slow(c, pc + 2, 2);
        return lo | (hi << 16);
    }
    a32_tlbe *e = &c->tlb[priv][(pc >> 12) & (A32_TLB_SIZE - 1)];
    if (!hit(e->tag_x, pc & ~0xfffu))
        e = tlb_fill(c, pc, priv, 2);
    if (!(e->tag_x & A32_TLB_IO)) {
        c->fetch_page = pc & ~0xfffu;
        c->fetch_host = (uint8_t *)(e->addend + c->fetch_page);
        return (uint32_t)ld_le(c->fetch_host + (pc & 0xfff), size);
    }
    return (uint32_t)space_read(c->mem, e->pa | (pc & 0xfff), size);
}

uint32_t a32_fetch16(a32_cpu *c, uint32_t pc)
{
    if (likely((pc & ~0xfffu) == c->fetch_page))
        return (uint32_t)ld_le(c->fetch_host + (pc & 0xfff), 2);
    return fetch_slow(c, pc, 2);
}

static inline uint32_t fetch32(a32_cpu *c, uint32_t pc)
{
    if (likely((pc & ~0xfffu) == c->fetch_page))
        return (uint32_t)ld_le(c->fetch_host + (pc & 0xfff), 4);
    return fetch_slow(c, pc, 4);
}

/* ------------------------------------------------------------ operacoes */

static inline uint32_t add_c(uint32_t a, uint32_t b, uint32_t cin, uint32_t *cout, uint32_t *vout)
{
    uint64_t w = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)w;
    *cout = (uint32_t)(w >> 32);
    *vout = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}

uint32_t a32_dp(a32_cpu *c, unsigned opc, bool s, uint32_t a, uint32_t b, uint32_t sc, bool *write)
{
    uint32_t r, cf = sc, vf = c->nzcv & 1, cin = (c->nzcv >> 1) & 1;
    switch (opc) {
    case 0: case 8: r = a & b; break;
    case 1: case 9: r = a ^ b; break;
    case 2: case 10: r = add_c(a, ~b, 1, &cf, &vf); break;
    case 3: r = add_c(b, ~a, 1, &cf, &vf); break;
    case 4: case 11: r = add_c(a, b, 0, &cf, &vf); break;
    case 5: r = add_c(a, b, cin, &cf, &vf); break;
    case 6: r = add_c(a, ~b, cin, &cf, &vf); break;
    case 7: r = add_c(b, ~a, cin, &cf, &vf); break;
    case 12: r = a | b; break;
    case 13: r = b; break;
    case 14: r = a & ~b; break;
    default: r = ~b; break;
    }
    *write = !(opc >= 8 && opc <= 11);
    if (s)
        c->nzcv = ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2) | ((cf & 1) << 1) | (vf & 1);
    return r;
}

uint32_t a32_shift_imm_c(uint32_t v, unsigned type, unsigned imm5, uint32_t cin, uint32_t *cout)
{
    switch (type) {
    case 0:
        if (!imm5) { *cout = cin; return v; }
        *cout = (v >> (32 - imm5)) & 1;
        return v << imm5;
    case 1:
        if (!imm5) { *cout = v >> 31; return 0; }
        *cout = (v >> (imm5 - 1)) & 1;
        return v >> imm5;
    case 2:
        if (!imm5) { *cout = v >> 31; return (uint32_t)((int32_t)v >> 31); }
        *cout = (v >> (imm5 - 1)) & 1;
        return (uint32_t)((int32_t)v >> imm5);
    default:
        if (!imm5) { *cout = v & 1; return (cin << 31) | (v >> 1); }
        *cout = (v >> (imm5 - 1)) & 1;
        return ror32(v, imm5);
    }
}

uint32_t a32_shift_c(uint32_t v, unsigned type, unsigned amt, uint32_t cin, uint32_t *cout)
{
    amt &= 0xff;
    if (!amt) { *cout = cin; return v; }
    switch (type) {
    case 0:
        if (amt < 32) { *cout = (v >> (32 - amt)) & 1; return v << amt; }
        *cout = amt == 32 ? v & 1 : 0;
        return 0;
    case 1:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return v >> amt; }
        *cout = amt == 32 ? v >> 31 : 0;
        return 0;
    case 2:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *cout = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default:
        amt &= 31;
        if (!amt) { *cout = v >> 31; return v; }
        *cout = (v >> (amt - 1)) & 1;
        return ror32(v, amt);
    }
}

void a32_ldm_stm(a32_cpu *c, bool load, bool inc, bool before, unsigned rn, bool wb, uint32_t list, bool user)
{
    uint32_t base = c->r[rn];
    unsigned n = (unsigned)__builtin_popcount(list);
    uint32_t addr = inc ? base + (before ? 4 : 0) : base - 4 * n + (before ? 0 : 4);
    uint32_t nb = inc ? base + 4 * n : base - 4 * n;
    bool usr_bank = user && !(load && (list & 0x8000));
    bool banked_mode = c->mode != M_USR && c->mode != M_SYS;
    if (load) {
        uint32_t vals[16];
        for (int i = 0; i < 16; i++)
            if (list & (1u << i)) {
                vals[i] = a32_rd(c, addr, 4);
                addr += 4;
            }
        if (wb && !(list & (1u << rn)))
            c->r[rn] = nb;
        for (int i = 0; i < 15; i++) {
            if (!(list & (1u << i)))
                continue;
            if (usr_bank && i >= 13 && banked_mode) {
                if (i == 13) c->bank_r13[B_USR] = vals[i];
                else c->bank_r14[B_USR] = vals[i];
            } else if (usr_bank && i >= 8 && i <= 12 && c->mode == M_FIQ) {
                c->usr_r8_12[i - 8] = vals[i];
            } else {
                c->r[i] = vals[i];
            }
        }
        if (list & 0x8000) {
            if (user)
                a32_exc_return(c, vals[15]);
            else
                a32_write_pc_bx(c, vals[15]);
        }
    } else {
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i)))
                continue;
            uint32_t v = i == 15 ? c->cur + (c->thumb ? 4 : 8) : c->r[i];
            if (usr_bank) {
                if (i >= 13 && i <= 14 && banked_mode)
                    v = i == 13 ? c->bank_r13[B_USR] : c->bank_r14[B_USR];
                else if (i >= 8 && i <= 12 && c->mode == M_FIQ)
                    v = c->usr_r8_12[i - 8];
            }
            a32_wr(c, addr, v, 4);
            addr += 4;
        }
        if (wb)
            c->r[rn] = nb;
    }
}

void a32_psci(a32_cpu *c)
{
    bool handled;
    int64_t r = arm_psci_call(c->vm, c->r[0], c->r[1], &handled);
    c->r[0] = (uint32_t)r;
}

void a32_hint(a32_cpu *c, unsigned hint)
{
    if (hint == 3 && !c->irq_line && !c->fiq_line) /* WFI */
        c->halted = true;
}

void a32_barrier(a32_cpu *c, unsigned op)
{
    if (op == 1)
        c->excl_valid = false; /* CLREX */
    else if (op == 6)
        c->fetch_page = 1; /* ISB */
}

/* --------------------------------------------------------------- CP15 */

#define CP(opc1, crn, crm, opc2) (((opc1) << 12) | ((crn) << 8) | ((crm) << 4) | (opc2))

static bool cp15_user_ok(a32_cpu *c, uint32_t key, bool read)
{
    switch (key) {
    case CP(0, 13, 0, 2): return true;
    case CP(0, 13, 0, 3): return read;
    case CP(0, 7, 5, 4): case CP(0, 7, 10, 4): case CP(0, 7, 10, 5): return !read;
    case CP(0, 14, 0, 0): return read && (c->gt.cntkctl & 3);
    case CP(0, 14, 3, 0): case CP(0, 14, 3, 1): return (c->gt.cntkctl >> 8) & 1;
    case CP(0, 14, 2, 0): case CP(0, 14, 2, 1): return (c->gt.cntkctl >> 9) & 1;
    case CP(0, 9, 14, 0): return read;
    default: return false;
    }
}

static void ats1(a32_cpu *c, uint32_t va, int priv, bool write)
{
    uint32_t pa;
    int perm;
    uint32_t f = translate(c, va, priv, &pa, &perm);
    if (!f && (perm & (write ? 2 : 1))) {
        c->par = pa & 0xfffff000u;
        return;
    }
    if (!f)
        f = ((perm >> 8) & 1) ? 0xf : 0xd;
    c->par = 1 | ((f & 0xf) << 1) | ((f & 0x10) << 2);
}

static uint32_t cp15_read(a32_cpu *c, uint32_t key)
{
    switch (key) {
    case CP(0, 0, 0, 0): return 0x412fc0f1;  /* MIDR: Cortex-A15 */
    case CP(0, 0, 0, 1): return 0x8444c004;  /* CTR */
    case CP(0, 0, 0, 2): return 0;           /* TCMTR */
    case CP(0, 0, 0, 3): return 0;           /* TLBTR */
    case CP(0, 0, 0, 5): return 0x80000000;  /* MPIDR */
    case CP(0, 0, 0, 6): return 0;           /* REVIDR */
    case CP(0, 0, 1, 0): return 0x00000031;  /* ID_PFR0: ARM + Thumb-2 */
    case CP(0, 0, 1, 1): return 0x00010001;  /* ID_PFR1: timer generico */
    case CP(0, 0, 1, 2): return 0;           /* ID_DFR0 */
    case CP(0, 0, 1, 3): return 0;
    case CP(0, 0, 1, 4): return 0x10101103;  /* ID_MMFR0: VMSAv7 */
    case CP(0, 0, 1, 5): return 0x40000000;
    case CP(0, 0, 1, 6): return 0x01240000;
    case CP(0, 0, 1, 7): return 0x02102211;
    case CP(0, 0, 2, 0): return 0x02101110;  /* ID_ISAR0: SDIV/UDIV em ARM e Thumb */
    case CP(0, 0, 2, 1): return 0x13112111;
    case CP(0, 0, 2, 2): return 0x21232041;
    case CP(0, 0, 2, 3): return 0x11112131;
    case CP(0, 0, 2, 4): return 0x10011142;
    case CP(0, 0, 2, 5): return 0;
    case CP(1, 0, 0, 0): return 0x701fe00a;  /* CCSIDR */
    case CP(1, 0, 0, 1): return 0x0a200023;  /* CLIDR */
    case CP(1, 0, 0, 7): return 0;
    case CP(2, 0, 0, 0): return c->csselr;
    case CP(0, 1, 0, 0): return c->sctlr;
    case CP(0, 1, 0, 1): return c->actlr;
    case CP(0, 1, 0, 2): return c->cpacr;
    case CP(0, 2, 0, 0): return c->ttbr0;
    case CP(0, 2, 0, 1): return c->ttbr1;
    case CP(0, 2, 0, 2): return c->ttbcr;
    case CP(0, 3, 0, 0): return c->dacr;
    case CP(0, 5, 0, 0): return c->dfsr;
    case CP(0, 5, 0, 1): return c->ifsr;
    case CP(0, 5, 1, 0): return c->adfsr;
    case CP(0, 5, 1, 1): return c->aifsr;
    case CP(0, 6, 0, 0): return c->dfar;
    case CP(0, 6, 0, 2): return c->ifar;
    case CP(0, 7, 4, 0): return c->par;
    case CP(0, 9, 14, 0): return c->pmuserenr;
    case CP(0, 10, 2, 0): return c->prrr;
    case CP(0, 10, 2, 1): return c->nmrr;
    case CP(0, 12, 0, 0): return c->vbar;
    case CP(0, 12, 1, 0): return (c->irq_line ? 0x80 : 0) | (c->fiq_line ? 0x40 : 0);
    case CP(0, 13, 0, 0): return c->fcseidr;
    case CP(0, 13, 0, 1): return c->contextidr;
    case CP(0, 13, 0, 2): return c->tpidrurw;
    case CP(0, 13, 0, 3): return c->tpidruro;
    case CP(0, 13, 0, 4): return c->tpidrprw;
    case CP(0, 14, 0, 0): return (uint32_t)GT_FREQ;
    case CP(0, 14, 1, 0): return c->gt.cntkctl;
    case CP(0, 14, 2, 0): return gt_tval_read(&c->gt, GT_PHYS);
    case CP(0, 14, 2, 1): return gt_ctl_read(&c->gt, GT_PHYS);
    case CP(0, 14, 3, 0): return gt_tval_read(&c->gt, GT_VIRT);
    case CP(0, 14, 3, 1): return gt_ctl_read(&c->gt, GT_VIRT);
    default:
        LOGD("a32: MRC p15 desconhecido %x", key);
        return 0;
    }
}

static void cp15_write(a32_cpu *c, uint32_t key, uint32_t v)
{
    switch (key) {
    case CP(2, 0, 0, 0): c->csselr = v; break;
    case CP(0, 1, 0, 0): c->sctlr = v; tlb_flush(c); break;
    case CP(0, 1, 0, 1): c->actlr = v; break;
    case CP(0, 1, 0, 2): c->cpacr = v & 0x00f00000u; break;
    case CP(0, 2, 0, 0): c->ttbr0 = v; tlb_flush(c); break;
    case CP(0, 2, 0, 1): c->ttbr1 = v; tlb_flush(c); break;
    case CP(0, 2, 0, 2): c->ttbcr = v & 7; tlb_flush(c); break;
    case CP(0, 3, 0, 0): c->dacr = v; tlb_flush(c); break;
    case CP(0, 5, 0, 0): c->dfsr = v; break;
    case CP(0, 5, 0, 1): c->ifsr = v; break;
    case CP(0, 5, 1, 0): c->adfsr = v; break;
    case CP(0, 5, 1, 1): c->aifsr = v; break;
    case CP(0, 6, 0, 0): c->dfar = v; break;
    case CP(0, 6, 0, 2): c->ifar = v; break;
    case CP(0, 7, 0, 4): a32_hint(c, 3); break; /* WFI legado */
    case CP(0, 7, 4, 0): c->par = v; break;
    case CP(0, 7, 5, 4): c->fetch_page = 1; break;       /* CP15ISB */
    case CP(0, 7, 8, 0): ats1(c, v, 1, false); break;
    case CP(0, 7, 8, 1): ats1(c, v, 1, true); break;
    case CP(0, 7, 8, 2): ats1(c, v, 0, false); break;
    case CP(0, 7, 8, 3): ats1(c, v, 0, true); break;
    case CP(0, 9, 14, 0): c->pmuserenr = v; break;
    case CP(0, 10, 2, 0): c->prrr = v; break;
    case CP(0, 10, 2, 1): c->nmrr = v; break;
    case CP(0, 12, 0, 0): c->vbar = v & ~0x1fu; break;
    case CP(0, 13, 0, 0): c->fcseidr = v; tlb_flush(c); break;
    case CP(0, 13, 0, 1): c->contextidr = v; break;
    case CP(0, 13, 0, 2): c->tpidrurw = v; break;
    case CP(0, 13, 0, 3): c->tpidruro = v; break;
    case CP(0, 13, 0, 4): c->tpidrprw = v; break;
    case CP(0, 14, 1, 0): c->gt.cntkctl = v; break;
    case CP(0, 14, 2, 0): gt_tval_write(&c->gt, GT_PHYS, v); break;
    case CP(0, 14, 2, 1): gt_ctl_write(&c->gt, GT_PHYS, v); break;
    case CP(0, 14, 3, 0): gt_tval_write(&c->gt, GT_VIRT, v); break;
    case CP(0, 14, 3, 1): gt_ctl_write(&c->gt, GT_VIRT, v); break;
    default:
        if ((key >> 8 & 15) == 8) { /* TLB */
            tlb_flush(c);
            break;
        }
        if ((key >> 8 & 15) == 7) /* cache: nada a fazer */
            break;
        LOGD("a32: MCR p15 ignorado %x = %x", key, v);
        break;
    }
}

void a32_coproc(a32_cpu *c, uint32_t insn)
{
    unsigned cp = BITS(insn, 11, 8);
    if (cp == 10 || cp == 11) {
        a32_vfp(c, insn);
        return;
    }
    unsigned op1 = BITS(insn, 25, 20);
    if (cp == 14) { /* debug: RAZ/WI */
        if (!a32_priv(c))
            a32_undef(c);
        if ((op1 & 0x31) == 0x21 && BIT(insn, 4)) { /* MRC */
            unsigned rt = BITS(insn, 15, 12);
            if (rt == 15)
                c->nzcv = 0;
            else
                c->r[rt] = 0;
        }
        return;
    }
    if (cp != 15)
        a32_undef(c);
    unsigned rt = BITS(insn, 15, 12);
    if ((op1 & 0x3e) == 0x04) { /* MCRR/MRRC */
        unsigned rt2 = BITS(insn, 19, 16), opc1 = BITS(insn, 7, 4), crm = BITS(insn, 3, 0);
        bool read = op1 & 1;
        if (crm == 14) {
            if (!a32_priv(c)) {
                bool ok = (opc1 == 0 && read && (c->gt.cntkctl & 1)) || (opc1 == 1 && read && (c->gt.cntkctl & 2)) ||
                          (opc1 == 3 && ((c->gt.cntkctl >> 8) & 1)) || (opc1 == 2 && ((c->gt.cntkctl >> 9) & 1));
                if (!ok)
                    a32_undef(c);
            }
            uint64_t v = 0;
            switch (opc1) {
            case 0: v = gt_cntpct(&c->gt); break;
            case 1: v = gt_cntvct(&c->gt); break;
            case 2: v = c->gt.cval[GT_PHYS]; break;
            case 3: v = c->gt.cval[GT_VIRT]; break;
            case 4: v = c->gt.cntvoff; break;
            default: a32_undef(c);
            }
            if (read) {
                c->r[rt] = (uint32_t)v;
                c->r[rt2] = (uint32_t)(v >> 32);
            } else {
                uint64_t nv = ((uint64_t)c->r[rt2] << 32) | c->r[rt];
                if (opc1 == 2) gt_cval_write(&c->gt, GT_PHYS, nv);
                else if (opc1 == 3) gt_cval_write(&c->gt, GT_VIRT, nv);
                else if (opc1 == 4) { c->gt.cntvoff = nv; gt_update(&c->gt, GT_VIRT); }
            }
            return;
        }
        if (!a32_priv(c))
            a32_undef(c);
        if (crm == 2) { /* TTBR0/1 de 64 bits (LPAE): usa a parte baixa */
            if (read) {
                c->r[rt] = opc1 ? c->ttbr1 : c->ttbr0;
                c->r[rt2] = 0;
            } else {
                if (opc1) c->ttbr1 = c->r[rt];
                else c->ttbr0 = c->r[rt];
                tlb_flush(c);
            }
            return;
        }
        if (read) {
            c->r[rt] = 0;
            c->r[rt2] = 0;
        }
        return;
    }
    if ((op1 & 0x30) == 0x20 && BIT(insn, 4)) { /* MCR/MRC */
        unsigned opc1 = BITS(insn, 23, 21), crn = BITS(insn, 19, 16), crm = BITS(insn, 3, 0), opc2 = BITS(insn, 7, 5);
        uint32_t key = CP(opc1, crn, crm, opc2);
        bool read = BIT(insn, 20);
        if (!a32_priv(c) && !cp15_user_ok(c, key, read))
            a32_undef(c);
        if (read) {
            uint32_t v = cp15_read(c, key);
            if (rt == 15)
                c->nzcv = v >> 28;
            else
                c->r[rt] = v;
        } else {
            cp15_write(c, key, rt == 15 ? c->r[15] : c->r[rt]);
        }
        return;
    }
    a32_undef(c);
}

/* ------------------------------------------------ media / multiplicacao */

uint32_t a32_sat_s(a32_cpu *c, int64_t v, unsigned bits)
{
    int64_t max = ((int64_t)1 << (bits - 1)) - 1, min = -((int64_t)1 << (bits - 1));
    if (v > max) { c->q = true; return (uint32_t)max; }
    if (v < min) { c->q = true; return (uint32_t)min; }
    return (uint32_t)v;
}

uint32_t a32_sat_u(a32_cpu *c, int64_t v, unsigned bits)
{
    int64_t max = bits >= 32 ? 0xffffffffLL : (((int64_t)1 << bits) - 1);
    if (v > max) { c->q = true; return (uint32_t)max; }
    if (v < 0) { c->q = true; return 0; }
    return (uint32_t)v;
}

#define sat_s a32_sat_s
#define sat_u a32_sat_u

uint32_t a32_satop(a32_cpu *c, unsigned op, int32_t a, int32_t b)
{
    /* 0 QADD, 1 QSUB, 2 QDADD, 3 QDSUB */
    if (op & 2)
        b = (int32_t)sat_s(c, (int64_t)b * 2, 32);
    return (op & 1) ? sat_s(c, (int64_t)a - b, 32) : sat_s(c, (int64_t)a + b, 32);
}

uint32_t a32_extend(unsigned op, uint32_t v, unsigned rot, uint32_t add)
{
    /* op: 0 SXTB16 2 SXTB 3 SXTH 4 UXTB16 6 UXTB 7 UXTH */
    v = ror32(v, rot * 8);
    switch (op) {
    case 0: {
        uint32_t lo = (uint32_t)(int32_t)(int8_t)v + add;
        uint32_t hi = (uint32_t)(int32_t)(int8_t)(v >> 16) + (add >> 16);
        return (lo & 0xffff) | (hi << 16);
    }
    case 2: return (uint32_t)(int32_t)(int8_t)v + add;
    case 3: return (uint32_t)(int32_t)(int16_t)v + add;
    case 4: {
        uint32_t lo = (v & 0xff) + add;
        uint32_t hi = ((v >> 16) & 0xff) + (add >> 16);
        return (lo & 0xffff) | (hi << 16);
    }
    case 6: return (v & 0xff) + add;
    default: return (v & 0xffff) + add;
    }
}

/* Soma/subtracao paralelas. pre: 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH. op: 0 ADD16 1 ASX 2 SAX 3 SUB16 4 ADD8 7 SUB8 */
uint32_t a32_parallel(a32_cpu *c, unsigned pre, unsigned op, uint32_t a, uint32_t b)
{
    bool uns = pre >= 5;
    unsigned kind = pre & 3; /* 1 normal (GE), 2 saturado, 3 metade */
    uint32_t r = 0, ge = 0;
    if (op >= 4) {
        for (int i = 0; i < 4; i++) {
            int32_t x = uns ? (int32_t)((a >> (8 * i)) & 0xff) : (int8_t)(a >> (8 * i));
            int32_t y = uns ? (int32_t)((b >> (8 * i)) & 0xff) : (int8_t)(b >> (8 * i));
            int32_t s = op == 4 ? x + y : x - y;
            uint32_t o;
            if (kind == 2) o = uns ? sat_u(c, s, 8) : sat_s(c, s, 8);
            else if (kind == 3) o = (uint32_t)(s >> 1);
            else {
                o = (uint32_t)s;
                if (uns ? (op == 4 ? s >= 0x100 : s >= 0) : s >= 0) ge |= 1u << i;
            }
            r |= (o & 0xff) << (8 * i);
        }
    } else {
        int32_t a0 = uns ? (int32_t)(a & 0xffff) : (int16_t)a, a1 = uns ? (int32_t)(a >> 16) : (int16_t)(a >> 16);
        int32_t b0 = uns ? (int32_t)(b & 0xffff) : (int16_t)b, b1 = uns ? (int32_t)(b >> 16) : (int16_t)(b >> 16);
        int32_t s0, s1;
        bool add0, add1;
        switch (op) {
        case 0: s0 = a0 + b0; s1 = a1 + b1; add0 = add1 = true; break;
        case 1: s0 = a0 - b1; s1 = a1 + b0; add0 = false; add1 = true; break;
        case 2: s0 = a0 + b1; s1 = a1 - b0; add0 = true; add1 = false; break;
        default: s0 = a0 - b0; s1 = a1 - b1; add0 = add1 = false; break;
        }
        int32_t s[2] = {s0, s1};
        bool add[2] = {add0, add1};
        for (int i = 0; i < 2; i++) {
            uint32_t o;
            if (kind == 2) o = uns ? sat_u(c, s[i], 16) : sat_s(c, s[i], 16);
            else if (kind == 3) o = (uint32_t)(s[i] >> 1);
            else {
                o = (uint32_t)s[i];
                bool g = uns ? (add[i] ? s[i] >= 0x10000 : s[i] >= 0) : s[i] >= 0;
                if (g) ge |= 3u << (2 * i);
            }
            r |= (o & 0xffff) << (16 * i);
        }
    }
    if (kind == 1)
        c->ge = ge;
    return r;
}

uint32_t a32_sel(a32_cpu *c, uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= (((c->ge >> i) & 1) ? a : b) & (0xffu << (8 * i));
    return r;
}

uint32_t a32_usad8(uint32_t a, uint32_t b)
{
    uint32_t s = 0;
    for (int i = 0; i < 4; i++) {
        int32_t x = (a >> (8 * i)) & 0xff, y = (b >> (8 * i)) & 0xff;
        s += (uint32_t)(x > y ? x - y : y - x);
    }
    return s;
}

uint32_t a32_rbit(uint32_t v)
{
    uint32_t r = 0;
    for (int i = 0; i < 32; i++)
        if (v & (1u << i)) r |= 1u << (31 - i);
    return r;
}

uint32_t a32_sdiv(int32_t a, int32_t b)
{
    if (!b) return 0;
    if (a == INT32_MIN && b == -1) return (uint32_t)INT32_MIN;
    return (uint32_t)(a / b);
}

/* dual multiply: op 0 SMLAD 1 SMLSD; swap = X; acc sera somado */
uint32_t a32_smlad(a32_cpu *c, bool sub, bool swap, uint32_t n, uint32_t m, uint32_t acc, bool has_acc)
{
    if (swap) m = ror32(m, 16);
    int64_t p1 = (int64_t)(int16_t)n * (int16_t)m, p2 = (int64_t)(int16_t)(n >> 16) * (int16_t)(m >> 16);
    int64_t r = sub ? p1 - p2 : p1 + p2;
    if (has_acc) r += (int32_t)acc;
    if (r != (int32_t)r) c->q = true;
    return (uint32_t)r;
}

uint64_t a32_smlald(bool sub, bool swap, uint32_t n, uint32_t m, uint64_t acc)
{
    if (swap) m = ror32(m, 16);
    int64_t p1 = (int64_t)(int16_t)n * (int16_t)m, p2 = (int64_t)(int16_t)(n >> 16) * (int16_t)(m >> 16);
    return acc + (uint64_t)(sub ? p1 - p2 : p1 + p2);
}

uint32_t a32_smmul(bool sub, bool round, uint32_t n, uint32_t m, uint32_t acc, bool has_acc)
{
    int64_t p = (int64_t)(int32_t)n * (int32_t)m;
    int64_t a = has_acc ? (int64_t)((uint64_t)acc << 32) : 0;
    int64_t r = sub ? a - p : a + p;
    if (round) r += 0x80000000LL;
    return (uint32_t)((uint64_t)r >> 32);
}

/* halfword multiply: SMULxy/SMLAxy */
static inline int32_t half(uint32_t v, bool top) { return top ? (int16_t)(v >> 16) : (int16_t)v; }

/* ------------------------------------------------------ decodificador ARM */

static inline uint32_t R(a32_cpu *c, unsigned n) { return c->r[n]; }

static void setreg(a32_cpu *c, unsigned n, uint32_t v)
{
    if (n == 15)
        a32_write_pc_alu(c, v);
    else
        c->r[n] = v;
}

static void load_reg(a32_cpu *c, unsigned n, uint32_t v)
{
    if (n == 15)
        a32_write_pc_bx(c, v);
    else
        c->r[n] = v;
}

static uint32_t arm_expand_imm_c(uint32_t insn, uint32_t cin, uint32_t *cout)
{
    uint32_t imm = insn & 0xff, rot = BITS(insn, 11, 8) * 2;
    uint32_t v = ror32(imm, rot);
    *cout = rot ? v >> 31 : cin;
    return v;
}

static void arm_dp(a32_cpu *c, uint32_t insn, uint32_t op2, uint32_t sc)
{
    unsigned opc = BITS(insn, 24, 21), rd = BITS(insn, 15, 12), rn = BITS(insn, 19, 16);
    bool s = BIT(insn, 20);
    bool w;
    if (s && rd == 15 && !(opc >= 8 && opc <= 11)) {
        /* SUBS PC, LR, ... : retorno de excecao */
        uint32_t r = a32_dp(c, opc, false, R(c, rn), op2, sc, &w);
        a32_exc_return(c, r);
        return;
    }
    uint32_t r = a32_dp(c, opc, s, R(c, rn), op2, sc, &w);
    if (w)
        setreg(c, rd, r);
}

static void arm_misc(a32_cpu *c, uint32_t insn)
{
    unsigned op2 = BITS(insn, 6, 4), op = BITS(insn, 22, 21);
    unsigned rd = BITS(insn, 15, 12), rm = BITS(insn, 3, 0), rn = BITS(insn, 19, 16);
    switch (op2) {
    case 0:
        if (op & 1) {
            if (BIT(insn, 9)) a32_undef(c); /* MSR banked */
            a32_msr(c, op & 2, BITS(insn, 19, 16), R(c, rm));
        } else {
            if (BIT(insn, 9)) a32_undef(c);
            c->r[rd] = a32_mrs(c, op & 2);
        }
        return;
    case 1:
        if (op == 1) { a32_write_pc_bx(c, R(c, rm)); return; }
        if (op == 3) { uint32_t v = R(c, rm); c->r[rd] = v ? (uint32_t)__builtin_clz(v) : 32; return; }
        break;
    case 2:
        if (op == 1) { a32_write_pc_bx(c, R(c, rm)); return; } /* BXJ */
        break;
    case 3:
        if (op == 1) {
            uint32_t t = R(c, rm);
            c->r[14] = c->cur + 4;
            a32_write_pc_bx(c, t);
            return;
        }
        break;
    case 5:
        c->r[rd] = a32_satop(c, op, (int32_t)R(c, rm), (int32_t)R(c, rn));
        return;
    case 7:
        if (op == 1) { a32_bkpt(c); return; }
        if (op == 2 || op == 3) { /* HVC / SMC -> PSCI */
            if (!a32_priv(c)) a32_undef(c);
            a32_psci(c);
            return;
        }
        break;
    }
    a32_undef(c);
}

static void arm_halfmul(a32_cpu *c, uint32_t insn)
{
    unsigned op = BITS(insn, 22, 21), rd = BITS(insn, 19, 16), ra = BITS(insn, 15, 12);
    unsigned rm = BITS(insn, 11, 8), rn = BITS(insn, 3, 0);
    bool nt = BIT(insn, 5), mt = BIT(insn, 6);
    int32_t a = half(R(c, rn), nt), b = half(R(c, rm), mt);
    switch (op) {
    case 0: { /* SMLAxy */
        int64_t r = (int64_t)a * b + (int32_t)R(c, ra);
        if (r != (int32_t)r) c->q = true;
        c->r[rd] = (uint32_t)r;
        break;
    }
    case 1: { /* SMLAWy / SMULWy */
        int64_t p = ((int64_t)(int32_t)R(c, rn) * half(R(c, rm), mt)) >> 16;
        if (!nt) {
            int64_t r = p + (int32_t)R(c, ra);
            if (r != (int32_t)r) c->q = true;
            c->r[rd] = (uint32_t)r;
        } else {
            c->r[rd] = (uint32_t)p;
        }
        break;
    }
    case 2: { /* SMLALxy */
        uint64_t acc = ((uint64_t)R(c, rd) << 32) | R(c, ra);
        acc += (uint64_t)(int64_t)(a * b);
        c->r[ra] = (uint32_t)acc;
        c->r[rd] = (uint32_t)(acc >> 32);
        break;
    }
    default: /* SMULxy */
        c->r[rd] = (uint32_t)(a * b);
        break;
    }
}

static void arm_mul(a32_cpu *c, uint32_t insn)
{
    unsigned op = BITS(insn, 23, 21), rd = BITS(insn, 19, 16), ra = BITS(insn, 15, 12);
    unsigned rm = BITS(insn, 11, 8), rn = BITS(insn, 3, 0);
    bool s = BIT(insn, 20);
    uint32_t n = R(c, rn), m = R(c, rm);
    switch (op) {
    case 0: case 1: {
        uint32_t r = n * m + (op == 1 ? R(c, ra) : 0);
        c->r[rd] = r;
        if (s) c->nzcv = (c->nzcv & 3) | ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2);
        return;
    }
    case 2: { /* UMAAL */
        uint64_t r = (uint64_t)n * m + R(c, ra) + R(c, rd);
        c->r[ra] = (uint32_t)r;
        c->r[rd] = (uint32_t)(r >> 32);
        return;
    }
    case 3: /* MLS */
        c->r[rd] = R(c, ra) - n * m;
        return;
    default: {
        uint64_t r;
        bool sgn = op & 2, acc = op & 1;
        if (sgn) r = (uint64_t)((int64_t)(int32_t)n * (int32_t)m);
        else r = (uint64_t)n * m;
        if (acc) r += ((uint64_t)R(c, rd) << 32) | R(c, ra);
        c->r[ra] = (uint32_t)r;
        c->r[rd] = (uint32_t)(r >> 32);
        if (s) c->nzcv = (c->nzcv & 3) | ((uint32_t)(r >> 63) << 3) | ((uint32_t)(r == 0) << 2);
        return;
    }
    }
}

static void arm_sync(a32_cpu *c, uint32_t insn)
{
    unsigned op = BITS(insn, 23, 20), rn = BITS(insn, 19, 16), rd = BITS(insn, 15, 12), rt = BITS(insn, 3, 0);
    uint32_t addr = R(c, rn);
    if (!(op & 8)) { /* SWP/SWPB */
        unsigned sz = BIT(insn, 22) ? 1 : 4;
        uint32_t old = a32_rd(c, addr, sz);
        a32_wr(c, addr, R(c, rt), sz);
        c->r[rd] = old;
        return;
    }
    static const unsigned sizes[4] = {4, 8, 1, 2};
    unsigned sz = sizes[(op >> 1) & 3];
    if (op & 1) { /* LDREX* */
        if (sz == 8) {
            uint32_t lo = a32_rd(c, addr, 4), hi = a32_rd(c, addr + 4, 4);
            c->r[rd] = lo;
            c->r[rd + 1] = hi;
            c->excl_val = ((uint64_t)hi << 32) | lo;
        } else {
            uint32_t v = a32_rd(c, addr, sz);
            c->r[rd] = v;
            c->excl_val = v;
        }
        c->excl_addr = addr;
        c->excl_size = (int)sz;
        c->excl_valid = true;
    } else { /* STREX*: Rd = status, Rt = valor */
        uint32_t status = 1;
        if (c->excl_valid && c->excl_addr == addr && c->excl_size == (int)sz) {
            if (sz == 8) {
                a32_wr(c, addr, R(c, rt), 4);
                a32_wr(c, addr + 4, R(c, rt + 1), 4);
            } else {
                a32_wr(c, addr, R(c, rt), sz);
            }
            status = 0;
        }
        c->excl_valid = false;
        c->r[rd] = status;
    }
}

static inline uint32_t ld_priv(a32_cpu *c, uint32_t addr, unsigned sz, int priv)
{
    return priv == a32_priv(c) ? a32_rd(c, addr, sz) : a32_read_slow(c, addr, sz, priv);
}

static inline void st_priv(a32_cpu *c, uint32_t addr, uint32_t v, unsigned sz, int priv)
{
    if (priv == a32_priv(c))
        a32_wr(c, addr, v, sz);
    else
        a32_write_slow(c, addr, v, sz, priv);
}

static void arm_extra_ldst(a32_cpu *c, uint32_t insn)
{
    bool p = BIT(insn, 24), u = BIT(insn, 23), imm = BIT(insn, 22), w = BIT(insn, 21), l = BIT(insn, 20);
    unsigned rn = BITS(insn, 19, 16), rt = BITS(insn, 15, 12), op2 = BITS(insn, 6, 5);
    uint32_t off = imm ? ((BITS(insn, 11, 8) << 4) | BITS(insn, 3, 0)) : R(c, BITS(insn, 3, 0));
    uint32_t base = rn == 15 ? (c->cur + 8) & ~3u : R(c, rn);
    uint32_t off_addr = u ? base + off : base - off;
    uint32_t addr = p ? off_addr : base;
    bool wb = !p || w;
    int priv = (!p && w) ? 0 : a32_priv(c);
    uint32_t v = 0, v2 = 0;
    bool dual = false;
    switch (op2) {
    case 1:
        if (l) v = ld_priv(c, addr, 2, priv);
        else st_priv(c, addr, R(c, rt), 2, priv);
        break;
    case 2:
        if (l) {
            v = (uint32_t)(int32_t)(int8_t)ld_priv(c, addr, 1, priv);
        } else { /* LDRD */
            v = a32_rd(c, addr, 4);
            v2 = a32_rd(c, addr + 4, 4);
            dual = true;
        }
        break;
    default:
        if (l) {
            v = (uint32_t)(int32_t)(int16_t)ld_priv(c, addr, 2, priv);
        } else { /* STRD */
            a32_wr(c, addr + 4, R(c, rt + 1), 4);
            a32_wr(c, addr, R(c, rt), 4);
        }
        break;
    }
    if (wb)
        c->r[rn] = off_addr;
    if (dual) {
        c->r[rt] = v;
        load_reg(c, rt + 1, v2);
    } else if (l) {
        load_reg(c, rt, v);
    }
}

static void arm_ldst(a32_cpu *c, uint32_t insn)
{
    bool reg = BIT(insn, 25), p = BIT(insn, 24), u = BIT(insn, 23), b = BIT(insn, 22), w = BIT(insn, 21), l = BIT(insn, 20);
    unsigned rn = BITS(insn, 19, 16), rt = BITS(insn, 15, 12);
    uint32_t off;
    if (reg) {
        uint32_t co;
        off = a32_shift_imm_c(R(c, BITS(insn, 3, 0)), BITS(insn, 6, 5), BITS(insn, 11, 7), (c->nzcv >> 1) & 1, &co);
    } else {
        off = insn & 0xfff;
    }
    uint32_t base = R(c, rn);
    if (rn == 15) base &= ~3u;
    uint32_t off_addr = u ? base + off : base - off;
    uint32_t addr = p ? off_addr : base;
    unsigned sz = b ? 1 : 4;
    bool user = !p && w;
    if (l) {
        uint32_t v = user ? a32_read_slow(c, addr, sz, 0) : a32_rd(c, addr, sz);
        if (!p || w)
            c->r[rn] = off_addr;
        load_reg(c, rt, v);
    } else {
        uint32_t v = rt == 15 ? c->cur + 8 : R(c, rt);
        if (user) a32_write_slow(c, addr, v, sz, 0);
        else a32_wr(c, addr, v, sz);
        if (!p || w)
            c->r[rn] = off_addr;
    }
}

static void arm_media(a32_cpu *c, uint32_t insn)
{
    unsigned op1 = BITS(insn, 24, 20), op2 = BITS(insn, 7, 5);
    unsigned rd = BITS(insn, 15, 12), rn = BITS(insn, 19, 16), rm = BITS(insn, 3, 0);
    if ((op1 & 0x1c) == 0x00) { /* paralelo */
        unsigned pre = op1 & 3 ? (op1 & 3) | (BIT(insn, 22) ? 4 : 0) : 0;
        if (!pre || (op2 != 0 && op2 != 1 && op2 != 2 && op2 != 3 && op2 != 4 && op2 != 7))
            a32_undef(c);
        c->r[rd] = a32_parallel(c, pre, op2, R(c, rn), R(c, rm));
        return;
    }
    if ((op1 & 0x18) == 0x08) { /* empacotamento, extensao, saturacao, reversao */
        unsigned o = op1 & 7;
        if (o == 0 && !(op2 & 1)) { /* PKH */
            uint32_t co, sh = a32_shift_imm_c(R(c, rm), BIT(insn, 6) ? 2 : 0, BITS(insn, 11, 7), 0, &co);
            if (BIT(insn, 6)) c->r[rd] = (R(c, rn) & 0xffff0000u) | (sh & 0xffff);
            else c->r[rd] = (R(c, rn) & 0xffff) | (sh & 0xffff0000u);
            return;
        }
        if (op2 == 3) { /* extensoes */
            unsigned rot = BITS(insn, 11, 10);
            unsigned t = ((op1 & 4) ? 4 : 0) | (op1 & 3);
            uint32_t add = rn == 15 ? 0 : R(c, rn);
            if (t == 1 || t == 5) a32_undef(c);
            c->r[rd] = a32_extend(t, R(c, rm), rot, add);
            return;
        }
        if (o == 0 && op2 == 5) { c->r[rd] = a32_sel(c, R(c, rn), R(c, rm)); return; }
        if ((o & 2) && !(op2 & 1)) { /* SSAT/USAT */
            uint32_t co;
            int32_t v = (int32_t)a32_shift_imm_c(R(c, rm), BIT(insn, 6) ? 2 : 0, BITS(insn, 11, 7), 0, &co);
            unsigned sat = BITS(insn, 20, 16);
            if (o & 4) c->r[rd] = sat_u(c, v, sat);
            else c->r[rd] = sat_s(c, v, sat + 1);
            return;
        }
        if ((o == 2 || o == 6) && op2 == 1) { /* SSAT16/USAT16 */
            unsigned sat = BITS(insn, 19, 16);
            uint32_t v = R(c, rm), lo, hi;
            if (o == 6) { lo = sat_u(c, (int16_t)v, sat); hi = sat_u(c, (int16_t)(v >> 16), sat); }
            else { lo = sat_s(c, (int16_t)v, sat + 1); hi = sat_s(c, (int16_t)(v >> 16), sat + 1); }
            c->r[rd] = (lo & 0xffff) | (hi << 16);
            return;
        }
        uint32_t v = R(c, rm);
        if (o == 3 && op2 == 1) { c->r[rd] = bswap32(v); return; }
        if (o == 3 && op2 == 5) { c->r[rd] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); return; }
        if (o == 7 && op2 == 1) { c->r[rd] = a32_rbit(v); return; }
        if (o == 7 && op2 == 5) { c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff)); return; }
        a32_undef(c);
    }
    if ((op1 & 0x18) == 0x10) { /* multiplicacoes com sinal, divisao */
        unsigned o = op1 & 7, rdh = BITS(insn, 19, 16), ra = BITS(insn, 15, 12), rmm = BITS(insn, 11, 8), rnn = BITS(insn, 3, 0);
        bool swap = BIT(insn, 5);
        switch (o) {
        case 0:
            if (op2 >= 4) a32_undef(c);
            c->r[rdh] = a32_smlad(c, op2 & 2, swap, R(c, rnn), R(c, rmm), ra == 15 ? 0 : R(c, ra), ra != 15);
            return;
        case 1:
            if (op2) a32_undef(c);
            c->r[rdh] = a32_sdiv((int32_t)R(c, rnn), (int32_t)R(c, rmm));
            return;
        case 3:
            if (op2) a32_undef(c);
            c->r[rdh] = R(c, rmm) ? R(c, rnn) / R(c, rmm) : 0;
            return;
        case 4: {
            if (op2 >= 4) a32_undef(c);
            uint64_t acc = ((uint64_t)R(c, rdh) << 32) | R(c, ra);
            acc = a32_smlald(op2 & 2, swap, R(c, rnn), R(c, rmm), acc);
            c->r[ra] = (uint32_t)acc;
            c->r[rdh] = (uint32_t)(acc >> 32);
            return;
        }
        case 5:
            if (op2 == 0 || op2 == 1)
                c->r[rdh] = a32_smmul(false, op2 & 1, R(c, rnn), R(c, rmm), ra == 15 ? 0 : R(c, ra), ra != 15);
            else if (op2 == 6 || op2 == 7)
                c->r[rdh] = a32_smmul(true, op2 & 1, R(c, rnn), R(c, rmm), R(c, ra), true);
            else
                a32_undef(c);
            return;
        default:
            a32_undef(c);
        }
    }
    if (op1 == 0x18 && op2 == 0) { /* USAD8/USADA8 */
        unsigned rdh = BITS(insn, 19, 16), ra = BITS(insn, 15, 12), rmm = BITS(insn, 11, 8), rnn = BITS(insn, 3, 0);
        c->r[rdh] = a32_usad8(R(c, rnn), R(c, rmm)) + (ra == 15 ? 0 : R(c, ra));
        return;
    }
    if ((op1 & 0x1e) == 0x1a && (op2 & 3) == 2) { /* SBFX */
        unsigned lsb = BITS(insn, 11, 7), w = BITS(insn, 20, 16) + 1;
        if (lsb + w > 32) a32_undef(c);
        c->r[rd] = (uint32_t)((int32_t)(R(c, rm) << (32 - lsb - w)) >> (32 - w));
        return;
    }
    if ((op1 & 0x1e) == 0x1c && (op2 & 3) == 0) { /* BFC/BFI */
        unsigned lsb = BITS(insn, 11, 7), msb = BITS(insn, 20, 16);
        if (msb < lsb) a32_undef(c);
        uint32_t mask = (msb - lsb == 31) ? ~0u : (((1u << (msb - lsb + 1)) - 1) << lsb);
        uint32_t src = rm == 15 ? 0 : R(c, rm) << lsb;
        c->r[rd] = (R(c, rd) & ~mask) | (src & mask);
        return;
    }
    if ((op1 & 0x1e) == 0x1e && (op2 & 3) == 2) { /* UBFX */
        unsigned lsb = BITS(insn, 11, 7), w = BITS(insn, 20, 16) + 1;
        if (lsb + w > 32) a32_undef(c);
        c->r[rd] = (R(c, rm) >> lsb) & (w == 32 ? ~0u : ((1u << w) - 1));
        return;
    }
    a32_undef(c);
}

static void arm_uncond(a32_cpu *c, uint32_t insn)
{
    if ((insn & 0x0e000000) == 0x0a000000) { /* BLX imediato */
        int32_t off = (int32_t)(insn << 8) >> 6;
        c->r[14] = c->cur + 4;
        c->thumb = true;
        c->npc = c->cur + 8 + (uint32_t)off + (BIT(insn, 24) << 1);
        return;
    }
    if ((insn & 0x0ff1fe20) == 0x01000000) { /* CPS */
        a32_cps(c, BITS(insn, 19, 18), BIT(insn, 17), BITS(insn, 8, 6), BITS(insn, 4, 0));
        return;
    }
    if ((insn & 0x0ffffdff) == 0x01010000) { /* SETEND */
        if (BIT(insn, 9)) LOGW("a32: SETEND BE nao suportado");
        return;
    }
    if ((insn & 0x0fffff00) == 0x057ff000) { /* CLREX/DSB/DMB/ISB */
        a32_barrier(c, BITS(insn, 7, 4));
        return;
    }
    if ((insn & 0x0d700000) == 0x05500000 || (insn & 0x0d700000) == 0x05100000 ||
        (insn & 0x0f700000) == 0x04500000 || (insn & 0x0f700000) == 0x04100000) {
        return; /* PLD/PLI/PLDW */
    }
    if ((insn & 0x0e5fffe0) == 0x084d0500 || (insn & 0x0e500000) == 0x08400000) { /* SRS */
        if (!a32_priv(c)) a32_undef(c);
        unsigned mode = insn & 0x1f;
        bool p = BIT(insn, 24), u = BIT(insn, 23), w = BIT(insn, 21);
        int b = bank_of(mode);
        uint32_t cur_sp = (bank_of(c->mode) == b) ? c->r[13] : c->bank_r13[b];
        uint32_t addr = u ? cur_sp + (p ? 4 : 0) : cur_sp - 8 + (p ? 0 : 4);
        uint32_t *spsr = cur_spsr(c);
        a32_wr(c, addr, c->r[14], 4);
        a32_wr(c, addr + 4, spsr ? *spsr : 0, 4);
        if (w) {
            uint32_t nsp = u ? cur_sp + 8 : cur_sp - 8;
            if (bank_of(c->mode) == b)
                c->r[13] = nsp;
            else
                c->bank_r13[b] = nsp;
        }
        return;
    }
    if ((insn & 0x0e500000) == 0x08100000) { /* RFE */
        if (!a32_priv(c)) a32_undef(c);
        bool p = BIT(insn, 24), u = BIT(insn, 23), w = BIT(insn, 21);
        unsigned rn = BITS(insn, 19, 16);
        uint32_t base = R(c, rn);
        uint32_t addr = u ? base + (p ? 4 : 0) : base - 8 + (p ? 0 : 4);
        uint32_t pc = a32_rd(c, addr, 4), cpsr = a32_rd(c, addr + 4, 4);
        if (w) c->r[rn] = u ? base + 8 : base - 8;
        set_cpsr_full(c, cpsr);
        c->npc = c->thumb ? pc & ~1u : pc & ~3u;
        return;
    }
    a32_undef(c);
}

static void arm_exec(a32_cpu *c, uint32_t insn)
{
    unsigned cond = insn >> 28;
    if (cond == 0xf) {
        arm_uncond(c, insn);
        return;
    }
    if (cond != 0xe && !arm_cond(cond, c->nzcv))
        return;
    switch (BITS(insn, 27, 25)) {
    case 0: {
        unsigned op1 = BITS(insn, 24, 20), op2 = BITS(insn, 7, 4);
        if ((op1 & 0x19) == 0x10 && !(op2 & 8)) { arm_misc(c, insn); return; }
        if ((op1 & 0x19) == 0x10 && (op2 & 9) == 8) { arm_halfmul(c, insn); return; }
        if (op2 == 9) {
            if (op1 & 0x10) arm_sync(c, insn);
            else arm_mul(c, insn);
            return;
        }
        if (op2 == 0xb || (op2 & 0xd) == 0xd) { arm_extra_ldst(c, insn); return; }
        uint32_t cin = (c->nzcv >> 1) & 1, sc, op2v;
        if (!(op2 & 1)) {
            op2v = a32_shift_imm_c(R(c, BITS(insn, 3, 0)), BITS(insn, 6, 5), BITS(insn, 11, 7), cin, &sc);
        } else {
            unsigned rm = BITS(insn, 3, 0), rs = BITS(insn, 11, 8);
            uint32_t mv = rm == 15 ? c->cur + 12 : R(c, rm);
            op2v = a32_shift_c(mv, BITS(insn, 6, 5), R(c, rs) & 0xff, cin, &sc);
            /* Rn == PC le PC+12 com deslocamento por registrador */
            if (BITS(insn, 19, 16) == 15) {
                uint32_t save = c->r[15];
                c->r[15] = c->cur + 12;
                arm_dp(c, insn, op2v, sc);
                c->r[15] = save;
                return;
            }
        }
        arm_dp(c, insn, op2v, sc);
        return;
    }
    case 1: {
        unsigned op1 = BITS(insn, 24, 20);
        if (op1 == 0x10) { c->r[BITS(insn, 15, 12)] = (BITS(insn, 19, 16) << 12) | (insn & 0xfff); return; }
        if (op1 == 0x14) {
            unsigned rd = BITS(insn, 15, 12);
            c->r[rd] = (c->r[rd] & 0xffff) | (((BITS(insn, 19, 16) << 12) | (insn & 0xfff)) << 16);
            return;
        }
        if ((op1 & 0x1b) == 0x12) { /* MSR imediato / hints */
            unsigned mask = BITS(insn, 19, 16);
            if (!mask && !BIT(insn, 22)) { a32_hint(c, insn & 0xff); return; }
            uint32_t co;
            a32_msr(c, BIT(insn, 22), mask, arm_expand_imm_c(insn, 0, &co));
            return;
        }
        uint32_t sc, v = arm_expand_imm_c(insn, (c->nzcv >> 1) & 1, &sc);
        arm_dp(c, insn, v, sc);
        return;
    }
    case 2:
        arm_ldst(c, insn);
        return;
    case 3:
        if (BIT(insn, 4)) { arm_media(c, insn); return; }
        arm_ldst(c, insn);
        return;
    case 4: { /* LDM/STM */
        bool p = BIT(insn, 24), u = BIT(insn, 23), s = BIT(insn, 22), w = BIT(insn, 21), l = BIT(insn, 20);
        unsigned rn = BITS(insn, 19, 16);
        a32_ldm_stm(c, l, u, p, rn, w, insn & 0xffff, s);
        return;
    }
    case 5: { /* B/BL */
        int32_t off = (int32_t)(insn << 8) >> 6;
        if (BIT(insn, 24))
            c->r[14] = c->cur + 4;
        c->npc = c->cur + 8 + (uint32_t)off;
        return;
    }
    default: /* coprocessador / SVC */
        if (BITS(insn, 27, 24) == 0xf) {
            a32_svc(c);
            return;
        }
        a32_coproc(c, insn);
        return;
    }
}

/* ------------------------------------------------------------------ run */

static int64_t a32_run(void *opaque, int64_t budget)
{
    a32_cpu *c = opaque;
    volatile int64_t n = 0;
    if (c->halted) {
        if (!c->irq_line && !c->fiq_line)
            return 0;
        c->halted = false;
    }
    setjmp(c->jb);
    while (n < budget) {
        if (unlikely(c->fiq_line && !c->fiq_dis))
            exception(c, 0x1c, M_FIQ, c->npc + 4);
        else if (unlikely(c->irq_line && !c->irq_dis))
            exception(c, 0x18, M_IRQ, c->npc + 4);
        if (unlikely(c->halted || atomic_load_explicit(&c->vm->cpu_exit, memory_order_relaxed)))
            break;
        uint32_t pc = c->npc;
        c->cur = pc;
        n++;
        if (c->thumb) {
            a32_exec_thumb(c);
        } else {
            uint32_t insn = fetch32(c, pc);
            c->insn = insn;
            c->npc = pc + 4;
            c->r[15] = pc + 8;
            arm_exec(c, insn);
        }
    }
    return n;
}

static bool a32_halted(void *opaque)
{
    a32_cpu *c = opaque;
    return c->halted && !c->irq_line && !c->fiq_line;
}

static void a32_reset(void *opaque)
{
    a32_cpu *c = opaque;
    memset(c->r, 0, sizeof(c->r));
    memset(c->bank_r13, 0, sizeof(c->bank_r13));
    memset(c->bank_r14, 0, sizeof(c->bank_r14));
    memset(c->bank_spsr, 0, sizeof(c->bank_spsr));
    memset(c->usr_r8_12, 0, sizeof(c->usr_r8_12));
    memset(c->fiq_r8_12, 0, sizeof(c->fiq_r8_12));
    memset(&c->vfp, 0, sizeof(c->vfp));
    c->mode = M_SVC;
    c->thumb = false;
    c->nzcv = 0;
    c->q = false;
    c->ge = 0;
    c->it = 0;
    c->irq_dis = c->fiq_dis = c->abt_dis = true;
    c->big_endian = false;
    c->fpscr = c->fpexc = 0;
    c->sctlr = 0x00c50078;
    c->actlr = c->cpacr = c->ttbr0 = c->ttbr1 = c->ttbcr = c->dacr = 0;
    c->dfsr = c->ifsr = c->dfar = c->ifar = c->adfsr = c->aifsr = c->par = 0;
    c->prrr = c->nmrr = c->vbar = c->fcseidr = c->contextidr = 0;
    c->tpidrurw = c->tpidruro = c->tpidrprw = c->csselr = c->pmuserenr = 0;
    c->excl_valid = false;
    c->halted = false;
    c->npc = 0;
    gt_reset(&c->gt);
    tlb_flush(c);
}

static void a32_dump(void *opaque, FILE *f)
{
    a32_cpu *c = opaque;
    fprintf(f, "PC=%08x %s CPSR=%08x\n", c->cur, c->thumb ? "Thumb" : "ARM", a32_get_cpsr(c));
    for (int i = 0; i < 15; i++)
        fprintf(f, "r%-2d=%08x%s", i, c->r[i], (i % 4 == 3) ? "\n" : " ");
    fprintf(f, "\nSCTLR=%08x TTBR0=%08x TTBR1=%08x TTBCR=%x DACR=%08x\n", c->sctlr, c->ttbr0, c->ttbr1,
            c->ttbcr, c->dacr);
    fprintf(f, "DFSR=%08x DFAR=%08x IFSR=%08x IFAR=%08x\n", c->dfsr, c->dfar, c->ifsr, c->ifar);
}

static void a32_destroy(void *opaque)
{
    a32_cpu *c = opaque;
    c->gt.irq = NULL; /* a maquina (GIC) ja pode ter sido liberada */
    gt_reset(&c->gt);
    free(c);
}

static void cpu_tlb_flush_cb(void *cpu) { tlb_flush((a32_cpu *)cpu); }

const mvm_cpu_ops arm32_cpu_ops = {
    .reset = a32_reset,
    .run = a32_run,
    .halted = a32_halted,
    .dump = a32_dump,
    .destroy = a32_destroy,
    .tlb_flush = cpu_tlb_flush_cb,
};

void *arm32_cpu_new(mvm_vm *vm, const arm_hooks *hooks)
{
    a32_cpu *c = calloc(1, sizeof(*c));
    c->vm = vm;
    c->mem = &vm->mem;
    c->hooks = *hooks;
    gt_init(&c->gt, vm);
    c->gt.irq = hooks->timer_irq;
    c->gt.opaque = hooks->opaque;
    a32_reset(c);
    return c;
}

void arm32_set_irq(void *opaque, int line, int level)
{
    a32_cpu *c = opaque;
    if (line == 0)
        c->irq_line = level;
    else
        c->fiq_line = level;
}

void arm32_set_entry(void *opaque, uint32_t pc, uint32_t r0, uint32_t r1, uint32_t r2)
{
    a32_cpu *c = opaque;
    c->r[0] = r0;
    c->r[1] = r1;
    c->r[2] = r2;
    c->thumb = pc & 1;
    c->npc = pc & ~1u;
}
