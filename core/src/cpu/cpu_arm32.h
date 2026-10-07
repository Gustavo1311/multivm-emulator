/* Estado interno da CPU ARMv7-A (AArch32). */
#ifndef MVM_CPU_ARM32_H
#define MVM_CPU_ARM32_H

#include "arm_common.h"

#include <setjmp.h>

#define A32_TLB_BITS 9
#define A32_TLB_SIZE (1u << A32_TLB_BITS)
#define A32_TLB_INVALID 1u
#define A32_TLB_IO 2u

typedef struct {
    uint32_t tag_r, tag_w, tag_x;
    uintptr_t addend;
    uint32_t pa;
} a32_tlbe;

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1b, M_SYS = 0x1f };
enum { B_USR = 0, B_FIQ, B_IRQ, B_SVC, B_ABT, B_UND, B_COUNT };

typedef struct a32_cpu {
    uint32_t r[16];
    uint32_t cur;       /* endereco da instrucao atual */
    uint32_t npc;       /* proximo PC */
    uint32_t insn;      /* instrucao atual (depuracao) */
    bool thumb;
    uint32_t nzcv;      /* N=8 Z=4 C=2 V=1 */
    bool q;
    uint32_t ge;        /* bits GE[3:0] */
    uint32_t it;        /* ITSTATE (8 bits) */
    uint32_t mode;
    bool irq_dis, fiq_dis, abt_dis;
    bool big_endian;

    uint32_t bank_r13[B_COUNT], bank_r14[B_COUNT], bank_spsr[B_COUNT];
    uint32_t usr_r8_12[5], fiq_r8_12[5];

    /* VFP */
    union {
        uint64_t d[32];
        uint32_t s[64];
        double fd[32];
        float fs[64];
    } vfp;
    uint32_t fpscr, fpexc, fpinst, fpinst2;

    /* CP15 */
    uint32_t sctlr, actlr, cpacr, ttbr0, ttbr1, ttbcr, dacr;
    uint32_t dfsr, ifsr, dfar, ifar, adfsr, aifsr, par;
    uint32_t prrr, nmrr, vbar, fcseidr, contextidr, tpidrurw, tpidruro, tpidrprw, csselr;
    uint32_t pmuserenr;

    arm_gtimer gt;
    arm_hooks hooks;

    uint32_t excl_addr;
    uint64_t excl_val;
    int excl_size;
    bool excl_valid;

    int irq_line, fiq_line;
    bool halted;
    jmp_buf jb;

    a32_tlbe tlb[2][A32_TLB_SIZE];
    uint32_t fetch_page;
    uint8_t *fetch_host;

    mvm_vm *vm;
    mvm_space *mem;
} a32_cpu;

static inline bool a32_priv(const a32_cpu *c) { return c->mode != M_USR; }

uint32_t a32_read_slow(a32_cpu *c, uint32_t va, unsigned size, int priv);
void a32_write_slow(a32_cpu *c, uint32_t va, uint32_t val, unsigned size, int priv);
_Noreturn void a32_undef(a32_cpu *c);

static inline uint32_t a32_rd(a32_cpu *c, uint32_t va, unsigned size)
{
    int p = a32_priv(c);
    a32_tlbe *e = &c->tlb[p][(va >> 12) & (A32_TLB_SIZE - 1)];
    if (likely(e->tag_r == (va & ~0xfffu)) && likely((va & 0xfff) + size <= 0x1000))
        return (uint32_t)ld_le((const uint8_t *)(e->addend + va), size);
    return a32_read_slow(c, va, size, p);
}

static inline void a32_wr(a32_cpu *c, uint32_t va, uint32_t val, unsigned size)
{
    int p = a32_priv(c);
    a32_tlbe *e = &c->tlb[p][(va >> 12) & (A32_TLB_SIZE - 1)];
    if (likely(e->tag_w == (va & ~0xfffu)) && likely((va & 0xfff) + size <= 0x1000)) {
        st_le((uint8_t *)(e->addend + va), val, size);
        return;
    }
    a32_write_slow(c, va, val, size, p);
}

/* compartilhado entre ARM e Thumb */
uint32_t a32_get_cpsr(a32_cpu *c);
void a32_set_cpsr(a32_cpu *c, uint32_t v, uint32_t mask);
void a32_write_pc_bx(a32_cpu *c, uint32_t v);
void a32_write_pc_alu(a32_cpu *c, uint32_t v);
uint32_t a32_dp(a32_cpu *c, unsigned opc, bool s, uint32_t a, uint32_t b, uint32_t carry, bool *write);
uint32_t a32_shift_c(uint32_t v, unsigned type, unsigned amt, uint32_t cin, uint32_t *cout);
uint32_t a32_shift_imm_c(uint32_t v, unsigned type, unsigned imm5, uint32_t cin, uint32_t *cout);
void a32_svc(a32_cpu *c);
void a32_bkpt(a32_cpu *c);
void a32_exc_return(a32_cpu *c, uint32_t pc);
void a32_cps(a32_cpu *c, unsigned imod, bool m, unsigned aif, unsigned mode);
bool a32_msr(a32_cpu *c, bool spsr, uint32_t mask, uint32_t val);
uint32_t a32_mrs(a32_cpu *c, bool spsr);
void a32_coproc(a32_cpu *c, uint32_t insn); /* MCR/MRC/MCRR/MRRC/LDC/STC/CDP (bits 27:0 como em ARM) */
void a32_ldm_stm(a32_cpu *c, bool load, bool inc, bool before, unsigned rn, bool wb, uint32_t list, bool user);
uint32_t a32_fetch16(a32_cpu *c, uint32_t pc);
uint32_t a32_satop(a32_cpu *c, unsigned op, int32_t a, int32_t b);
uint32_t a32_extend(unsigned op, uint32_t v, unsigned rot, uint32_t add);
uint32_t a32_parallel(a32_cpu *c, unsigned pre, unsigned op, uint32_t a, uint32_t b);
uint32_t a32_sel(a32_cpu *c, uint32_t a, uint32_t b);
uint32_t a32_usad8(uint32_t a, uint32_t b);
uint32_t a32_rbit(uint32_t v);
uint32_t a32_sdiv(int32_t a, int32_t b);
uint32_t a32_smlad(a32_cpu *c, bool sub, bool swap, uint32_t n, uint32_t m, uint32_t acc, bool has_acc);
uint64_t a32_smlald(bool sub, bool swap, uint32_t n, uint32_t m, uint64_t acc);
uint32_t a32_smmul(bool sub, bool round, uint32_t n, uint32_t m, uint32_t acc, bool has_acc);
uint32_t a32_sat_s(a32_cpu *c, int64_t v, unsigned bits);
uint32_t a32_sat_u(a32_cpu *c, int64_t v, unsigned bits);
void a32_hint(a32_cpu *c, unsigned hint);
void a32_barrier(a32_cpu *c, unsigned op);
void a32_psci(a32_cpu *c);

/* Thumb (a32_thumb.c) */
void a32_exec_thumb(a32_cpu *c);
/* VFP (a32_vfp.c): insn com bits 27:0 no formato ARM */
void a32_vfp(a32_cpu *c, uint32_t insn);
bool a32_vfp_enabled(a32_cpu *c);

static inline uint32_t a32_reg(a32_cpu *c, unsigned n) { return c->r[n]; }

#undef BIT
#undef BITS
#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

static inline uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

#endif
