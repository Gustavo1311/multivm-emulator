/* Declaracoes internas compartilhadas entre os arquivos da CPU x86. */
#ifndef MVM_X86_PRIV_H
#define MVM_X86_PRIV_H

#include "cpu_x86.h"

static inline uint64_t szmask(int sz) { return sz >= 8 ? ~0ULL : ((1ULL << (sz * 8)) - 1); }

static inline uint64_t sext_sz(uint64_t v, int sz)
{
    switch (sz) {
    case 1: return (uint64_t)(int64_t)(int8_t)v;
    case 2: return (uint64_t)(int64_t)(int16_t)v;
    case 4: return (uint64_t)(int64_t)(int32_t)v;
    default: return v;
    }
}

uint64_t x86_arith_flags(x86_cpu *c);
bool x86_cond(x86_cpu *c, int cc);
void x86_tlb_flush(x86_cpu *c);
void x86_tlb_flush_page(x86_cpu *c, uint64_t lin);
void x86_tlb_flush_nonglobal(x86_cpu *c);
void x86_debug_symbolize(x86_cpu *c, uint64_t addr, char *out, size_t n);
uint64_t x86_sys_rd(x86_cpu *c, uint64_t lin, unsigned size);
void x86_sys_wr(x86_cpu *c, uint64_t lin, uint64_t v, unsigned size);
uint64_t x86_read_desc(x86_cpu *c, uint16_t sel, uint64_t *hi);
void x86_desc_to_seg(x86_seg *s, uint16_t sel, uint64_t d);
void x86_load_seg(x86_cpu *c, int s, uint16_t sel);
void x86_load_cs(x86_cpu *c, uint16_t sel, int cpl);
void x86_push(x86_cpu *c, uint64_t v, int size);
uint64_t x86_pop(x86_cpu *c, int size);
void x86_sw_interrupt(x86_cpu *c, int vec, uint64_t ret_rip);
void x86_iret(x86_cpu *c, int osz);
void x86_far_ret(x86_cpu *c, int osz, uint16_t imm);
void x86_far_jump(x86_cpu *c, uint16_t sel, uint64_t off, int osz, bool call);
void x86_write_cr(x86_cpu *c, int n, uint64_t v);
uint64_t x86_read_cr(x86_cpu *c, int n);
uint64_t x86_tsc(x86_cpu *c);
void x86_cpuid(x86_cpu *c);
uint64_t x86_host_random(void);
void x86_rdmsr(x86_cpu *c);
void x86_wrmsr(x86_cpu *c);
void x86_syscall(x86_cpu *c);
void x86_sysret(x86_cpu *c, bool rex_w);
void x86_sysenter(x86_cpu *c);
void x86_sysexit(x86_cpu *c, bool rex_w);
void x86_load_ldtr(x86_cpu *c, uint16_t sel);
void x86_load_tr(x86_cpu *c, uint16_t sel);
uint64_t x86_jit_shift(x86_cpu *c, uint64_t opsz, uint64_t v, uint64_t cnt); /* opsz = op | tamanho << 8 */
uint64_t x86_jit_imul2(x86_cpu *c, uint64_t a, uint64_t b, uint64_t sz);
void x86_jit_muldiv(x86_cpu *c, uint64_t op, uint64_t v, uint64_t sz);

static inline void set_lazy(x86_cpu *c, int op, int sz, uint64_t res, uint64_t a, uint64_t b)
{
    c->cc_op = op;
    c->cc_size = sz;
    c->cc_dst = res;
    c->cc_src1 = a;
    c->cc_src2 = b;
}

/* CF atual (materializado) */
static inline uint64_t x86_cf(x86_cpu *c) { return x86_arith_flags(c) & EFL_CF; }

#endif
