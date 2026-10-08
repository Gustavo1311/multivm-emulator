/*
 * CPU x86 (i386 / x86-64): infraestrutura. Memoria, paginacao, segmentacao,
 * excecoes e interrupcoes, instrucoes de sistema, laco de execucao.
 * A decodificacao das instrucoes esta em x86_exec.c.
 */
#include "cpu_x86.h"
#include "x86_priv.h"
#include "x86_jit.h"

#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ------------------------------------------------------------ flags */

uint64_t x86_arith_flags(x86_cpu *c)
{
    if (c->cc_op == CC_NONE)
        return c->eflags & EFL_ARITH;
    int sz = c->cc_size;
    uint64_t m = szmask(sz), sb = 1ULL << (sz * 8 - 1);
    uint64_t res = c->cc_dst & m, a = c->cc_src1 & m, b = c->cc_src2 & m;
    bool cf = false, of = false, af = false;
    switch (c->cc_op) {
    case CC_ADD:
        cf = res < a;
        of = ((a ^ res) & (b ^ res) & sb) != 0;
        af = ((a ^ b ^ res) & 0x10) != 0;
        break;
    case CC_ADC:
        cf = c->cc_aux ? res <= a : res < a;
        of = ((a ^ res) & (b ^ res) & sb) != 0;
        af = ((a ^ b ^ res) & 0x10) != 0;
        break;
    case CC_SUB:
        cf = a < b;
        of = ((a ^ b) & (a ^ res) & sb) != 0;
        af = ((a ^ b ^ res) & 0x10) != 0;
        break;
    case CC_SBB:
        cf = c->cc_aux ? a <= b : a < b;
        of = ((a ^ b) & (a ^ res) & sb) != 0;
        af = ((a ^ b ^ res) & 0x10) != 0;
        break;
    case CC_LOGIC:
        break;
    case CC_INC:
        cf = c->cc_aux;
        of = res == sb;
        af = (res & 0xf) == 0;
        break;
    case CC_DEC:
        cf = c->cc_aux;
        of = res == sb - 1;
        af = (res & 0xf) == 0xf;
        break;
    case CC_SZP:
        cf = c->cc_aux & 1;
        of = (c->cc_aux >> 1) & 1;
        break;
    }
    uint64_t f = 0;
    if (cf) f |= EFL_CF;
    if (!__builtin_parity((unsigned)(res & 0xff))) f |= EFL_PF;
    if (af) f |= EFL_AF;
    if (!res) f |= EFL_ZF;
    if (res & sb) f |= EFL_SF;
    if (of) f |= EFL_OF;
    return f;
}

uint64_t x86_get_flags(x86_cpu *c) { return (c->eflags & ~(uint64_t)EFL_ARITH) | x86_arith_flags(c) | 2; }

void x86_set_arith_flags(x86_cpu *c, uint64_t fl)
{
    c->eflags = (c->eflags & ~(uint64_t)EFL_ARITH) | (fl & EFL_ARITH);
    c->cc_op = CC_NONE;
}

bool x86_cond(x86_cpu *c, int cc)
{
    bool r;
    if (c->cc_op == CC_SUB && (cc >> 1) != 5 && (cc >> 1) != 0) {
        int sz = c->cc_size;
        uint64_t m = szmask(sz);
        uint64_t a = c->cc_src1 & m, b = c->cc_src2 & m;
        int64_t sa = (int64_t)sext_sz(a, sz), sbv = (int64_t)sext_sz(b, sz);
        switch (cc >> 1) {
        case 1: r = a < b; break;
        case 2: r = a == b; break;
        case 3: r = a <= b; break;
        case 4: r = ((c->cc_dst & m) >> (sz * 8 - 1)) & 1; break;
        case 6: r = sa < sbv; break;
        default: r = sa <= sbv; break;
        }
        return (cc & 1) ? !r : r;
    }
    if (c->cc_op == CC_LOGIC) {
        int sz = c->cc_size;
        uint64_t res = c->cc_dst & szmask(sz);
        bool sf = (res >> (sz * 8 - 1)) & 1;
        switch (cc >> 1) {
        case 0: r = false; break;
        case 1: r = false; break;
        case 2: r = res == 0; break;
        case 3: r = res == 0; break;
        case 4: r = sf; break;
        case 5: r = !__builtin_parity((unsigned)(res & 0xff)); break;
        case 6: r = sf; break;
        default: r = res == 0 || sf; break;
        }
        return (cc & 1) ? !r : r;
    }
    uint64_t f = x86_arith_flags(c);
    bool cf = f & EFL_CF, zf = f & EFL_ZF, sf = f & EFL_SF, of = f & EFL_OF, pf = f & EFL_PF;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf || zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf || (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}

static uint8_t *x86_host_ptr_nofault(x86_cpu *c, uint64_t lin);
static void trace_dump(x86_cpu *c, const char *why);
static void x86_ram_dump(x86_cpu *c);
/* MVM_X86_TRACE_STOP_ON_PF=1: o rastro e gravado na falta de pagina em MVM_X86_TRACE_STOP */
static bool trace_stop_on_pf(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("MVM_X86_TRACE_STOP_ON_PF") != NULL;
    return v;
}
static bool dbg_read(x86_cpu *c, uint64_t lin, void *out, size_t n);
static void x86_guest_process(x86_cpu *c, char *out, size_t n);
void x86_debug_dump(void *opaque);

/* ------------------------------------------------------------ TLB/MMU */

#define TLB_E(c, k) (&(c)->tlb[0][0] + (k)) /* k = (user << X86_TLB_BITS) | indice */

void x86_tlb_flush(x86_cpu *c)
{
    if (!c->tlb_listed) {
        for (int u = 0; u < 2; u++)
            for (unsigned i = 0; i < X86_TLB_SIZE; i++)
                c->tlb[u][i].tag_r = c->tlb[u][i].tag_w = c->tlb[u][i].tag_x = XTLB_INVALID;
        memset(c->tlb_inlist, 0, sizeof(c->tlb_inlist));
        c->tlb_listed = true;
    } else {
        for (unsigned n = 0; n < c->tlb_nused; n++) {
            unsigned k = c->tlb_used[n];
            x86_tlbe *e = TLB_E(c, k);
            e->tag_r = e->tag_w = e->tag_x = XTLB_INVALID;
            (&c->tlb_inlist[0][0])[k] = 0;
        }
    }
    c->tlb_nused = 0;
    c->fetch_page = 1;
    if (c->jit)
        x86_jit_tlb_flushed(c->jit);
}

/* paginas sujas de VRAM foram consumidas: as escritas nelas voltam ao caminho lento */
static void x86_tlb_protect(void *opaque, const uint8_t *host, size_t len)
{
    x86_cpu *c = opaque;
    for (unsigned n = 0; n < c->tlb_nused; n++) {
        x86_tlbe *e = TLB_E(c, c->tlb_used[n]);
        if (!(e->tag_w & (XTLB_INVALID | XTLB_IO)) &&
            (uintptr_t)(e->addend + (e->tag_w & ~0xfffULL)) - (uintptr_t)host < len)
            e->tag_w |= XTLB_IO;
    }
}

/* a pagina fisica ppage passou a ter codigo traduzido: escritas nela pelo caminho lento.
 * So as entradas que apontam para ela mudam (esvaziar o TLB inteiro custava caro). */
void x86_tlb_protect_page(x86_cpu *c, uint64_t ppage)
{
    for (unsigned n = 0; n < c->tlb_nused; n++) {
        x86_tlbe *e = TLB_E(c, c->tlb_used[n]);
        if (e->pa == ppage && !(e->tag_w & (XTLB_INVALID | XTLB_IO)))
            e->tag_w |= XTLB_IO;
    }
}

/* a pagina deixou de ter codigo: a proxima escrita refaz a entrada (rapida de novo) */
void x86_tlb_unprotect_page(x86_cpu *c, uint64_t ppage)
{
    for (unsigned n = 0; n < c->tlb_nused; n++) {
        x86_tlbe *e = TLB_E(c, c->tlb_used[n]);
        if (e->pa == ppage && (e->tag_w & XTLB_IO) && !(e->tag_w & XTLB_INVALID))
            e->tag_w = XTLB_INVALID;
    }
}

/* troca de CR3: mantem as entradas de paginas globais */
void x86_tlb_flush_nonglobal(x86_cpu *c)
{
    if (!c->tlb_listed) {
        x86_tlb_flush(c);
        return;
    }
    unsigned keep = 0;
    for (unsigned n = 0; n < c->tlb_nused; n++) {
        unsigned k = c->tlb_used[n];
        if ((&c->tlb_g[0][0])[k]) {
            c->tlb_used[keep++] = (uint16_t)k;
        } else {
            x86_tlbe *e = TLB_E(c, k);
            e->tag_r = e->tag_w = e->tag_x = XTLB_INVALID;
            (&c->tlb_inlist[0][0])[k] = 0;
        }
    }
    c->tlb_nused = keep;
    c->fetch_page = 1;
    if (c->jit)
        x86_jit_tlb_flushed(c->jit);
}

void x86_tlb_flush_page(x86_cpu *c, uint64_t lin)
{
    for (int u = 0; u < 2; u++) {
        x86_tlbe *e = &c->tlb[u][(lin >> 12) & (X86_TLB_SIZE - 1)];
        e->tag_r = e->tag_w = e->tag_x = XTLB_INVALID;
    }
    c->fetch_page = 1;
    if (c->jit)
        x86_jit_tlb_flushed(c->jit);
}

static uint64_t phys_rd(x86_cpu *c, uint64_t pa, unsigned size)
{
    uint8_t *p = space_ram_ptr(c->mem, pa, size, false);
    return p ? ld_le(p, size) : space_read(c->mem, pa, size);
}

static void phys_wr(x86_cpu *c, uint64_t pa, uint64_t v, unsigned size)
{
    uint8_t *p = space_ram_ptr(c->mem, pa, size, true);
    if (p)
        st_le(p, v, size);
    else
        space_write(c->mem, pa, v, size);
}

static _Noreturn void page_fault(x86_cpu *c, uint64_t lin, bool present, int acc, int user)
{
    uint32_t err = (present ? 1 : 0) | (acc == 1 ? 2 : 0) | (user ? 4 : 0);
    if (acc == 2 && (c->efer & EFER_NXE))
        err |= 16;
    static int pfdbg = -1;
    if (pfdbg < 0)
        pfdbg = getenv("MVM_X86_PFDEBUG") != NULL; /* #PF com IRQL alto (Windows x64) */
    if (pfdbg && !user &&
        ((c->code64 && c->apic && (c->apic_get_tpr(c->apic) >> 4) >= 2) || (!c->code64 && lin < 0x10000))) {
        /* falta de pagina com IRQL >= DISPATCH (Windows x64): quase sempre um bug */
        uint8_t *h = x86_host_ptr_nofault(c, c->cur_rip);
        LOGI("x86: #PF com IRQL %d em %llx: lin=%llx err=%x bytes %02x %02x %02x %02x %02x %02x",
             c->apic ? (int)(c->apic_get_tpr(c->apic) >> 4) : -1, (unsigned long long)c->cur_rip, (unsigned long long)lin, err,
             h ? h[0] : 0, h ? h[1] : 0, h ? h[2] : 0, h ? h[3] : 0, h ? h[4] : 0, h ? h[5] : 0);
        x86_debug_dump(c);
        x86_ram_dump(c);
        if (c->trace && !c->trace_dumped && lin < 0x10000) {
            c->trace_dumped = true;
            trace_dump(c, "#PF em endereco nulo com IRQL alto");
        }
    }
    c->cr2 = lin;
    if (c->trace && c->trace_stop && c->cur_rip == c->trace_stop && !c->trace_dumped && trace_stop_on_pf()) {
        c->trace_dumped = true;
        LOGI("x86: #PF em %llx (lin=%llx): gravando rastro", (unsigned long long)c->cur_rip, (unsigned long long)lin);
        x86_debug_dump(c);
        trace_dump(c, "#PF em MVM_X86_TRACE_STOP");
    }
    x86_exception(c, EXC_PF, 1, err);
}

/* Percorre as tabelas. perm: bit0 R, bit1 W, bit2 X (para o nivel de privilegio pedido). */
static uint64_t walk(x86_cpu *c, uint64_t lin, int acc, int user, int *perm_out)
{
    if (!(c->cr0 & CR0_PG)) {
        *perm_out = 7;
        return (c->efer & EFER_LMA ? lin : (uint32_t)lin) & c->a20_mask;
    }
    bool nxe = c->efer & EFER_NXE;
    int pw = 1, pu = 1, nx = 0;
    uint64_t pa, leaf_addr, leaf;
    unsigned leaf_size;
    if (c->efer & EFER_LMA) {
        if ((uint64_t)((int64_t)(lin << 16) >> 16) != lin)
            x86_gp(c, 0);
        uint64_t table = c->cr3 & 0x000ffffffffff000ULL;
        int shift = 39;
        for (;;) {
            uint64_t a = table + ((lin >> shift) & 511) * 8;
            uint64_t e = phys_rd(c, a, 8);
            if (!(e & 1))
                page_fault(c, lin, false, acc, user);
            pw &= (int)(e >> 1) & 1;
            pu &= (int)(e >> 2) & 1;
            if (nxe) nx |= (int)(e >> 63);
            if ((shift == 30 || shift == 21) && (e & 0x80)) {
                uint64_t sz = 1ULL << shift;
                pa = (e & 0x000fffffffffe000ULL & ~(sz - 1)) | (lin & (sz - 1));
                leaf_addr = a;
                leaf = e;
                break;
            }
            if (shift == 12) {
                pa = (e & 0x000ffffffffff000ULL) | (lin & 0xfff);
                leaf_addr = a;
                leaf = e;
                break;
            }
            if (!(e & 0x20))
                phys_wr(c, a, e | 0x20, 8);
            table = e & 0x000ffffffffff000ULL;
            shift -= 9;
        }
        leaf_size = 8;
    } else if (c->cr4 & CR4_PAE) {
        uint64_t pdpte = phys_rd(c, (c->cr3 & 0xffffffe0u) + ((lin >> 30) & 3) * 8, 8);
        if (!(pdpte & 1))
            page_fault(c, lin, false, acc, user);
        uint64_t a = (pdpte & 0x000ffffffffff000ULL) + ((lin >> 21) & 511) * 8;
        uint64_t e = phys_rd(c, a, 8);
        if (!(e & 1))
            page_fault(c, lin, false, acc, user);
        pw &= (int)(e >> 1) & 1;
        pu &= (int)(e >> 2) & 1;
        if (nxe) nx |= (int)(e >> 63);
        if (e & 0x80) {
            pa = (e & 0x000fffffffe00000ULL) | (lin & 0x1fffff);
            leaf_addr = a;
            leaf = e;
        } else {
            if (!(e & 0x20))
                phys_wr(c, a, e | 0x20, 8);
            uint64_t a2 = (e & 0x000ffffffffff000ULL) + ((lin >> 12) & 511) * 8;
            uint64_t e2 = phys_rd(c, a2, 8);
            if (!(e2 & 1))
                page_fault(c, lin, false, acc, user);
            pw &= (int)(e2 >> 1) & 1;
            pu &= (int)(e2 >> 2) & 1;
            if (nxe) nx |= (int)(e2 >> 63);
            pa = (e2 & 0x000ffffffffff000ULL) | (lin & 0xfff);
            leaf_addr = a2;
            leaf = e2;
        }
        leaf_size = 8;
    } else {
        uint32_t la = (uint32_t)lin;
        uint64_t a = (c->cr3 & 0xfffff000u) + ((la >> 22) & 1023) * 4;
        uint32_t e = (uint32_t)phys_rd(c, a, 4);
        if (!(e & 1))
            page_fault(c, lin, false, acc, user);
        pw &= (int)(e >> 1) & 1;
        pu &= (int)(e >> 2) & 1;
        if ((e & 0x80) && (c->cr4 & CR4_PSE)) {
            pa = (e & 0xffc00000u) | (la & 0x3fffff);
            leaf_addr = a;
            leaf = e;
        } else {
            if (!(e & 0x20))
                phys_wr(c, a, e | 0x20, 4);
            uint64_t a2 = (e & 0xfffff000u) + ((la >> 12) & 1023) * 4;
            uint32_t e2 = (uint32_t)phys_rd(c, a2, 4);
            if (!(e2 & 1))
                page_fault(c, lin, false, acc, user);
            pw &= (int)(e2 >> 1) & 1;
            pu &= (int)(e2 >> 2) & 1;
            pa = (e2 & 0xfffff000u) | (la & 0xfff);
            leaf_addr = a2;
            leaf = e2;
        }
        leaf_size = 4;
    }
    bool wp = c->cr0 & CR0_WP;
    if (user && !pu)
        page_fault(c, lin, true, acc, user);
    if (acc == 1 && !pw && (user || wp))
        page_fault(c, lin, true, acc, user);
    if (acc == 2 && nx)
        page_fault(c, lin, true, acc, user);
    uint64_t upd = leaf | 0x20;
    if (acc == 1)
        upd |= 0x40;
    if (upd != leaf)
        phys_wr(c, leaf_addr, upd, leaf_size);
    int perm = 1;
    bool can_w = pw || (!user && !wp);
    if (can_w && (upd & 0x40)) /* so cacheia escrita se o bit D ja estiver marcado */
        perm |= 2;
    if (!nx)
        perm |= 4;
    if ((leaf & 0x100) && (c->cr4 & CR4_PGE))
        perm |= 8; /* global */
    *perm_out = perm;
    return pa & c->a20_mask;
}

static x86_tlbe *tlb_fill(x86_cpu *c, uint64_t lin, int acc, int user)
{
    int perm;
    uint64_t pa = walk(c, lin, acc, user, &perm);
    uint64_t page = lin & ~0xfffULL, ppage = pa & ~0xfffULL;
    unsigned idx = (unsigned)(lin >> 12) & (X86_TLB_SIZE - 1);
    x86_tlbe *e = &c->tlb[user][idx];
    if (!c->tlb_inlist[user][idx]) {
        c->tlb_inlist[user][idx] = 1;
        c->tlb_used[c->tlb_nused++] = (uint16_t)(((unsigned)user << X86_TLB_BITS) | idx);
    }
    mvm_region *reg = space_find(c->mem, ppage);
    uint8_t *host = (reg && reg->host && ppage - reg->base + 0x1000 <= reg->size) ? reg->host + (ppage - reg->base) : NULL;
    uint64_t io = host ? 0 : XTLB_IO;
    e->tag_r = page | io;
    /* paginas com codigo traduzido pelo JIT: escritas pelo caminho lento (invalidacao) */
    /* paginas de VRAM ja marcadas sujas tambem: o caminho lento so precisa ver a primeira escrita */
    bool fast_w = host && !reg->readonly && (!reg->dirty_gen || region_page_dirty(reg, ppage)) &&
                  !space_is_code(c->mem, ppage);
    e->tag_w = (perm & 2) ? page | (fast_w ? 0 : XTLB_IO) : XTLB_INVALID;
    e->tag_x = (perm & 4) ? page | io : XTLB_INVALID;
    e->addend = host ? (uintptr_t)host - (uintptr_t)page : 0;
    e->pa = ppage;
    c->tlb_g[user][(lin >> 12) & (X86_TLB_SIZE - 1)] = (perm & 8) != 0;
    return e;
}

static inline bool hit(uint64_t tag, uint64_t page) { return !(tag & XTLB_INVALID) && (tag & ~0xfffULL) == page; }

uint64_t x86_read_slow(x86_cpu *c, uint64_t lin, unsigned size, int user)
{
    if ((lin & 0xfff) + size > 0x1000) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++)
            v |= x86_read_slow(c, lin + i, 1, user) << (8 * i);
        return v;
    }
    x86_tlbe *e = &c->tlb[user][(lin >> 12) & (X86_TLB_SIZE - 1)];
    if (!hit(e->tag_r, lin & ~0xfffULL))
        e = tlb_fill(c, lin, 0, user);
    if (!(e->tag_r & XTLB_IO))
        return ld_le((const uint8_t *)(e->addend + lin), size);
    return space_read(c->mem, e->pa | (lin & 0xfff), size);
}

void x86_write_slow(x86_cpu *c, uint64_t lin, uint64_t v, unsigned size, int user)
{
    if ((lin & 0xfff) + size > 0x1000) {
        uint64_t last = lin + size - 1;
        x86_tlbe *e2 = &c->tlb[user][(last >> 12) & (X86_TLB_SIZE - 1)];
        if (!hit(e2->tag_w, last & ~0xfffULL))
            tlb_fill(c, last, 1, user);
        x86_tlbe *e1 = &c->tlb[user][(lin >> 12) & (X86_TLB_SIZE - 1)];
        if (!hit(e1->tag_w, lin & ~0xfffULL))
            tlb_fill(c, lin, 1, user);
        for (unsigned i = 0; i < size; i++)
            x86_write_slow(c, lin + i, (v >> (8 * i)) & 0xff, 1, user);
        return;
    }
    x86_tlbe *e = &c->tlb[user][(lin >> 12) & (X86_TLB_SIZE - 1)];
    if (!hit(e->tag_w, lin & ~0xfffULL))
        e = tlb_fill(c, lin, 1, user);
    if (!(e->tag_w & XTLB_IO)) {
        st_le((uint8_t *)(e->addend + lin), v, size);
        return;
    }
    space_write(c->mem, e->pa | (lin & 0xfff), v, size);
}

/* Garante que [lin, lin+size) pode ser escrito, gerando a falha agora (antes de a
 * instrucao alterar registradores ou flags que ela mesma le ao ser reiniciada). */
void x86_probe_write(x86_cpu *c, uint64_t lin, unsigned size)
{
    int user = x86_user(c);
    uint64_t last = lin + size - 1;
    x86_tlbe *e = &c->tlb[user][(lin >> 12) & (X86_TLB_SIZE - 1)];
    if (!hit(e->tag_w, lin & ~0xfffULL))
        tlb_fill(c, lin, 1, user);
    if ((last ^ lin) & ~0xfffULL) {
        x86_tlbe *e2 = &c->tlb[user][(last >> 12) & (X86_TLB_SIZE - 1)];
        if (!hit(e2->tag_w, last & ~0xfffULL))
            tlb_fill(c, last, 1, user);
    }
}

/* Ponteiro de host para a pagina de 'lin' (RAM), preenchendo a TLB; NULL se for E/S. */
uint8_t *x86_host_ptr(x86_cpu *c, uint64_t lin, bool write)
{
    int user = x86_user(c);
    x86_tlbe *e = &c->tlb[user][(lin >> 12) & (X86_TLB_SIZE - 1)];
    uint64_t page = lin & ~0xfffULL;
    if (!hit(write ? e->tag_w : e->tag_r, page))
        e = tlb_fill(c, lin, write ? 1 : 0, user);
    uint64_t tag = write ? e->tag_w : e->tag_r;
    if ((tag & (XTLB_IO | XTLB_INVALID)))
        return NULL;
    return (uint8_t *)(e->addend + lin);
}

/* Acessos do sistema (GDT/IDT/TSS): sempre supervisor */
uint64_t x86_sys_rd(x86_cpu *c, uint64_t lin, unsigned size) { return x86_read_slow(c, lin, size, 0); }
void x86_sys_wr(x86_cpu *c, uint64_t lin, uint64_t v, unsigned size) { x86_write_slow(c, lin, v, size, 0); }

static uint8_t fetch_slow(x86_cpu *c, uint64_t lin)
{
    int user = x86_user(c);
    x86_tlbe *e = &c->tlb[user][(lin >> 12) & (X86_TLB_SIZE - 1)];
    if (!hit(e->tag_x, lin & ~0xfffULL))
        e = tlb_fill(c, lin, 2, user);
    if (!(e->tag_x & XTLB_IO)) {
        c->fetch_page = lin & ~0xfffULL;
        c->fetch_host = (uint8_t *)(e->addend + c->fetch_page);
        return c->fetch_host[lin & 0xfff];
    }
    return (uint8_t)space_read(c->mem, e->pa | (lin & 0xfff), 1);
}

/* Ponteiro de host e endereco fisico da pagina de codigo em 'lin' (pode gerar #PF).
 * NULL se o codigo estiver em E/S. Usado pelo JIT para ler as instrucoes. */
uint8_t *x86_code_host(x86_cpu *c, uint64_t lin, uint64_t *pa)
{
    int user = x86_user(c);
    x86_tlbe *e = &c->tlb[user][(lin >> 12) & (X86_TLB_SIZE - 1)];
    if (!hit(e->tag_x, lin & ~0xfffULL))
        e = tlb_fill(c, lin, 2, user);
    if (e->tag_x & XTLB_IO)
        return NULL;
    *pa = e->pa;
    return (uint8_t *)(e->addend + (lin & ~0xfffULL));
}

uint8_t x86_fetch8_slow(x86_cpu *c)
{
    c->ip_left = 0; /* o resto da instrucao tambem vai pelo caminho lento */
    uint64_t lin = c->code64 ? c->rip : (uint32_t)(c->seg[S_CS].base + c->rip);
    c->rip++;
    if (!c->code64 && c->csz == 2)
        c->rip &= 0xffff;
    if (likely((lin & ~0xfffULL) == c->fetch_page))
        return c->fetch_host[lin & 0xfff];
    return fetch_slow(c, lin);
}

uint32_t x86_fetch32_slow(x86_cpu *c)
{
    c->ip_left = 0;
    uint64_t lin = c->code64 ? c->rip : (uint32_t)(c->seg[S_CS].base + c->rip);
    if (likely((lin & ~0xfffULL) == c->fetch_page) && (lin & 0xfff) <= 0xffc && (c->code64 || c->csz == 4)) {
        c->rip += 4;
        return (uint32_t)ld_le(c->fetch_host + (lin & 0xfff), 4);
    }
    uint32_t v = x86_fetch8_slow(c);
    v |= (uint32_t)x86_fetch8_slow(c) << 8;
    v |= (uint32_t)x86_fetch8_slow(c) << 16;
    v |= (uint32_t)x86_fetch8_slow(c) << 24;
    return v;
}


/* ------------------------------------------------------------ segmentos */

static void update_mode(x86_cpu *c)
{
    x86_seg *cs = &c->seg[S_CS];
    c->code64 = (c->efer & EFER_LMA) && (cs->attr & SEG_L);
    if (c->code64) {
        c->csz = 4;
        c->ssz = 8;
    } else {
        c->csz = (cs->attr & SEG_DB) ? 4 : 2;
        c->ssz = (c->seg[S_SS].attr & SEG_DB) ? 4 : 2;
    }
    c->fetch_page = 1;
}

static bool protected_mode(x86_cpu *c) { return (c->cr0 & CR0_PE) && !(c->eflags & EFL_VM); }

uint64_t x86_read_desc(x86_cpu *c, uint16_t sel, uint64_t *hi)
{
    uint64_t base;
    uint32_t limit;
    if (sel & 4) {
        if (!(c->seg[S_LDTR].sel & ~3u)) /* LDTR nula */
            x86_gp(c, sel & 0xfffc);
        base = c->seg[S_LDTR].base;
        limit = c->seg[S_LDTR].limit;
    } else {
        base = c->gdt_base;
        limit = c->gdt_limit;
    }
    if ((uint32_t)(sel | 7) > limit)
        x86_gp(c, sel & 0xfffc);
    uint64_t d = x86_sys_rd(c, base + (sel & ~7u), 8);
    if (hi)
        *hi = ((sel | 15u) <= limit) ? x86_sys_rd(c, base + (sel & ~7u) + 8, 8) : 0;
    return d;
}

void x86_desc_to_seg(x86_seg *s, uint16_t sel, uint64_t d)
{
    s->sel = sel;
    s->base = ((d >> 16) & 0xffffff) | (((d >> 56) & 0xff) << 24);
    uint32_t limit = (uint32_t)((d & 0xffff) | ((d >> 32) & 0xf0000));
    if ((d >> 55) & 1)
        limit = (limit << 12) | 0xfff;
    s->limit = limit;
    s->attr = (uint32_t)((d >> 40) & 0xf0ff);
}

void x86_load_seg(x86_cpu *c, int s, uint16_t sel)
{
    x86_seg *sg = &c->seg[s];
    if (!protected_mode(c)) {
        sg->sel = sel;
        sg->base = (uint64_t)sel << 4;
        if (c->eflags & EFL_VM) {
            sg->limit = 0xffff;
            sg->attr = SEG_P | SEG_S | 3 | (3 << 5) | (s == S_CS ? 8 : 0);
        }
        if (s == S_CS || s == S_SS)
            update_mode(c);
        return;
    }
    if ((sel & ~3u) == 0) {
        if (s == S_CS || (s == S_SS && !(c->code64 && c->cpl != 3)))
            x86_gp(c, 0);
        sg->sel = sel;
        sg->base = 0;
        sg->limit = 0;
        sg->attr = 0;
        if (s == S_SS)
            update_mode(c);
        return;
    }
    uint64_t d = x86_read_desc(c, sel, NULL);
    if (!((d >> 47) & 1))
        x86_exception(c, s == S_SS ? EXC_SS : EXC_NP, 1, sel & 0xfffc);
    if (!((d >> 44) & 1))
        x86_gp(c, sel & 0xfffc); /* descritor de sistema */
    x86_desc_to_seg(sg, sel, d);
    if (s == S_CS)
        c->cpl = sel & 3;
    if (s == S_CS || s == S_SS)
        update_mode(c);
}

/* carrega CS para desvios distantes/retornos (sem verificacoes de gates) */
/*
 * Regras de privilegio do CS de destino de IRET/RETF (SDM vol. 2, IRET): RPL >= CPL;
 * segmento nao conforme exige DPL == RPL, conforme exige DPL <= RPL. O NT depende
 * disso: no fim de Ke386CallBios ele executa de proposito um IRET com CS = 0x0b
 * (codigo do kernel com RPL 3) e trata o #GP resultante em KiTrap0D.
 */
static void check_ret_cs(x86_cpu *c, uint16_t sel)
{
    int rpl = sel & 3;
    if ((sel & ~3u) == 0 || rpl < c->cpl)
        x86_gp(c, sel & 0xfffc);
    uint64_t d = x86_read_desc(c, sel, NULL);
    int dpl = (int)((d >> 45) & 3);
    bool code = ((d >> 44) & 1) && ((d >> 43) & 1);
    bool conforming = (d >> 42) & 1;
    if (!code || (conforming ? dpl > rpl : dpl != rpl))
        x86_gp(c, sel & 0xfffc);
}

void x86_load_cs(x86_cpu *c, uint16_t sel, int cpl)
{
    if (!protected_mode(c)) {
        x86_load_seg(c, S_CS, sel);
        return;
    }
    if ((sel & ~3u) == 0)
        x86_gp(c, 0);
    uint64_t d = x86_read_desc(c, sel, NULL);
    if (!((d >> 47) & 1))
        x86_exception(c, EXC_NP, 1, sel & 0xfffc);
    if (!((d >> 44) & 1) || !((d >> 43) & 1))
        x86_gp(c, sel & 0xfffc); /* precisa ser codigo */
    x86_desc_to_seg(&c->seg[S_CS], (uint16_t)((sel & ~3u) | (unsigned)cpl), d);
    c->cpl = cpl;
    update_mode(c);
}

/* ------------------------------------------------------------ pilha */

void x86_push(x86_cpu *c, uint64_t v, int size)
{
    uint64_t sp = c->r[R_SP] - (uint64_t)size;
    if (c->ssz == 2) {
        sp &= 0xffff;
        x86_wr_lin(c, x86_lin(c, S_SS, sp), v, (unsigned)size);
        c->r[R_SP] = (c->r[R_SP] & ~0xffffULL) | sp;
    } else if (c->ssz == 4) {
        sp &= 0xffffffffu;
        x86_wr_lin(c, x86_lin(c, S_SS, sp), v, (unsigned)size);
        c->r[R_SP] = sp;
    } else {
        x86_wr_lin(c, sp, v, (unsigned)size);
        c->r[R_SP] = sp;
    }
}

uint64_t x86_pop(x86_cpu *c, int size)
{
    uint64_t sp = c->r[R_SP];
    uint64_t m = c->ssz == 8 ? ~0ULL : c->ssz == 4 ? 0xffffffffULL : 0xffffULL;
    uint64_t v = x86_rd_lin(c, x86_lin(c, S_SS, sp & m), (unsigned)size);
    sp = (sp & ~m) | ((sp + (uint64_t)size) & m);
    c->r[R_SP] = sp;
    return v;
}

/* leitura da pilha sem alterar RSP (para instrucoes que podem falhar no meio) */
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

/* ------------------------------------------------------ interrupcoes */

static void set_eflags_raw(x86_cpu *c, uint64_t v)
{
    c->eflags = (v & 0x3f7fd7ULL) | 2;
    c->cc_op = CC_NONE;
}

static uint64_t tss_rsp(x86_cpu *c, int n)
{
    return x86_sys_rd(c, c->seg[S_TR].base + 4 + 8 * (uint64_t)n, 8);
}

void x86_load_ldtr(x86_cpu *c, uint16_t sel);

/* ------------------------------------------------------ troca de tarefa */

enum { TS_JMP, TS_CALL, TS_IRET }; /* TS_CALL tambem vale para interrupcoes/excecoes */

/*
 * Troca de tarefa por hardware com TSS de 32 bits (SDM vol. 3, 8.3). Usada pelo
 * Windows/ReactOS de 32 bits no double fault e no NMI (task gates na IDT).
 * ret_eip e o EIP salvo na tarefa antiga.
 */
static void task_switch(x86_cpu *c, uint16_t sel, int reason, bool has_err, uint32_t err, uint32_t ret_eip)
{
    if ((sel & 4) || (sel & ~3u) == 0)
        x86_exception(c, EXC_TS, 1, sel & 0xfffc);
    uint64_t d = x86_read_desc(c, sel, NULL);
    unsigned type = (d >> 40) & 0x1f;
    if (type != 9 && type != 0xb) {
        LOGW("x86: troca de tarefa para descritor tipo %u nao suportada", type);
        x86_exception(c, reason == TS_IRET ? EXC_TS : EXC_GP, 1, sel & 0xfffc);
    }
    if ((reason == TS_IRET) != (type == 0xb))
        x86_exception(c, reason == TS_IRET ? EXC_TS : EXC_GP, 1, sel & 0xfffc);
    if (!((d >> 47) & 1))
        x86_exception(c, EXC_NP, 1, sel & 0xfffc);
    x86_seg nt;
    x86_desc_to_seg(&nt, sel, d);
    if (nt.limit < 0x67)
        x86_exception(c, EXC_TS, 1, sel & 0xfffc);
    uint64_t nb = nt.base, ob = c->seg[S_TR].base;

    /* le a nova tarefa antes de alterar qualquer estado */
    uint32_t n_cr3 = (uint32_t)x86_sys_rd(c, nb + 0x1c, 4);
    uint32_t n_eip = (uint32_t)x86_sys_rd(c, nb + 0x20, 4);
    uint32_t n_fl = (uint32_t)x86_sys_rd(c, nb + 0x24, 4);
    uint32_t n_r[8];
    for (int i = 0; i < 8; i++)
        n_r[i] = (uint32_t)x86_sys_rd(c, nb + 0x28 + 4 * (uint64_t)i, 4);
    uint16_t n_sel[6];
    for (int i = 0; i < 6; i++) /* ES CS SS DS FS GS */
        n_sel[i] = (uint16_t)x86_sys_rd(c, nb + 0x48 + 4 * (uint64_t)i, 2);
    uint16_t n_ldt = (uint16_t)x86_sys_rd(c, nb + 0x60, 2);

    /* salva a tarefa atual */
    uint32_t fl = (uint32_t)x86_get_flags(c);
    if (reason == TS_IRET)
        fl &= ~(uint32_t)EFL_NT;
    x86_sys_wr(c, ob + 0x20, ret_eip, 4);
    x86_sys_wr(c, ob + 0x24, fl, 4);
    for (int i = 0; i < 8; i++)
        x86_sys_wr(c, ob + 0x28 + 4 * (uint64_t)i, (uint32_t)c->r[i], 4);
    static const int segmap[6] = {S_ES, S_CS, S_SS, S_DS, S_FS, S_GS};
    for (int i = 0; i < 6; i++)
        x86_sys_wr(c, ob + 0x48 + 4 * (uint64_t)i, c->seg[segmap[i]].sel, 2);

    uint16_t old_tr = c->seg[S_TR].sel;
    if (reason == TS_JMP || reason == TS_IRET) { /* a tarefa antiga deixa de estar ocupada */
        uint64_t od = x86_sys_rd(c, c->gdt_base + (old_tr & ~7u) + 5, 1);
        x86_sys_wr(c, c->gdt_base + (old_tr & ~7u) + 5, od & ~2u, 1);
    }
    if (reason == TS_CALL) {
        x86_sys_wr(c, nb + 0x00, old_tr, 2); /* back link */
        n_fl |= EFL_NT;
    }
    if (reason != TS_IRET) {
        uint64_t nd = x86_sys_rd(c, c->gdt_base + (sel & ~7u) + 5, 1);
        x86_sys_wr(c, c->gdt_base + (sel & ~7u) + 5, nd | 2u, 1);
    }

    /* carrega a nova tarefa */
    c->seg[S_TR] = nt;
    c->seg[S_TR].attr |= 2;
    c->cr0 |= CR0_TS;
    if (c->cr0 & CR0_PG) {
        c->cr3 = n_cr3;
        x86_tlb_flush(c);
    }
    for (int i = 0; i < 8; i++)
        c->r[i] = n_r[i];
    set_eflags_raw(c, n_fl);
    c->rip = n_eip;
    x86_load_ldtr(c, n_ldt);
    if (n_fl & EFL_VM) {
        for (int i = 0; i < 6; i++)
            x86_load_seg(c, segmap[i], n_sel[i]);
        c->cpl = 3;
    } else {
        c->cpl = n_sel[1] & 3;
        x86_load_seg(c, S_CS, n_sel[1]);
        x86_load_seg(c, S_SS, n_sel[2]);
        x86_load_seg(c, S_ES, n_sel[0]);
        x86_load_seg(c, S_DS, n_sel[3]);
        x86_load_seg(c, S_FS, n_sel[4]);
        x86_load_seg(c, S_GS, n_sel[5]);
    }
    update_mode(c);
    if (has_err)
        x86_push(c, err, (c->seg[S_CS].attr & SEG_DB) ? 4 : 2);
}

static void deliver(x86_cpu *c, int vec, bool sw, bool has_err, uint32_t err, uint64_t ret_rip)
{
    uint64_t fl = x86_get_flags(c);

    if (!(c->cr0 & CR0_PE)) { /* modo real */
        uint16_t off = (uint16_t)x86_sys_rd(c, c->idt_base + (uint64_t)vec * 4, 2);
        uint16_t sel = (uint16_t)x86_sys_rd(c, c->idt_base + (uint64_t)vec * 4 + 2, 2);
        x86_push(c, fl & 0xffff, 2);
        x86_push(c, c->seg[S_CS].sel, 2);
        x86_push(c, ret_rip & 0xffff, 2);
        c->eflags &= ~(uint64_t)(EFL_IF | EFL_TF | EFL_AC | EFL_RF);
        x86_load_seg(c, S_CS, sel);
        c->rip = off;
        return;
    }

    if (c->efer & EFER_LMA) {
        if ((uint32_t)vec * 16 + 15 > c->idt_limit)
            x86_gp(c, (uint32_t)vec * 8 + 2);
        uint64_t lo = x86_sys_rd(c, c->idt_base + (uint64_t)vec * 16, 8);
        uint64_t hi = x86_sys_rd(c, c->idt_base + (uint64_t)vec * 16 + 8, 8);
        unsigned type = (lo >> 40) & 0xf, dpl = (lo >> 45) & 3, ist = (lo >> 32) & 7;
        if (!((lo >> 47) & 1))
            x86_exception(c, EXC_NP, 1, (uint32_t)vec * 8 + 2);
        if (sw && (int)dpl < c->cpl)
            x86_gp(c, (uint32_t)vec * 8 + 2);
        if (type != 0xe && type != 0xf)
            x86_gp(c, (uint32_t)vec * 8 + 2);
        uint64_t target = (lo & 0xffff) | ((lo >> 32) & 0xffff0000ULL) | (hi << 32);
        uint16_t sel = (uint16_t)(lo >> 16);
        uint64_t cd = x86_read_desc(c, sel, NULL);
        int new_cpl = (int)((cd >> 45) & 3);
        uint64_t old_ss = c->seg[S_SS].sel, old_rsp = c->r[R_SP];
        uint64_t rsp;
        if (ist)
            rsp = x86_sys_rd(c, c->seg[S_TR].base + 0x24 + 8 * (uint64_t)(ist - 1), 8);
        else if (new_cpl < c->cpl)
            rsp = tss_rsp(c, new_cpl);
        else
            rsp = c->r[R_SP];
        rsp &= ~0xfULL;
        /* empilha em 64 bits */
        uint64_t frame[6];
        int n = 0;
        if (has_err) frame[n++] = err;
        frame[n++] = ret_rip;
        frame[n++] = c->seg[S_CS].sel;
        frame[n++] = fl;
        frame[n++] = old_rsp;
        frame[n++] = old_ss;
        int save_cpl = c->cpl;
        c->cpl = new_cpl; /* acessos a pilha com o novo privilegio */
        for (int i = n - 1; i >= 0; i--) {
            rsp -= 8;
            x86_write_slow(c, rsp, frame[i], 8, new_cpl == 3);
        }
        c->cpl = save_cpl;
        if (new_cpl < c->cpl) {
            c->seg[S_SS].sel = (uint16_t)new_cpl;
            c->seg[S_SS].base = 0;
            c->seg[S_SS].limit = 0;
            c->seg[S_SS].attr = SEG_P | SEG_S | 3 | ((uint32_t)new_cpl << 5);
        }
        c->r[R_SP] = rsp;
        x86_desc_to_seg(&c->seg[S_CS], (uint16_t)((sel & ~3u) | (unsigned)new_cpl), cd);
        c->cpl = new_cpl;
        update_mode(c);
        c->eflags &= ~(uint64_t)(EFL_TF | EFL_NT | EFL_RF | EFL_VM);
        if (type == 0xe)
            c->eflags &= ~(uint64_t)EFL_IF;
        c->rip = target;
        return;
    }

    /* modo protegido 16/32 bits */
    if ((uint32_t)vec * 8 + 7 > c->idt_limit)
        x86_gp(c, (uint32_t)vec * 8 + 2);
    uint64_t g = x86_sys_rd(c, c->idt_base + (uint64_t)vec * 8, 8);
    unsigned type = (g >> 40) & 0x1f, dpl = (g >> 45) & 3;
    if (!((g >> 47) & 1))
        x86_exception(c, EXC_NP, 1, (uint32_t)vec * 8 + 2);
    if (sw && (int)dpl < c->cpl)
        x86_gp(c, (uint32_t)vec * 8 + 2);
    type &= 0xf;
    if (type == 5) { /* task gate */
        task_switch(c, (uint16_t)(g >> 16), TS_CALL, has_err, err, (uint32_t)ret_rip);
        return;
    }
    bool gate32 = type & 8;
    uint32_t target = (uint32_t)((g & 0xffff) | ((g >> 32) & 0xffff0000u));
    uint16_t sel = (uint16_t)(g >> 16);
    uint64_t cd = x86_read_desc(c, sel, NULL);
    int new_cpl = (int)((cd >> 45) & 3);
    bool from_vm = c->eflags & EFL_VM;
    int psz = gate32 ? 4 : 2;
    if (new_cpl < c->cpl || from_vm) {
        /* troca de pilha pela TSS de 32 bits */
        uint64_t tb = c->seg[S_TR].base;
        uint32_t nesp;
        uint16_t nss;
        if (c->seg[S_TR].attr & 8) { /* TSS 32 */
            nesp = (uint32_t)x86_sys_rd(c, tb + 4 + 8 * (uint64_t)new_cpl, 4);
            nss = (uint16_t)x86_sys_rd(c, tb + 8 + 8 * (uint64_t)new_cpl, 2);
        } else {
            nesp = (uint32_t)x86_sys_rd(c, tb + 2 + 4 * (uint64_t)new_cpl, 2);
            nss = (uint16_t)x86_sys_rd(c, tb + 4 + 4 * (uint64_t)new_cpl, 2);
        }
        uint64_t ssd = x86_read_desc(c, nss, NULL);
        uint32_t old_ss = c->seg[S_SS].sel, old_esp = (uint32_t)c->r[R_SP];
        uint16_t old_es = c->seg[S_ES].sel, old_ds = c->seg[S_DS].sel, old_fs = c->seg[S_FS].sel, old_gs = c->seg[S_GS].sel;
        x86_seg nssg;
        x86_desc_to_seg(&nssg, nss, ssd);
        int nssz = (nssg.attr & SEG_DB) ? 4 : 2;
        uint64_t m = nssz == 4 ? 0xffffffffULL : 0xffffULL;
        uint64_t esp = nesp;
        uint64_t frame[10];
        int n = 0;
        if (has_err) frame[n++] = err;
        frame[n++] = ret_rip;
        frame[n++] = c->seg[S_CS].sel;
        frame[n++] = fl;
        frame[n++] = old_esp;
        frame[n++] = old_ss;
        if (from_vm) {
            frame[n++] = old_es;
            frame[n++] = old_ds;
            frame[n++] = old_fs;
            frame[n++] = old_gs;
        }
        for (int i = n - 1; i >= 0; i--) {
            esp = (esp - (uint64_t)psz) & m;
            x86_write_slow(c, (uint32_t)(nssg.base + esp), frame[i], (unsigned)psz, 0);
        }
        c->seg[S_SS] = nssg;
        c->r[R_SP] = (c->r[R_SP] & ~m) | esp;
        if (from_vm) {
            for (int s = 0; s < 6; s++)
                if (s != S_CS && s != S_SS) {
                    c->seg[s].sel = 0;
                    c->seg[s].base = 0;
                    c->seg[s].attr = 0;
                }
            c->eflags &= ~(uint64_t)EFL_VM;
        }
    } else {
        if (has_err) {
            x86_push(c, fl, psz);
            x86_push(c, c->seg[S_CS].sel, psz);
            x86_push(c, ret_rip, psz);
            x86_push(c, err, psz);
        } else {
            x86_push(c, fl, psz);
            x86_push(c, c->seg[S_CS].sel, psz);
            x86_push(c, ret_rip, psz);
        }
    }
    x86_desc_to_seg(&c->seg[S_CS], (uint16_t)((sel & ~3u) | (unsigned)new_cpl), cd);
    c->cpl = new_cpl;
    update_mode(c);
    c->eflags &= ~(uint64_t)(EFL_TF | EFL_NT | EFL_RF | EFL_VM);
    if (!(type & 1))
        c->eflags &= ~(uint64_t)EFL_IF;
    c->rip = gate32 ? target : (target & 0xffff);
}

static void trace_dump(x86_cpu *c, const char *why);
static bool dbg_read(x86_cpu *c, uint64_t lin, void *out, size_t n);

static bool exc_has_err(int vec)
{
    return vec == EXC_DF || vec == EXC_TS || vec == EXC_NP || vec == EXC_SS || vec == EXC_GP ||
           vec == EXC_PF || vec == EXC_AC;
}

_Noreturn void x86_exception(x86_cpu *c, int vec, int has_err, uint32_t err)
{
    if (!(c->cr0 & CR0_PE))
        has_err = 0;
    if ((vec == EXC_UD || vec == EXC_GP) && (c->cr0 & CR0_PE)) {
        char b[64];
        int o = 0;
        for (int i = -6; i < 6; i++) {
            uint8_t *h = x86_host_ptr_nofault(c, x86_lin(c, S_CS, c->cur_rip) + (uint64_t)(int64_t)i);
            o += snprintf(b + o, sizeof(b) - (size_t)o, i == 0 ? "[%02x]" : "%02x ", h ? *h : 0);
        }
        LOGD("x86: excecao %d em %llx err=%x rax=%llx ecx=%llx bytes %s", vec, (unsigned long long)c->cur_rip, err, (unsigned long long)c->r[R_AX],
             (unsigned long long)c->r[R_CX], b);
    }
    c->exc_depth++;
    if (c->exc_depth >= 3) {
        LOGE("x86: falha tripla (vetor %d em 0x%llx) - reiniciando", vec, (unsigned long long)c->cur_rip);
        if (c->trace && !c->trace_dumped) {
            c->trace_dumped = true;
            trace_dump(c, "falha tripla");
        }
        c->exc_depth = 0;
        vm_request_guest_reset(c->vm);
        c->halted = true;
        longjmp(c->jb, 1);
    }
    if (c->exc_depth == 2 && vec != EXC_PF && vec != EXC_DF) {
        LOGD("x86: excecao %d (err=%x) na entrega de outro evento em %04x:%llx vm=%d -> #DF", vec, err,
             c->seg[S_CS].sel, (unsigned long long)c->cur_rip, !!(c->eflags & EFL_VM));
        vec = EXC_DF;
        has_err = 1;
        err = 0;
    }
    if (vec != EXC_PF || (c->eflags & EFL_VM) || c->exc_depth > 1)
        LOGD("x86: excecao %d err=%x em %04x:%llx cr0=%llx cr2=%llx idt=%llx/%x prof=%d vm=%d esp=%llx", vec, err,
             c->seg[S_CS].sel, (unsigned long long)c->cur_rip, (unsigned long long)c->cr0, (unsigned long long)c->cr2,
             (unsigned long long)c->idt_base, c->idt_limit, c->exc_depth, !!(c->eflags & EFL_VM),
             (unsigned long long)c->r[R_SP]);
    if (c->trace && !c->trace_dumped && vec == EXC_UD && c->cpl == 0) {
        c->trace_dumped = true;
        trace_dump(c, "#UD em modo kernel");
    }
    /* restaura RSP/RIP ao inicio da instrucao */
    c->rip = c->cur_rip;
    deliver(c, vec, false, has_err && exc_has_err(vec), err, c->cur_rip);
    c->exc_depth = 0;
    longjmp(c->jb, 1);
}

/* Depuracao de kernels NT: INT 2Dh com EAX=1 e o DbgPrint (ECX = texto, EDX = tamanho). */
static void nt_dbgprint(x86_cpu *c)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MVM_X86_DBGPRINT") != NULL;
    if (!on || (uint32_t)c->r[R_AX] != 1)
        return;
    char buf[512];
    uint32_t n = (uint32_t)c->r[R_DX];
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    uint64_t p = c->code64 ? c->r[R_CX] : (uint32_t)c->r[R_CX];
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *h = x86_host_ptr(c, p + i, false);
        buf[i] = h ? (char)*h : '?';
        if (buf[i] == 0) { n = i; break; }
    }
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = 0;
    LOGI("DbgPrint: %s", buf);
}

void x86_sw_interrupt(x86_cpu *c, int vec, uint64_t ret_rip)
{
    if (vec == 0x2d)
        nt_dbgprint(c);
    if ((c->eflags & EFL_VM) && ((c->eflags >> 12) & 3) < 3)
        x86_gp(c, 0);
    /* INT n/INT3/INTO/ICEBP sao instrucoes: uma falha na entrega (DPL do gate, pilha,
     * pagina ausente) e uma falha comum da instrucao, nunca um double fault */
    deliver(c, vec, true, false, 0, ret_rip);
}

void x86_iret(x86_cpu *c, int osz)
{
    if (!(c->cr0 & CR0_PE)) {
        uint64_t ip = x86_pop(c, osz), cs = x86_pop(c, osz), fl = x86_pop(c, osz);
        x86_load_seg(c, S_CS, (uint16_t)cs);
        c->rip = osz == 2 ? ip & 0xffff : ip;
        uint64_t mask = osz == 2 ? 0xffff : 0x257fd5;
        set_eflags_raw(c, (c->eflags & ~mask) | (fl & mask));
        return;
    }
    if (c->eflags & EFL_VM) { /* IRET em VM86 com IOPL 3 */
        if (((c->eflags >> 12) & 3) != 3)
            x86_gp(c, 0);
        uint64_t ip = x86_pop(c, osz), cs = x86_pop(c, osz), fl = x86_pop(c, osz);
        x86_load_seg(c, S_CS, (uint16_t)cs);
        c->rip = ip & 0xffff;
        uint64_t mask = osz == 2 ? 0xcfff : 0x1c4fff & ~(uint64_t)EFL_IOPL;
        set_eflags_raw(c, (c->eflags & ~mask) | (fl & mask));
        return;
    }
    if ((c->eflags & EFL_NT) && !(c->efer & EFER_LMA)) { /* volta para a tarefa anterior */
        uint16_t link = (uint16_t)x86_sys_rd(c, c->seg[S_TR].base, 2);
        task_switch(c, link, TS_IRET, false, 0, (uint32_t)c->rip);
        return;
    }
    uint64_t ip = stack_peek(c, 0, osz);
    uint16_t cs = (uint16_t)stack_peek(c, (uint64_t)osz, osz);
    uint64_t fl = stack_peek(c, 2 * (uint64_t)osz, osz);
    int rpl = cs & 3;
    if (rpl < c->cpl)
        x86_gp(c, cs & 0xfffc);
    int old_cpl = c->cpl;
    if (!(c->efer & EFER_LMA) && osz == 4 && (fl & EFL_VM) && c->cpl == 0) {
        /* retorno para VM86 */
        uint32_t esp = (uint32_t)stack_peek(c, 12, 4);
        uint16_t ss = (uint16_t)stack_peek(c, 16, 4), es = (uint16_t)stack_peek(c, 20, 4);
        uint16_t ds = (uint16_t)stack_peek(c, 24, 4), fs = (uint16_t)stack_peek(c, 28, 4), gs = (uint16_t)stack_peek(c, 32, 4);
        set_eflags_raw(c, fl);
        c->cpl = 3;
        x86_load_seg(c, S_CS, cs);
        x86_load_seg(c, S_SS, ss);
        x86_load_seg(c, S_ES, es);
        x86_load_seg(c, S_DS, ds);
        x86_load_seg(c, S_FS, fs);
        x86_load_seg(c, S_GS, gs);
        c->r[R_SP] = esp;
        c->rip = ip & 0xffff;
        return;
    }
    check_ret_cs(c, cs);
    bool outer = rpl > c->cpl || (c->efer & EFER_LMA && c->code64);
    uint64_t nsp = 0;
    uint16_t nss = 0;
    if (outer) {
        nsp = stack_peek(c, 3 * (uint64_t)osz, osz);
        nss = (uint16_t)stack_peek(c, 4 * (uint64_t)osz, osz);
    }
    x86_load_cs(c, cs, rpl);
    c->rip = osz == 2 ? ip & 0xffff : osz == 4 ? (uint32_t)ip : ip;
    uint64_t mask = EFL_ARITH | EFL_TF | EFL_DF | EFL_NT | EFL_RF | EFL_AC | EFL_ID;
    if (old_cpl == 0) mask |= EFL_IOPL | EFL_VIF | EFL_VIP;
    if (old_cpl <= (int)((c->eflags >> 12) & 3)) mask |= EFL_IF;
    if (osz == 2) mask &= 0xffff;
    set_eflags_raw(c, (c->eflags & ~mask) | (fl & mask));
    if (outer) {
        if ((nss & ~3u) == 0 && (c->efer & EFER_LMA) && rpl != 3) {
            c->seg[S_SS].sel = nss;
            c->seg[S_SS].base = 0;
            c->seg[S_SS].attr = SEG_P | SEG_S | 3 | ((uint32_t)rpl << 5);
            update_mode(c);
        } else {
            x86_load_seg(c, S_SS, nss);
        }
        c->r[R_SP] = osz == 4 ? (uint32_t)nsp : osz == 2 ? ((c->r[R_SP] & ~0xffffULL) | (nsp & 0xffff)) : nsp;
        if (rpl > old_cpl) {
            /* invalida segmentos de dados com DPL < CPL */
            for (int s = S_ES; s <= S_GS; s++) {
                if (s == S_CS || s == S_SS) continue;
                x86_seg *sg = &c->seg[s];
                bool conforming_code = (sg->attr & 0x1c) == 0x1c;
                if (!conforming_code && (int)SEG_DPL(sg->attr) < rpl && (sg->sel & ~3u)) {
                    sg->sel = 0;
                    sg->attr = 0;
                    sg->base = 0;
                }
            }
        }
    } else {
        stack_add(c, 3 * (uint64_t)osz);
    }
}

void x86_far_ret(x86_cpu *c, int osz, uint16_t imm)
{
    uint64_t ip = stack_peek(c, 0, osz);
    uint16_t cs = (uint16_t)stack_peek(c, (uint64_t)osz, osz);
    if (!protected_mode(c)) {
        stack_add(c, 2 * (uint64_t)osz + imm);
        x86_load_seg(c, S_CS, cs);
        c->rip = osz == 2 ? ip & 0xffff : ip;
        return;
    }
    int rpl = cs & 3;
    check_ret_cs(c, cs);
    if (rpl > c->cpl) {
        uint64_t nsp = stack_peek(c, 2 * (uint64_t)osz + imm, osz);
        uint16_t nss = (uint16_t)stack_peek(c, 3 * (uint64_t)osz + imm, osz);
        x86_load_cs(c, cs, rpl);
        x86_load_seg(c, S_SS, nss);
        c->r[R_SP] = nsp + imm;
    } else {
        x86_load_cs(c, cs, c->cpl);
        stack_add(c, 2 * (uint64_t)osz + imm);
    }
    c->rip = osz == 2 ? ip & 0xffff : osz == 4 ? (uint32_t)ip : ip;
}

void x86_far_jump(x86_cpu *c, uint16_t sel, uint64_t off, int osz, bool call)
{
    uint16_t old_cs = c->seg[S_CS].sel;
    uint64_t old_ip = c->rip;
    if (protected_mode(c)) {
        uint64_t d = x86_read_desc(c, sel, NULL);
        if (!((d >> 44) & 1)) {
            unsigned type = (d >> 40) & 0xf;
            if (!(c->efer & EFER_LMA) && (type == 9 || type == 5)) { /* TSS ou task gate */
                uint16_t tss = type == 5 ? (uint16_t)(d >> 16) : sel;
                task_switch(c, tss, call ? TS_CALL : TS_JMP, false, 0, (uint32_t)c->rip);
                return;
            }
            LOGW("x86: desvio distante para gate tipo %u nao suportado", type);
            x86_gp(c, sel & 0xfffc);
        }
        x86_load_cs(c, sel, c->cpl);
    } else {
        x86_load_seg(c, S_CS, sel);
    }
    if (call) {
        x86_push(c, old_cs, osz);
        x86_push(c, old_ip, osz);
    }
    c->rip = osz == 2 ? off & 0xffff : osz == 4 ? (uint32_t)off : off;
}

/* --------------------------------------------------- registradores de controle */

void x86_write_cr(x86_cpu *c, int n, uint64_t v)
{
    switch (n) {
    case 0: {
        v |= 0x10; /* ET */
        if ((v & CR0_PG) && !(v & CR0_PE))
            x86_gp(c, 0);
        uint64_t old = c->cr0;
        c->cr0 = v;
        if ((v & CR0_PG) && !(old & CR0_PG) && (c->efer & EFER_LME)) {
            if (!(c->cr4 & CR4_PAE))
                x86_gp(c, 0);
            c->efer |= EFER_LMA;
        }
        if (!(v & CR0_PG) && (old & CR0_PG))
            c->efer &= ~(uint64_t)EFER_LMA;
        /* o Windows liga CR0.TS a cada troca de thread (FPU preguicosa): so os bits que
         * mudam a traducao esvaziam o TLB */
        if ((old ^ v) & (CR0_PE | CR0_WP | CR0_PG))
            x86_tlb_flush(c);
        update_mode(c);
        break;
    }
    case 2: c->cr2 = v; break;
    case 3:
        c->cr3 = v;
        if (c->cr4 & CR4_PGE)
            x86_tlb_flush_nonglobal(c);
        else
            x86_tlb_flush(c);
        break;
    case 4:
        if (!(v & CR4_PAE) && (c->efer & EFER_LMA))
            x86_gp(c, 0);
        /* PSE, PAE, PGE (o Windows alterna PGE para esvaziar paginas globais), PCIDE, SMEP, SMAP, PKE */
        if ((c->cr4 ^ v) & (CR4_PSE | CR4_PAE | CR4_PGE | (1u << 17) | (1u << 20) | (1u << 21) | (1u << 22)))
            x86_tlb_flush(c);
        c->cr4 = v;
        break;
    case 8:
        c->cr8 = v & 15;
        if (c->apic)
            c->apic_set_tpr(c->apic, (uint8_t)((v & 15) << 4));
        break;
    default: x86_ud(c);
    }
}

uint64_t x86_read_cr(x86_cpu *c, int n)
{
    switch (n) {
    case 0: return c->cr0;
    case 2: return c->cr2;
    case 3: return c->cr3;
    case 4: return c->cr4;
    case 8: return c->apic ? (uint64_t)(c->apic_get_tpr(c->apic) >> 4) : c->cr8;
    default: x86_ud(c);
    }
}

/* ---------------------------------------------------------- CPUID/MSR */

uint64_t x86_tsc(x86_cpu *c) { return (uint64_t)mvm_now(c->vm) + c->tsc_offset; } /* 1 GHz */

/* 64 bits aleatorios do host (RDRAND/RDSEED), com buffer para nao chamar o kernel toda vez */
uint64_t x86_host_random(void)
{
    static __thread uint64_t buf[32];
    static __thread int left;
    if (!left) {
        /* syscall direto: a libc do Android < 9 nao tem getrandom() */
        if (syscall(SYS_getrandom, buf, sizeof(buf), 0) != (long)sizeof(buf)) {
            static __thread uint64_t s = 0x9e3779b97f4a7c15ULL;
            for (int i = 0; i < 32; i++) { /* splitmix64 de reserva */
                uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
                z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
                z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
                buf[i] = z ^ (z >> 31);
            }
        }
        left = 32;
    }
    return buf[--left];
}

void x86_cpuid(x86_cpu *c)
{
    uint32_t leaf = (uint32_t)c->r[R_AX], sub = (uint32_t)c->r[R_CX];
    uint32_t a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0:
        a = 0x16;
        b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; /* GenuineIntel */
        break;
    case 1:
        a = 0x000006f1; /* familia 6, modelo 15 */
        b = (8 << 8) | (1 << 16);
        /* SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT */
        cc = (1u << 0) | (1u << 9) | (1u << 19) | (1u << 20) | (1u << 23);
        cc |= 1u << 30; /* RDRAND: o Linux semeia o gerador aleatorio sem esperar entropia */
        if (c->cpuid_lm)
            cc |= 1u << 13; /* CMPXCHG16B (exigido pelo Windows 8.1+ x64) */
        /* FPU DE PSE TSC MSR PAE MCE CX8 SEP MTRR PGE MCA CMOV PAT CLFSH MMX FXSR SSE SSE2 */
        d = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) |
            (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16) | (1u << 19) |
            (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
        if (c->apic)
            d |= 1u << 9; /* APIC (ID 0 em EBX[31:24]) */
        break;
    case 2:
        /* descritores de cache; AL = 1 (numero de chamadas necessarias: CPUs reais
         * sempre devolvem 1 e ha sistemas que repetem a instrucao AL vezes) */
        a = 0x00302c01; /* 0x2C: L1D 32K 8-way; 0x30: L1I 32K 8-way */
        d = 0x0000007d; /* 0x7D: L2 2M 8-way, linha de 64 bytes */
        break;
    case 4: {
        /* parametros deterministicos de cache (coerentes com a folha 2): o Windows monta
         * KeQueryLogicalProcessorRelationship(RelationCache) a partir daqui */
        static const struct { uint8_t type, level; uint32_t ways, sets; } cache[] = {
            {1, 1, 8, 64}, {2, 1, 8, 64}, {3, 2, 8, 4096}, /* L1D 32K, L1I 32K, L2 2M; linha de 64 */
        };
        if (sub < 3) {
            a = cache[sub].type | ((uint32_t)cache[sub].level << 5) | (1u << 8); /* autoinicializavel */
            b = ((cache[sub].ways - 1) << 22) | (63u); /* 1 particao, linha de 64 bytes */
            cc = cache[sub].sets - 1;
            d = 0;
        }
        break;
    }
    case 7:
        if (sub == 0) {
            b = (1u << 1) | (1u << 18); /* IA32_TSC_ADJUST, RDSEED */
            d = 1u << 29; /* IA32_ARCH_CAPABILITIES */
        }
        break;
    case 0x15:
        /* TSC = cristal * EBX / EAX. O Linux tambem assume que o timer do APIC local
         * roda na frequencia do cristal (e pula a calibracao): ele precisa ser igual
         * ao barramento do APIC emulado, 1 GHz, como o TSC. */
        a = 1;
        b = 1;
        cc = 1000000000;
        break;
    case 0x16: /* frequencias base/maxima/barramento em MHz */
        a = 1000;
        b = 1000;
        cc = 100;
        break;
    case 0x80000007:
        d = 1u << 8; /* TSC invariante: o TSC segue o relogio monotonico do host */
        break;
    case 0x80000000:
        a = 0x80000008;
        break;
    case 0x80000001:
        if (c->cpuid_lm) {
            d = (1u << 11) | (1u << 20) | (1u << 27) | (1u << 29); /* SYSCALL NX RDTSCP LM */
            cc = 1 | (1u << 8); /* LAHF/SAHF em 64 bits, PREFETCHW */
        } else {
            d = 1u << 20;
        }
        break;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        static const char brand[48] = "MultiVM Virtual CPU";
        uint32_t w[4];
        memcpy(w, brand + (leaf - 0x80000002) * 16, 16);
        a = w[0]; b = w[1]; cc = w[2]; d = w[3];
        break;
    }
    case 0x80000008:
        a = c->cpuid_lm ? 0x3028 : 0x2020;
        break;
    default:
        break;
    }
    c->r[R_AX] = a;
    c->r[R_BX] = b;
    c->r[R_CX] = cc;
    c->r[R_DX] = d;
}

void x86_rdmsr(x86_cpu *c)
{
    uint32_t msr = (uint32_t)c->r[R_CX];
    uint64_t v;
    switch (msr) {
    case 0x10: v = x86_tsc(c); break;
    case 0x17: v = 0; break;
    case 0x3a: v = 1; break; /* IA32_FEATURE_CONTROL travado */
    case 0x8b: v = 0; break;
    case 0x174: v = c->sysenter_cs; break;
    case 0x175: v = c->sysenter_esp; break;
    case 0x176: v = c->sysenter_eip; break;
    case 0x1a0: v = c->misc_enable; break;
    case 0x277: v = c->pat; break;
    case 0xc0000080: v = c->efer; break;
    case 0xc0000081: v = c->star; break;
    case 0xc0000082: v = c->lstar; break;
    case 0xc0000083: v = c->cstar; break;
    case 0xc0000084: v = c->sfmask; break;
    case 0xc0000100: v = c->seg[S_FS].base; break;
    case 0xc0000101: v = c->seg[S_GS].base; break;
    case 0xc0000102: v = c->kernel_gs_base; break;
    case 0xc0000103: v = c->tsc_aux; break;
    case 0x1b:
        if (!c->apic) x86_gp(c, 0);
        v = c->apic_get_base(c->apic);
        break;
    case 0x3b: v = c->tsc_adjust; break;
    case 0xfe: v = 0x508; break; /* MTRRcap: 8 variaveis, fixos, WC */
    case 0x10a: /* IA32_ARCH_CAPABILITIES: CPU emulada sem Meltdown/L1TF/SSB/MDS/TAA (dispensa KPTI) */
        v = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8);
        break;
    case 0x2ff: v = c->mtrr_def; break;
    case 0x200 ... 0x20f: v = c->mtrr_var[msr - 0x200]; break;
    case 0x250: v = c->mtrr_fix[0]; break;
    case 0x258: case 0x259: v = c->mtrr_fix[msr - 0x257]; break;
    case 0x268 ... 0x26f: v = c->mtrr_fix[msr - 0x268 + 3]; break;
    case 0x179: v = 1; break; /* MCG_CAP: 1 banco, sem MCG_CTL */
    case 0x17a: v = c->mcg_status; break;
    case 0x400 ... 0x403: v = c->mc_bank[msr - 0x400]; break;
    default:
        LOGD("x86: RDMSR desconhecido 0x%x", msr);
        x86_gp(c, 0);
    }
    c->r[R_AX] = (uint32_t)v;
    c->r[R_DX] = (uint32_t)(v >> 32);
}

void x86_wrmsr(x86_cpu *c)
{
    uint32_t msr = (uint32_t)c->r[R_CX];
    uint64_t v = ((uint64_t)(uint32_t)c->r[R_DX] << 32) | (uint32_t)c->r[R_AX];
    switch (msr) {
    case 0x10: {
        uint64_t old = x86_tsc(c);
        c->tsc_offset = v - (uint64_t)mvm_now(c->vm);
        c->tsc_adjust += v - old; /* escrever no TSC tambem ajusta IA32_TSC_ADJUST */
        break;
    }
    case 0x3b:
        c->tsc_offset += v - c->tsc_adjust;
        c->tsc_adjust = v;
        break;
    case 0x8b: case 0x79: break;
    case 0x174: c->sysenter_cs = v & 0xffff; break;
    case 0x175: c->sysenter_esp = v; break;
    case 0x176: c->sysenter_eip = v; break;
    case 0x1a0: c->misc_enable = v; break;
    case 0x277: c->pat = v; break;
    case 0xc0000080: {
        uint64_t allowed = EFER_SCE | EFER_NXE | (c->cpuid_lm ? EFER_LME : 0);
        if (v & ~allowed & ~(uint64_t)EFER_LMA)
            x86_gp(c, 0);
        if (((v ^ c->efer) & EFER_LME) && (c->cr0 & CR0_PG))
            x86_gp(c, 0);
        uint64_t nefer = (v & allowed) | (c->efer & EFER_LMA);
        if ((nefer ^ c->efer) & (EFER_NXE | EFER_LME | EFER_LMA))
            x86_tlb_flush(c);
        c->efer = nefer;
        break;
    }
    case 0xc0000081: c->star = v; break;
    case 0xc0000082: c->lstar = v; break;
    case 0xc0000083: c->cstar = v; break;
    case 0xc0000084: c->sfmask = v; break;
    case 0xc0000100: c->seg[S_FS].base = v; break;
    case 0xc0000101: c->seg[S_GS].base = v; break;
    case 0xc0000102: c->kernel_gs_base = v; break;
    case 0xc0000103: c->tsc_aux = v; break;
    case 0x1b:
        if (!c->apic) x86_gp(c, 0);
        c->apic_set_base(c->apic, v);
        break;
    case 0x2ff: c->mtrr_def = v & 0xcff; break;
    case 0x200 ... 0x20f: c->mtrr_var[msr - 0x200] = v; break;
    case 0x250: c->mtrr_fix[0] = v; break;
    case 0x258: case 0x259: c->mtrr_fix[msr - 0x257] = v; break;
    case 0x268 ... 0x26f: c->mtrr_fix[msr - 0x268 + 3] = v; break;
    case 0x17a: c->mcg_status = v; break;
    case 0x400 ... 0x403: c->mc_bank[msr - 0x400] = v; break;
    default:
        LOGD("x86: WRMSR desconhecido 0x%x = 0x%llx", msr, (unsigned long long)v);
        x86_gp(c, 0);
    }
}

/* ------------------------------------------------ SYSCALL/SYSENTER */

static void set_flat(x86_seg *s, uint16_t sel, bool code, bool l, int dpl)
{
    s->sel = sel;
    s->base = 0;
    s->limit = 0xffffffffu;
    s->attr = SEG_P | SEG_S | SEG_G | ((uint32_t)dpl << 5) | (code ? 0xb : 0x3) | (l ? SEG_L : SEG_DB);
}

/* MVM_X86_SYSCALL_LOG=num,num,... (hex): registra essas syscalls (Windows x64) com o
 * processo, os argumentos e os enderecos de retorno do modo usuario simbolizados */
static void syscall_log(x86_cpu *c)
{
    static int init;
    static uint32_t nums[32];
    static int nnums;
    if (!init++) {
        const char *e = getenv("MVM_X86_SYSCALL_LOG");
        while (e && *e && nnums < 32) {
            nums[nnums++] = (uint32_t)strtoul(e, (char **)&e, 16);
            if (*e == ',')
                e++;
        }
    }
    if (!nnums || c->cpl != 3 || !(c->efer & EFER_LMA))
        return;
    uint32_t n = (uint32_t)c->r[R_AX];
    int k;
    for (k = 0; k < nnums && nums[k] != n; k++)
        ;
    if (k == nnums)
        return;
    char proc[16] = "?";
    uint64_t thread = 0, process = 0;
    if (dbg_read(c, c->kernel_gs_base + 0x188, &thread, 8) && dbg_read(c, thread + 0xb8, &process, 8))
        dbg_read(c, process + 0x5a8, proc, 15); /* EPROCESS.ImageFileName (build 19041) */
    char sym[192];
    x86_debug_symbolize(c, c->rip, sym, sizeof(sym));
    LOGI("x86: syscall 0x%x em %s: args %llx %llx %llx %llx, retorno %llx %s", n, proc,
         (unsigned long long)c->r[10], (unsigned long long)c->r[R_DX], (unsigned long long)c->r[8],
         (unsigned long long)c->r[9], (unsigned long long)c->rip, sym);
    for (int i = 0; i < 512; i++) { /* enderecos de retorno no modo usuario */
        uint64_t v;
        if (!dbg_read(c, c->r[R_SP] + 8 * (uint64_t)i, &v, 8))
            break;
        if (v < 0x10000 || v >= 0x800000000000ULL)
            continue;
        x86_debug_symbolize(c, v, sym, sizeof(sym));
        if (sym[0])
            LOGI("x86:   [rsp+%03x] %llx = %s", 8 * i, (unsigned long long)v, sym);
    }
}

void x86_syscall(x86_cpu *c)
{
    syscall_log(c);
    if (!(c->efer & EFER_SCE))
        x86_ud(c);
    uint16_t cs = (uint16_t)((c->star >> 32) & 0xfffc);
    if (c->efer & EFER_LMA) {
        c->r[R_CX] = c->rip;
        c->r[11] = x86_get_flags(c) & ~(uint64_t)EFL_RF;
        bool was64 = c->code64;
        set_flat(&c->seg[S_CS], cs, true, true, 0);
        set_flat(&c->seg[S_SS], (uint16_t)(cs + 8), false, false, 0);
        c->cpl = 0;
        update_mode(c);
        set_eflags_raw(c, x86_get_flags(c) & ~c->sfmask & ~(uint64_t)EFL_RF);
        c->rip = was64 ? c->lstar : c->cstar;
    } else {
        c->r[R_CX] = (uint32_t)c->rip;
        set_flat(&c->seg[S_CS], cs, true, false, 0);
        set_flat(&c->seg[S_SS], (uint16_t)(cs + 8), false, false, 0);
        c->cpl = 0;
        update_mode(c);
        c->eflags &= ~(uint64_t)(EFL_IF | EFL_RF | EFL_VM);
        c->rip = (uint32_t)c->star;
    }
}

void x86_sysret(x86_cpu *c, bool rex_w)
{
    if (!(c->efer & EFER_SCE))
        x86_ud(c);
    if (c->cpl != 0 || !protected_mode(c))
        x86_gp(c, 0);
    uint16_t base = (uint16_t)((c->star >> 48) & 0xfffc);
    if (c->efer & EFER_LMA) {
        if (rex_w) {
            set_flat(&c->seg[S_CS], (uint16_t)((base + 16) | 3), true, true, 3);
            c->rip = c->r[R_CX];
        } else {
            set_flat(&c->seg[S_CS], (uint16_t)(base | 3), true, false, 3);
            c->rip = (uint32_t)c->r[R_CX];
        }
        set_eflags_raw(c, (c->r[11] & 0x3c7fd7ULL) | 2);
    } else {
        set_flat(&c->seg[S_CS], (uint16_t)(base | 3), true, false, 3);
        c->rip = (uint32_t)c->r[R_CX];
        c->eflags |= EFL_IF;
    }
    set_flat(&c->seg[S_SS], (uint16_t)((base + 8) | 3), false, false, 3);
    c->cpl = 3;
    update_mode(c);
}

void x86_sysenter(x86_cpu *c)
{
    if (!protected_mode(c) || !(c->sysenter_cs & 0xfffc))
        x86_gp(c, 0);
    uint16_t cs = (uint16_t)(c->sysenter_cs & 0xfffc);
    bool lm = c->efer & EFER_LMA;
    set_flat(&c->seg[S_CS], cs, true, lm, 0);
    set_flat(&c->seg[S_SS], (uint16_t)(cs + 8), false, false, 0);
    c->cpl = 0;
    update_mode(c);
    c->eflags &= ~(uint64_t)(EFL_IF | EFL_VM | EFL_RF);
    c->r[R_SP] = lm ? c->sysenter_esp : (uint32_t)c->sysenter_esp;
    c->rip = lm ? c->sysenter_eip : (uint32_t)c->sysenter_eip;
}

void x86_sysexit(x86_cpu *c, bool rex_w)
{
    if (!protected_mode(c) || c->cpl != 0 || !(c->sysenter_cs & 0xfffc))
        x86_gp(c, 0);
    uint16_t base = (uint16_t)(c->sysenter_cs & 0xfffc);
    if (rex_w) {
        set_flat(&c->seg[S_CS], (uint16_t)((base + 32) | 3), true, true, 3);
        set_flat(&c->seg[S_SS], (uint16_t)((base + 40) | 3), false, false, 3);
        c->r[R_SP] = c->r[R_CX];
        c->rip = c->r[R_DX];
    } else {
        set_flat(&c->seg[S_CS], (uint16_t)((base + 16) | 3), true, false, 3);
        set_flat(&c->seg[S_SS], (uint16_t)((base + 24) | 3), false, false, 3);
        c->r[R_SP] = (uint32_t)c->r[R_CX];
        c->rip = (uint32_t)c->r[R_DX];
    }
    c->cpl = 3;
    update_mode(c);
}

/* ------------------------------------------------- tabelas de descritores */

void x86_load_ldtr(x86_cpu *c, uint16_t sel)
{
    if ((sel & ~3u) == 0) {
        c->seg[S_LDTR].sel = 0;
        c->seg[S_LDTR].base = 0;
        c->seg[S_LDTR].limit = 0;
        c->seg[S_LDTR].attr = 0;
        return;
    }
    if (sel & 4)
        x86_gp(c, sel & 0xfffc);
    uint64_t hi, d = x86_read_desc(c, sel, &hi);
    if (((d >> 40) & 0x1f) != 2)
        x86_gp(c, sel & 0xfffc);
    x86_desc_to_seg(&c->seg[S_LDTR], sel, d);
    if (c->efer & EFER_LMA)
        c->seg[S_LDTR].base |= (hi & 0xffffffffULL) << 32;
}

void x86_load_tr(x86_cpu *c, uint16_t sel)
{
    if ((sel & ~3u) == 0 || (sel & 4))
        x86_gp(c, sel & 0xfffc);
    uint64_t hi, d = x86_read_desc(c, sel, &hi);
    unsigned type = (d >> 40) & 0x1f;
    if (type != 9 && type != 1 && type != 0xb && type != 3)
        x86_gp(c, sel & 0xfffc);
    x86_desc_to_seg(&c->seg[S_TR], sel, d);
    if (c->efer & EFER_LMA)
        c->seg[S_TR].base |= (hi & 0xffffffffULL) << 32;
    /* marca como ocupado */
    uint64_t base = c->gdt_base + (sel & ~7u);
    x86_sys_wr(c, base + 5, (uint8_t)((d >> 40) | 2), 1);
    c->seg[S_TR].attr |= 2;
}

/* ------------------------------------------------------------ execucao */

void x86_exec_one(x86_cpu *c);

static void trace_dump(x86_cpu *c, const char *why)
{
    const char *path = getenv("MVM_X86_TRACE_FILE");
    FILE *f = fopen(path ? path : "mvm-x86-trace.txt", "w");
    if (!f)
        return;
    fprintf(f, "# %s\n", why);
    for (unsigned i = 0; i < c->trace_n; i++) {
        struct x86_trace *t = &c->trace[(c->trace_pos + i) % c->trace_n];
        if (!t->rip && !t->cs)
            continue;
        fprintf(f, "%04x:%016llx sp=%016llx ax=%016llx fl=%06llx %02x %02x %02x %02x %02x %02x\n", t->cs,
                (unsigned long long)t->rip, (unsigned long long)t->rsp, (unsigned long long)t->rax, (unsigned long long)t->fl, t->bytes[0],
                t->bytes[1], t->bytes[2], t->bytes[3], t->bytes[4], t->bytes[5]);
    }
    fclose(f);
    LOGW("x86: rastro de instrucoes gravado (%s)", why);
}

static void trace_record(x86_cpu *c)
{
    struct x86_trace *t = &c->trace[c->trace_pos];
    c->trace_pos = (c->trace_pos + 1) % c->trace_n;
    t->rip = c->rip;
    t->cs = c->seg[S_CS].sel;
    t->rsp = c->r[R_SP];
    t->fl = c->eflags;
    t->rax = c->r[R_AX];
    uint64_t lin = x86_lin(c, S_CS, c->rip);
    uint8_t *p = (lin & ~0xfffULL) == c->fetch_page ? c->fetch_host + (lin & 0xfff) : NULL;
    for (int i = 0; i < 6; i++)
        t->bytes[i] = (p && (lin & 0xfff) + (uint64_t)i < 0x1000) ? p[i] : 0;
}

static void sample_dump(x86_cpu *c)
{
    char buf[512];
    int o = 0;
    for (int v = 0; v < 256 && o < (int)sizeof(buf) - 16; v++)
        if (c->irq_hist[v]) {
            o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %d:%u", v, c->irq_hist[v]);
            c->irq_hist[v] = 0;
        }
    buf[o] = 0;
    char sym[192] = "", proc[24] = "";
    if (c->code64 && (c->efer & EFER_LMA)) {
        x86_debug_symbolize(c, c->rip, sym, sizeof(sym));
        x86_guest_process(c, proc, sizeof(proc));
    }
    LOGI("x86: amostra %04x:%llx cpl=%d if=%d hlt=%d [%s] %s irqs:%s", c->seg[S_CS].sel, (unsigned long long)c->rip,
         c->cpl, !!(c->eflags & EFL_IF), c->halted, proc, sym, buf);
}

/* nome do processo atual do Windows x64 (EPROCESS.ImageFileName, procurado por ser
 * uma string terminada em .exe perto do inicio do EPROCESS) */
static void x86_guest_process(x86_cpu *c, char *out, size_t n)
{
    uint64_t kpcr = c->cpl ? c->kernel_gs_base : c->seg[S_GS].base, thread = 0, process = 0;
    out[0] = 0;
    if (!dbg_read(c, kpcr + 0x188, &thread, 8) || !dbg_read(c, thread + 0xb8, &process, 8))
        return;
    static uint32_t off;
    char b[16];
    if (off && dbg_read(c, process + off, b, 15)) {
        b[15] = 0;
        snprintf(out, n, "%s", b);
        return;
    }
    for (uint32_t o = 0x200; o < 0x800; o += 8) {
        if (!dbg_read(c, process + o, b, 15))
            return;
        b[15] = 0;
        size_t l = strnlen(b, 15);
        if (l > 4 && !strcmp(b + l - 4, ".exe")) {
            off = o;
            snprintf(out, n, "%s", b);
            return;
        }
    }
}

/* MVM_RAMDUMP=arquivo: grava a RAM fisica inteira (uma vez) em arquivo.cr3-<CR3> */
static void x86_ram_dump(x86_cpu *c)
{
    static int done;
    const char *rd = getenv("MVM_RAMDUMP");
    if (!rd || done++)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s.cr3-%llx", rd, (unsigned long long)c->cr3);
    FILE *f = fopen(path, "wb");
    static uint8_t zero[4096];
    for (uint64_t pa = 0; f && pa < c->vm->ram_size; pa += 4096) {
        uint8_t *h = space_ram_ptr(c->mem, pa, 4096, false);
        fwrite(h ? h : zero, 1, 4096, f);
    }
    if (f)
        fclose(f);
    LOGI("x86: RAM gravada em %s", path);
}

/* traducao linear -> ponteiro de host so para depuracao: sem faltas nem bits A/D */
static uint8_t *x86_host_ptr_nofault(x86_cpu *c, uint64_t lin)
{
    uint64_t pa;
    if (!(c->cr0 & CR0_PG)) {
        pa = (uint32_t)lin;
    } else if (c->efer & EFER_LMA) {
        uint64_t t = c->cr3 & 0x000ffffffffff000ULL;
        int sh = 39;
        for (;;) {
            uint8_t *h = space_ram_ptr(c->mem, t + ((lin >> sh) & 511) * 8, 8, false);
            if (!h)
                return NULL;
            uint64_t e = ld_le(h, 8);
            if (!(e & 1))
                return NULL;
            if (sh == 12 || ((sh == 21 || sh == 30) && (e & 0x80))) {
                uint64_t sz = 1ULL << sh;
                pa = (e & 0x000ffffffffff000ULL & ~(sz - 1)) | (lin & (sz - 1));
                break;
            }
            t = e & 0x000ffffffffff000ULL;
            sh -= 9;
        }
    } else if (c->cr4 & CR4_PAE) {
        uint8_t *h = space_ram_ptr(c->mem, (c->cr3 & 0xffffffe0u) + ((lin >> 30) & 3) * 8, 8, false);
        if (!h || !(ld_le(h, 8) & 1))
            return NULL;
        uint64_t t = ld_le(h, 8) & 0x000ffffffffff000ULL;
        h = space_ram_ptr(c->mem, t + ((lin >> 21) & 511) * 8, 8, false);
        if (!h || !(ld_le(h, 8) & 1))
            return NULL;
        uint64_t e = ld_le(h, 8);
        if (e & 0x80) {
            pa = (e & 0x000fffffffe00000ULL) | (lin & 0x1fffff);
        } else {
            h = space_ram_ptr(c->mem, (e & 0x000ffffffffff000ULL) + ((lin >> 12) & 511) * 8, 8, false);
            if (!h || !(ld_le(h, 8) & 1))
                return NULL;
            pa = (ld_le(h, 8) & 0x000ffffffffff000ULL) | (lin & 0xfff);
        }
    } else {
        uint8_t *h = space_ram_ptr(c->mem, (c->cr3 & 0xfffff000u) + (((uint32_t)lin >> 22) & 1023) * 4, 4, false);
        if (!h || !(ld_le(h, 4) & 1))
            return NULL;
        uint32_t e = (uint32_t)ld_le(h, 4);
        if ((e & 0x80) && (c->cr4 & CR4_PSE)) {
            pa = (e & 0xffc00000u) | ((uint32_t)lin & 0x3fffff);
        } else {
            h = space_ram_ptr(c->mem, (e & 0xfffff000u) + (((uint32_t)lin >> 12) & 1023) * 4, 4, false);
            if (!h || !(ld_le(h, 4) & 1))
                return NULL;
            pa = (ld_le(h, 4) & 0xfffff000u) | ((uint32_t)lin & 0xfff);
        }
    }
    return space_ram_ptr(c->mem, pa, 1, false);
}

/* leitura de memoria do convidado sem faltas (depuracao) */
static bool dbg_read(x86_cpu *c, uint64_t lin, void *out, size_t n)
{
    uint8_t *o = out;
    for (size_t i = 0; i < n; i++) {
        uint8_t *h = x86_host_ptr_nofault(c, lin + i);
        if (!h)
            return false;
        o[i] = *h;
    }
    return true;
}

/* MVM_DBG_DUMPDIR=dir: grava a imagem de cada modulo simbolizado (uma vez) em dir/NOME@BASE.bin */
static void dbg_dump_module(x86_cpu *c, uint64_t base, uint32_t size, const char *name)
{
    static const char *dir;
    static int init;
    static uint64_t done[64];
    static int ndone;
    if (!init++)
        dir = getenv("MVM_DBG_DUMPDIR");
    if (!dir || size == 0 || size > (64u << 20))
        return;
    for (int i = 0; i < ndone; i++)
        if (done[i] == base)
            return;
    if (ndone < 64)
        done[ndone++] = base;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s@%llx.bin", dir, name, (unsigned long long)base);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    for (uint32_t off = 0; off < size; off++) {
        uint8_t *h = x86_host_ptr_nofault(c, base + off);
        fputc(h ? *h : 0, f);
    }
    fclose(f);
}

/* Acha o modulo PE (Windows) que contem 'addr': procura "MZ" para tras e devolve o
 * nome do PDB (diretorio de depuracao CodeView) ou o nome de exportacao. */
static bool dbg_pe_module(x86_cpu *c, uint64_t addr, char *name, size_t nlen, uint64_t *base)
{
    for (uint64_t b = addr & ~0xfffULL, k = 0; k < 8192; k++, b -= 0x1000) {
        uint8_t mz[2];
        uint32_t lfanew;
        if (!dbg_read(c, b, mz, 2) || mz[0] != 'M' || mz[1] != 'Z' || !dbg_read(c, b + 0x3c, &lfanew, 4) || lfanew > 0x1000)
            continue;
        uint8_t pe[4];
        if (!dbg_read(c, b + lfanew, pe, 4) || memcmp(pe, "PE\0\0", 4))
            continue;
        uint16_t magic;
        dbg_read(c, b + lfanew + 24, &magic, 2);
        uint32_t img_size = 0;
        dbg_read(c, b + lfanew + 24 + 56, &img_size, 4); /* SizeOfImage */
        uint64_t dd = b + lfanew + 24 + (magic == 0x20b ? 112 : 96); /* DataDirectory */
        uint32_t dbg[2] = {0, 0}, exp[2] = {0, 0};
        dbg_read(c, dd + 6 * 8, dbg, 8);
        dbg_read(c, dd, exp, 8);
        *base = b;
        name[0] = 0;
        for (uint32_t i = 0; dbg[0] && i < dbg[1] / 28 && i < 8; i++) {
            uint32_t ent[7];
            if (!dbg_read(c, b + dbg[0] + 28 * i, ent, 28) || ent[3] != 2) /* IMAGE_DEBUG_TYPE_CODEVIEW */
                continue;
            char cv[160] = {0};
            if (dbg_read(c, b + ent[5], cv, sizeof(cv) - 1) && !memcmp(cv, "RSDS", 4)) {
                const char *pn = cv + 24, *sl = strrchr(pn, '\\');
                snprintf(name, nlen, "%s", sl ? sl + 1 : pn);
                dbg_dump_module(c, b, img_size, name);
                return true;
            }
        }
        if (exp[0]) {
            uint32_t ed[10];
            if (dbg_read(c, b + exp[0], ed, 40)) {
                char en[64] = {0};
                dbg_read(c, b + ed[3], en, sizeof(en) - 1);
                snprintf(name, nlen, "%s", en);
            }
        }
        if (!name[0])
            snprintf(name, nlen, "?");
        return true;
    }
    return false;
}

/* simbolizacao para ferramentas (mapa de blocos do JIT): "modulo+offset" ou "" */
void x86_debug_symbolize(x86_cpu *c, uint64_t addr, char *out, size_t n)
{
    static struct { uint64_t base, end; char name[192]; } mods[64];
    static int nmods;
    static uint64_t miss[256];
    out[0] = 0;
    for (int i = 0; i < nmods; i++)
        if (addr >= mods[i].base && addr < mods[i].end) {
            snprintf(out, n, "%s+%llx", mods[i].name, (unsigned long long)(addr - mods[i].base));
            return;
        }
    uint64_t region = addr >> 16;
    if (miss[region & 255] == region + 1)
        return;
    char name[192];
    uint64_t base;
    if (!dbg_pe_module(c, addr, name, sizeof(name), &base)) {
        miss[region & 255] = region + 1;
        return;
    }
    uint32_t lfanew = 0, size = 0;
    dbg_read(c, base + 0x3c, &lfanew, 4);
    dbg_read(c, base + lfanew + 24 + 56, &size, 4);
    if (nmods < 64 && size) {
        mods[nmods].base = base;
        mods[nmods].end = base + size;
        snprintf(mods[nmods].name, sizeof(mods[nmods].name), "%s", name);
        nmods++;
    }
    snprintf(out, n, "%s+%llx", name, (unsigned long long)(addr - base));
}

/* simboliza RIP e enderecos de codigo na pilha (convidados Windows x64) */
static void dbg_win_stack(x86_cpu *c)
{
    char name[192];
    uint64_t base;
    if (dbg_pe_module(c, c->cur_rip, name, sizeof(name), &base))
        LOGI("x86:   rip %llx = %s+%llx", (unsigned long long)c->cur_rip, name, (unsigned long long)(c->cur_rip - base));
    bool w64 = c->code64;
    unsigned ws = w64 ? 8 : 4;
    for (int i = 0; i < (w64 ? 64 : 160); i++) {
        uint64_t v = 0;
        if (!dbg_read(c, c->r[R_SP] + ws * (uint64_t)i, &v, ws))
            break;
        if (w64 ? ((v >> 40) != 0xfffff8 || (v & 0xfff) == 0) : (v < 0x80000000u || (v & 0xfff) == 0))
            continue;
        if (dbg_pe_module(c, v, name, sizeof(name), &base))
            LOGI("x86:   [rsp+%03x] %llx = %s+%llx", ws * i, (unsigned long long)v, name, (unsigned long long)(v - base));
    }
}

void x86_debug_dump(void *opaque)
{
    x86_cpu *c = opaque;
    LOGI("x86: estado %04x:%llx cpl=%d if=%d hlt=%d intr=%d inib=%d", c->seg[S_CS].sel, (unsigned long long)c->rip, c->cpl,
         !!(c->eflags & EFL_IF), c->halted, c->intr_line, c->irq_inhibit);
    sample_dump(c);
    static const char *rn[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                 "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    char buf[1024];
    int o = 0;
    for (int i = 0; i < 16; i++)
        o += snprintf(buf + o, sizeof(buf) - (size_t)o, "%s=%llx ", rn[i], (unsigned long long)c->r[i]);
    LOGI("x86: %s", buf);
    LOGI("x86: cr0=%llx cr2=%llx cr3=%llx cr4=%llx efer=%llx fl=%llx", (unsigned long long)c->cr0,
         (unsigned long long)c->cr2, (unsigned long long)c->cr3, (unsigned long long)c->cr4,
         (unsigned long long)c->efer, (unsigned long long)x86_get_flags(c));
    /* bytes do codigo (a partir de RIP-16) e topo da pilha, sem gerar faltas */
    uint64_t pc = x86_lin(c, S_CS, c->rip) - 16;
    o = 0;
    for (int i = 0; i < 48; i++) {
        uint8_t *h = x86_host_ptr_nofault(c, pc + (uint64_t)i);
        o += snprintf(buf + o, sizeof(buf) - (size_t)o, i == 16 ? "[%s" : "%s", h ? "" : "??");
        if (h)
            o += snprintf(buf + o, sizeof(buf) - (size_t)o, "%02x", *h);
        if (i == 16)
            o += snprintf(buf + o, sizeof(buf) - (size_t)o, "]");
        buf[o++] = ' ';
        buf[o] = 0;
    }
    LOGI("x86: codigo %llx: %s", (unsigned long long)pc, buf);
    uint64_t sp = c->r[R_SP];
    o = 0;
    for (int i = 0; i < 24; i++) {
        uint64_t v = 0;
        bool ok = true;
        for (int b = 0; b < 8; b++) {
            uint8_t *h = x86_host_ptr_nofault(c, sp + 8 * (uint64_t)i + (uint64_t)b);
            if (!h) { ok = false; break; }
            v |= (uint64_t)*h << (8 * b);
        }
        o += snprintf(buf + o, sizeof(buf) - (size_t)o, ok ? "%llx " : "?? ", (unsigned long long)v);
    }
    LOGI("x86: pilha %llx: %s", (unsigned long long)sp, buf);
    if ((c->code64 && (c->rip >> 40) == 0xfffff8) || (!c->code64 && (c->cr0 & CR0_PG) && c->rip >= 0x80000000u))
        dbg_win_stack(c);
}

/* MVM_X86_V86LOG=N: registra as N primeiras instrucoes executadas em modo V86 */
static void v86_log(x86_cpu *c)
{
    static long left = -1;
    static FILE *f;
    if (left < 0) {
        const char *e = getenv("MVM_X86_V86LOG");
        left = e ? atol(e) : 0;
        if (left)
            f = fopen(getenv("MVM_X86_V86LOG_FILE") ? getenv("MVM_X86_V86LOG_FILE") : "mvm-v86.txt", "w");
    }
    static long after = -1;
    if (after < 0)
        after = getenv("MVM_X86_TRACE_V86IRET") ? atol(getenv("MVM_X86_TRACE_V86IRET")) : 0;
    if (after && !c->trace_countdown && ((c->eflags >> 12) & 3) < 3) { /* IRET em V86 com IOPL < 3 */
        uint8_t *h = x86_host_ptr_nofault(c, c->seg[S_CS].base + (c->rip & 0xffff));
        if (h && *h == 0xcf && c->seg[S_SS].sel == 0x1000)
            c->trace_countdown = after;
    }
    if (!left || !f)
        return;
    left--;
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        uint8_t *h = x86_host_ptr_nofault(c, c->seg[S_CS].base + ((c->rip + (uint64_t)i) & 0xffff));
        b[i] = h ? *h : 0;
    }
    fprintf(f, "%04x:%04llx ax=%08llx bx=%08llx cx=%08llx dx=%08llx si=%08llx di=%08llx bp=%08llx sp=%04x:%08llx ds=%04x es=%04x fl=%06llx  %02x %02x %02x %02x %02x %02x %02x %02x\n",
            c->seg[S_CS].sel, (unsigned long long)c->rip, (unsigned long long)c->r[0], (unsigned long long)c->r[3],
            (unsigned long long)c->r[1], (unsigned long long)c->r[2], (unsigned long long)c->r[6], (unsigned long long)c->r[7],
            (unsigned long long)c->r[5], c->seg[S_SS].sel, (unsigned long long)c->r[4], c->seg[S_DS].sel, c->seg[S_ES].sel,
            (unsigned long long)x86_get_flags(c), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    if (!left)
        fclose(f);
}

static int64_t x86_run(void *opaque, int64_t budget)
{
    x86_cpu *c = opaque;
    volatile int64_t n = 0;
    if (unlikely(c->sample_mask)) { /* MVM_X86_SAMPLE: uma amostra a cada ~2^n instrucoes */
        uint64_t before = c->sample_icount;
        c->sample_icount += (uint64_t)budget;
        if ((before & ~c->sample_mask) != (c->sample_icount & ~c->sample_mask))
            sample_dump(c);
    }
    if (c->halted) {
        if (!((c->intr_line && (c->eflags & EFL_IF)) || c->nmi_pending))
            return 0;
        c->halted = false;
    }
    setjmp(c->jb);
    while (n < budget) {
        if (unlikely(c->intr_line || c->nmi_pending)) {
            if (c->irq_inhibit) {
                c->irq_inhibit = 0;
            } else if (c->nmi_pending) {
                c->nmi_pending = false;
                c->exc_depth++;
                deliver(c, EXC_NMI, false, false, 0, c->rip);
                c->exc_depth = 0;
                c->halted = false;
            } else if (c->eflags & EFL_IF) {
                int vec = c->intr_ack ? c->intr_ack(c->intr_opaque) : -1;
                if (vec >= 0) {
                    c->irq_hist[vec & 255]++;
                    c->exc_depth++;
                    deliver(c, vec, false, false, 0, c->rip);
                    c->exc_depth = 0;
                    c->halted = false;
                }
            }
        } else {
            c->irq_inhibit = 0;
        }
        if (unlikely(c->halted || atomic_load_explicit(&c->vm->cpu_exit, memory_order_relaxed)))
            break;
        if (likely(c->jit != NULL) && !c->trace) {
            int64_t done = x86_jit_run(c, budget - n);
            if (done > 0) {
                n += done;
                continue;
            }
        }
        if (unlikely(c->jit != NULL))
            x86_jit_note_interp(c);
        c->cur_rip = c->rip;
        n++;
        if (unlikely(c->trace != NULL)) {
            trace_record(c);
            if (c->trace_stop && c->rip == c->trace_stop && !c->trace_dumped && !trace_stop_on_pf()) {
                c->trace_dumped = true;
                trace_dump(c, "MVM_X86_TRACE_STOP");
                uint32_t a[6];
                for (int i = 0; i < 6; i++)
                    a[i] = (uint32_t)x86_read_slow(c, (uint32_t)c->r[R_BP] + 8 + 4 * (uint64_t)i, 4, 0);
                LOGI("x86: parada em %llx: eax=%llx ebx=%llx ecx=%llx edx=%llx esp=%llx ebp=%llx [ebp+8..]=%08x %08x %08x %08x %08x %08x",
                     (unsigned long long)c->rip, (unsigned long long)c->r[0], (unsigned long long)c->r[3],
                     (unsigned long long)c->r[1], (unsigned long long)c->r[2], (unsigned long long)c->r[4],
                     (unsigned long long)c->r[5], a[0], a[1], a[2], a[3], a[4], a[5]);
                sample_dump(c);
                x86_ram_dump(c);
                const char *md = getenv("MVM_X86_MEMDUMP"); /* endereco:tamanho:arquivo */
                if (md) {
                    char path[512] = {0};
                    unsigned long long addr = 0, len = 0;
                    if (sscanf(md, "%llx:%llx:%511s", &addr, &len, path) == 3) {
                        FILE *f = fopen(path, "wb");
                        for (unsigned long long i = 0; f && i < len; i++) {
                            uint8_t *h = x86_host_ptr(c, addr + i, false);
                            fputc(h ? *h : 0, f);
                        }
                        if (f)
                            fclose(f);
                    }
                }
            }
        }
        if (unlikely(c->brk) && c->rip == c->brk) {
            c->brk = 0;
            LOGI("x86: MVM_X86_BREAK em %llx: eax=%llx ebx=%llx ecx=%llx edx=%llx esi=%llx edi=%llx esp=%llx ebp=%llx cr3=%llx",
                 (unsigned long long)c->rip, (unsigned long long)c->r[0], (unsigned long long)c->r[3],
                 (unsigned long long)c->r[1], (unsigned long long)c->r[2], (unsigned long long)c->r[6],
                 (unsigned long long)c->r[7], (unsigned long long)c->r[4], (unsigned long long)c->r[5],
                 (unsigned long long)c->cr3);
            x86_ram_dump(c);
        }
        if (unlikely(c->eflags & EFL_VM))
            v86_log(c);
        if (unlikely(c->trace_countdown > 0) && --c->trace_countdown == 0 && c->trace && !c->trace_dumped) {
            c->trace_dumped = true;
            trace_dump(c, "MVM_X86_TRACE_V86IRET");
        }
        x86_exec_one(c);
    }
    return n;
}

static bool x86_halted(void *opaque)
{
    x86_cpu *c = opaque;
    return c->halted && !((c->intr_line && (c->eflags & EFL_IF)) || c->nmi_pending);
}

static void x86_reset(void *opaque)
{
    x86_cpu *c = opaque;
    memset(c->r, 0, sizeof(c->r));
    memset(c->seg, 0, sizeof(c->seg));
    c->r[R_DX] = 0x6f1;
    c->eflags = 2;
    c->cc_op = CC_NONE;
    c->cr0 = 0x60000010;
    c->cr2 = c->cr3 = c->cr4 = c->cr8 = 0;
    c->efer = 0;
    c->xcr0 = 1;
    memset(c->dr, 0, sizeof(c->dr));
    c->dr[6] = 0xffff0ff0;
    c->dr[7] = 0x400;
    c->star = c->lstar = c->cstar = c->sfmask = c->kernel_gs_base = c->tsc_aux = 0;
    c->sysenter_cs = c->sysenter_esp = c->sysenter_eip = 0;
    c->pat = 0x0007040600070406ULL;
    c->misc_enable = 1;
    memset(c->mtrr_var, 0, sizeof(c->mtrr_var));
    memset(c->mtrr_fix, 0, sizeof(c->mtrr_fix));
    c->mtrr_def = 0;
    c->mcg_status = 0;
    memset(c->mc_bank, 0, sizeof(c->mc_bank));
    c->tsc_offset = 0;
    c->tsc_adjust = 0;
    for (int s = 0; s < 6; s++) {
        c->seg[s].limit = 0xffff;
        c->seg[s].attr = SEG_P | SEG_S | 3;
    }
    c->seg[S_CS].sel = 0xf000;
    c->seg[S_CS].base = 0xffff0000;
    c->seg[S_CS].attr = SEG_P | SEG_S | 0xb;
    c->seg[S_LDTR].limit = 0xffff;
    c->seg[S_LDTR].attr = SEG_P | 2;
    c->seg[S_TR].limit = 0xffff;
    c->seg[S_TR].attr = SEG_P | 0xb;
    c->rip = 0xfff0;
    c->gdt_base = c->idt_base = 0;
    c->gdt_limit = c->idt_limit = 0xffff;
    c->cpl = 0;
    c->halted = false;
    c->irq_inhibit = 0;
    c->nmi_pending = false;
    c->exc_depth = 0;
    c->a20_mask = ~0ULL;
    c->mxcsr = 0x1f80;
    memset(c->xmm, 0, sizeof(c->xmm));
    memset(c->mmx, 0, sizeof(c->mmx));
    x87_reset(c);
    update_mode(c);
    x86_tlb_flush(c);
    x86_jit_flush(c->jit); /* o carregador pode ter reescrito a RAM diretamente */
}

static void x86_dump(void *opaque, FILE *f)
{
    x86_cpu *c = opaque;
    static const char *names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    fprintf(f, "RIP=%016llx CS=%04x CPL=%d %s EFLAGS=%08llx\n", (unsigned long long)c->cur_rip,
            c->seg[S_CS].sel, c->cpl, c->code64 ? "64-bit" : c->csz == 4 ? "32-bit" : "16-bit",
            (unsigned long long)x86_get_flags(c));
    for (int i = 0; i < 16; i++)
        fprintf(f, "%s=%016llx%s", names[i], (unsigned long long)c->r[i], (i % 4 == 3) ? "\n" : " ");
    fprintf(f, "CR0=%llx CR2=%llx CR3=%llx CR4=%llx EFER=%llx\n", (unsigned long long)c->cr0,
            (unsigned long long)c->cr2, (unsigned long long)c->cr3, (unsigned long long)c->cr4,
            (unsigned long long)c->efer);
    static const char *sn[8] = {"ES", "CS", "SS", "DS", "FS", "GS", "LDTR", "TR"};
    for (int i = 0; i < 8; i++)
        fprintf(f, "%s=%04x base=%llx limit=%x attr=%x\n", sn[i], c->seg[i].sel,
                (unsigned long long)c->seg[i].base, c->seg[i].limit, c->seg[i].attr);
}

static void x86_destroy(void *opaque)
{
    x86_cpu *c = opaque;
    if (c->trace && !c->trace_dumped)
        trace_dump(c, "fim da execucao");
    x86_jit_stats(c->jit);
    x86_jit_free(c->jit);
    free(c->trace);
    free(c);
}

static void cpu_tlb_flush_cb(void *cpu) { x86_tlb_flush((x86_cpu *)cpu); }

const mvm_cpu_ops x86_cpu_ops = {
    .reset = x86_reset,
    .run = x86_run,
    .halted = x86_halted,
    .tlb_protect = x86_tlb_protect,
    .dump = x86_dump,
    .destroy = x86_destroy,
    .tlb_flush = cpu_tlb_flush_cb,
};

void *x86_cpu_new(mvm_vm *vm, bool lm)
{
    x86_cpu *c = calloc(1, sizeof(*c));
    c->vm = vm;
    c->mem = &vm->mem;
    c->io = &vm->io;
    c->cpuid_lm = lm;
    if (getenv("MVM_X86_BREAK"))
        c->brk = strtoull(getenv("MVM_X86_BREAK"), NULL, 16);
    const char *tr = getenv("MVM_X86_TRACE");
    if (tr && atoi(tr) > 0) {
        c->trace_n = (unsigned)atoi(tr);
        c->trace = calloc(c->trace_n, sizeof(*c->trace));
    }
    const char *ts = getenv("MVM_X86_TRACE_STOP");
    if (ts)
        c->trace_stop = strtoull(ts, NULL, 0);
    const char *sm = getenv("MVM_X86_SAMPLE");
    if (sm && atoi(sm) > 0 && atoi(sm) < 40)
        c->sample_mask = (1ULL << atoi(sm)) - 1;
    x86_reset(c);
    c->jit = x86_jit_new(c);
    return c;
}

void x86_set_intr(void *opaque, int level)
{
    x86_cpu *c = opaque;
    c->intr_line = level;
    /* interrupcao gerada pela propria CPU no meio de um bloco do JIT (TPR baixado, EOI,
     * auto-IPI, porta de E/S): o proximo bloco sai para que ela seja entregue ja, como no
     * interpretador. O XP entrega APCs por auto-IPI e conta com isso. */
    if (level && c->jit_budget > 0) {
        c->jit_kick += c->jit_budget;
        c->jit_budget = 0;
    }
}

void x86_set_apic(void *cpu, void *apic, uint64_t (*get_base)(void *), void (*set_base)(void *, uint64_t),
                  uint8_t (*get_tpr)(void *), void (*set_tpr)(void *, uint8_t))
{
    x86_cpu *c = cpu;
    c->apic = apic;
    c->apic_get_base = get_base;
    c->apic_set_base = set_base;
    c->apic_get_tpr = get_tpr;
    c->apic_set_tpr = set_tpr;
}

void x86_set_intr_ack(void *opaque, int (*ack)(void *), void *arg)
{
    x86_cpu *c = opaque;
    c->intr_ack = ack;
    c->intr_opaque = arg;
}

void x86_set_a20(void *opaque, bool on)
{
    x86_cpu *c = opaque;
    uint64_t m = on ? ~0ULL : ~(1ULL << 20);
    if (m != c->a20_mask) {
        c->a20_mask = m;
        x86_tlb_flush(c);
    }
}

void x86_setup_realmode(void *opaque, uint16_t cs, uint16_t ip)
{
    x86_cpu *c = opaque;
    x86_reset(c);
    c->seg[S_CS].sel = cs;
    c->seg[S_CS].base = (uint64_t)cs << 4;
    c->rip = ip;
    update_mode(c);
}

void x86_setup_flat32(void *opaque, uint32_t eip, uint32_t gdt_base, uint32_t esi)
{
    x86_cpu *c = opaque;
    c->cr0 |= CR0_PE;
    c->gdt_base = gdt_base;
    c->gdt_limit = 0x2f;
    set_flat(&c->seg[S_CS], 0x10, true, false, 0);
    for (int s = 0; s < 6; s++)
        if (s != S_CS)
            set_flat(&c->seg[s], 0x18, false, false, 0);
    c->cpl = 0;
    c->eflags = 2;
    c->r[R_SI] = esi;
    c->rip = eip;
    update_mode(c);
    x86_tlb_flush(c);
}

void x86_setup_long64(void *opaque, uint64_t rip, uint64_t cr3, uint32_t gdt_base, uint64_t rsi)
{
    x86_cpu *c = opaque;
    c->cr0 |= CR0_PE;
    c->gdt_base = gdt_base;
    c->gdt_limit = 0x2f;
    c->cr4 |= CR4_PAE | CR4_PSE;
    c->cr3 = cr3;
    c->efer = EFER_LME | EFER_LMA | EFER_SCE | EFER_NXE;
    c->cr0 |= CR0_PG | CR0_WP;
    set_flat(&c->seg[S_CS], 0x28, true, true, 0);
    for (int s = 0; s < 6; s++)
        if (s != S_CS)
            set_flat(&c->seg[s], 0x18, false, false, 0);
    c->cpl = 0;
    c->eflags = 2;
    c->r[R_SI] = rsi;
    c->rip = rip;
    update_mode(c);
    x86_tlb_flush(c);
}
