/* Estado interno da CPU AArch64 (compartilhado entre o nucleo inteiro e SIMD/FP). */
#ifndef MVM_CPU_ARM64_H
#define MVM_CPU_ARM64_H

#include "arm_common.h"

#include <setjmp.h>

#define A64_TLB_BITS 9
#define A64_TLB_SIZE (1u << A64_TLB_BITS)

#define TLB_INVALID 1ULL
#define TLB_IO 2ULL

typedef struct {
    uint64_t tag_r, tag_w, tag_x; /* pagina virtual | flags */
    uintptr_t addend;             /* host = addend + va */
    uint64_t pa;                  /* pagina fisica */
} a64_tlbe;

typedef union {
    uint64_t d[2];
    uint32_t s[4];
    uint16_t h[8];
    uint8_t b[16];
    double fd[2];
    float fs[4];
} a64_vreg;

enum { ACC_READ = 0, ACC_WRITE = 1, ACC_EXEC = 2 };

typedef struct a64_cpu {
    uint64_t x[32]; /* x[31] sempre 0 (XZR) */
    uint64_t sp[2]; /* SP_EL0, SP_EL1 */
    uint64_t pc;    /* durante a execucao: endereco da proxima instrucao */
    uint64_t cur;   /* endereco da instrucao atual */
    uint32_t nzcv;  /* N=8 Z=4 C=2 V=1 */
    uint32_t daif;  /* D=8 A=4 I=2 F=1 */
    int el;
    int spsel;

    a64_vreg v[32];
    uint32_t fpcr, fpsr;

    uint64_t sctlr, tcr, ttbr0, ttbr1, mair, amair, vbar, elr, spsr, esr, far, par;
    uint64_t tpidr0, tpidrro0, tpidr1, contextidr, cpacr, mdscr, afsr0, afsr1, actlr, csselr;
    uint64_t oslsr, mdccint, pmuserenr;

    arm_gtimer gt;
    arm_hooks hooks;

    uint64_t excl_addr;
    uint64_t excl_val[2];
    int excl_size;
    bool excl_valid;

    int irq_line, fiq_line;
    bool halted;
    jmp_buf jb;

    a64_tlbe tlb[2][A64_TLB_SIZE];
    uint64_t fetch_page;
    uint8_t *fetch_host;

    mvm_vm *vm;
    mvm_space *mem;
} a64_cpu;

/* nucleo (cpu_arm64.c) */
uint64_t a64_read_slow(a64_cpu *c, uint64_t va, unsigned size, int el);
void a64_write_slow(a64_cpu *c, uint64_t va, uint64_t val, unsigned size, int el);
_Noreturn void a64_undef(a64_cpu *c);
bool a64_fp_enabled(a64_cpu *c);
_Noreturn void a64_fp_trap(a64_cpu *c);

/* SIMD/FP (a64_simd.c) */
void a64_simd_fp(a64_cpu *c, uint32_t insn);
void a64_simd_ldst(a64_cpu *c, uint32_t insn);

static inline uint64_t a64_rd(a64_cpu *c, uint64_t va, unsigned size)
{
    a64_tlbe *e = &c->tlb[c->el][(va >> 12) & (A64_TLB_SIZE - 1)];
    if (likely(e->tag_r == (va & ~0xfffULL)) && likely((va & 0xfff) + size <= 0x1000))
        return ld_le((const uint8_t *)(e->addend + va), size);
    return a64_read_slow(c, va, size, c->el);
}

static inline void a64_wr(a64_cpu *c, uint64_t va, uint64_t val, unsigned size)
{
    a64_tlbe *e = &c->tlb[c->el][(va >> 12) & (A64_TLB_SIZE - 1)];
    if (likely(e->tag_w == (va & ~0xfffULL)) && likely((va & 0xfff) + size <= 0x1000)) {
        st_le((uint8_t *)(e->addend + va), val, size);
        return;
    }
    a64_write_slow(c, va, val, size, c->el);
}

#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

#endif
