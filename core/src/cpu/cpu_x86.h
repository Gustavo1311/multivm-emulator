/* Estado interno da CPU x86 (i386 e x86-64). */
#ifndef MVM_CPU_X86_H
#define MVM_CPU_X86_H

#include "../internal.h"

#include <setjmp.h>

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, S_LDTR, S_TR };

#define EFL_CF 0x1u
#define EFL_PF 0x4u
#define EFL_AF 0x10u
#define EFL_ZF 0x40u
#define EFL_SF 0x80u
#define EFL_TF 0x100u
#define EFL_IF 0x200u
#define EFL_DF 0x400u
#define EFL_OF 0x800u
#define EFL_IOPL 0x3000u
#define EFL_NT 0x4000u
#define EFL_RF 0x10000u
#define EFL_VM 0x20000u
#define EFL_AC 0x40000u
#define EFL_VIF 0x80000u
#define EFL_VIP 0x100000u
#define EFL_ID 0x200000u
#define EFL_ARITH (EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF)

#define CR0_PE 0x1u
#define CR0_MP 0x2u
#define CR0_EM 0x4u
#define CR0_TS 0x8u
#define CR0_NE 0x20u
#define CR0_WP 0x10000u
#define CR0_AM 0x40000u
#define CR0_PG 0x80000000u
#define CR4_PSE 0x10u
#define CR4_PAE 0x20u
#define CR4_PGE 0x80u
#define CR4_OSFXSR 0x200u
#define EFER_SCE 0x1u
#define EFER_LME 0x100u
#define EFER_LMA 0x400u
#define EFER_NXE 0x800u

/* excecoes */
enum {
    EXC_DE = 0, EXC_DB = 1, EXC_NMI = 2, EXC_BP = 3, EXC_OF = 4, EXC_BR = 5, EXC_UD = 6, EXC_NM = 7,
    EXC_DF = 8, EXC_TS = 10, EXC_NP = 11, EXC_SS = 12, EXC_GP = 13, EXC_PF = 14, EXC_MF = 16,
    EXC_AC = 17, EXC_MC = 18, EXC_XM = 19
};

/* atributos do segmento: bits 40..55 do descritor sem o limite 19:16 */
#define SEG_TYPE(a) ((a) & 0xf)
#define SEG_S 0x10u
#define SEG_DPL(a) (((a) >> 5) & 3)
#define SEG_P 0x80u
#define SEG_L 0x2000u
#define SEG_DB 0x4000u
#define SEG_G 0x8000u

typedef struct {
    uint16_t sel;
    uint64_t base;
    uint32_t limit;
    uint32_t attr;
} x86_seg;

/* flags preguicosas */
/* CC_SZP: SF/ZF/PF do resultado, cc_aux = CF | OF << 1, AF = 0 (SHL/SHR/SAR, MUL, IMUL) */
enum { CC_NONE = 0, CC_ADD, CC_ADC, CC_SUB, CC_SBB, CC_LOGIC, CC_INC, CC_DEC, CC_SZP };

#define X86_TLB_BITS 12
#define X86_TLB_SIZE (1u << X86_TLB_BITS)
#define XTLB_INVALID 1ULL
#define XTLB_IO 2ULL

typedef struct {
    uint64_t tag_r, tag_w, tag_x;
    uintptr_t addend;
    uint64_t pa;
} x86_tlbe;

typedef union {
    uint8_t b[16];
    uint16_t w[8];
    uint32_t d[4];
    uint64_t q[2];
    int8_t sb[16];
    int16_t sw[8];
    int32_t sd[4];
    int64_t sq[2];
    float f[4];
    double fd[2];
} xmm_reg;

typedef struct {
    int op;              /* opcode (0x0Fxx para 2 bytes) */
    int osz, asz;        /* tamanhos em bytes */
    int seg;             /* segmento efetivo (-1 = padrao) */
    bool rep, repne, lock, opsize_prefix;
    uint8_t rex;
    uint8_t modrm, mod, reg, rm;
    bool mem;
    uint64_t ea;         /* deslocamento (sem base de segmento) */
    int ea_seg;
    bool riprel;
} x86_dec;

typedef struct x86_cpu {
    uint64_t r[16];
    uint64_t rip;        /* proximo byte a buscar */
    uint64_t cur_rip;    /* inicio da instrucao atual */
    uint64_t eflags;
    int cc_op, cc_size;
    uint64_t cc_dst, cc_src1, cc_src2, cc_aux;
    /* JIT (x86_jit.c) */
    int64_t jit_budget;  /* instrucoes que os blocos ainda podem executar */
    int64_t jit_kick;    /* orcamento tirado dos blocos para sairem logo (interrupcao nova) */
    void *jit_exit;      /* saida de bloco usada para voltar ao despachante (encadeamento) */
    void *jit_site;      /* cache do ponto de salto que falhou (o despachante o preenche) */
    uint32_t jit_smc;    /* codigo traduzido foi invalidado por escrita durante o bloco */
    uint32_t jit_pad;
    struct x86_jit *jit;

    x86_seg seg[8];
    uint64_t gdt_base, idt_base;
    uint32_t gdt_limit, idt_limit;
    uint64_t cr0, cr2, cr3, cr4, cr8, efer, xcr0;
    uint64_t dr[8];
    int cpl;
    bool code64;         /* modo 64 bits (LMA && CS.L) */
    int csz;             /* tamanho de operando padrao: 2 ou 4 */
    int ssz;             /* tamanho do ponteiro de pilha: 2, 4 ou 8 */

    /* MSRs */
    uint64_t star, lstar, cstar, sfmask, kernel_gs_base, tsc_aux;
    uint64_t sysenter_cs, sysenter_esp, sysenter_eip;
    uint64_t pat, misc_enable, apic_base, tsc_offset, tsc_adjust;
    uint64_t mtrr_var[16], mtrr_fix[11], mtrr_def; /* MTRRs (so armazenados) */
    uint64_t mcg_status, mc_bank[4];               /* MCA: 1 banco (CTL/STATUS/ADDR/MISC) */

    /* x87 (registradores em precisao dupla) */
    double st[8];
    uint16_t fcw, fsw, ftw; /* ftw: 2 bits por registrador fisico */
    int ftop;
    uint16_t fop;
    uint64_t fip, fdp;

    /* SSE/MMX */
    xmm_reg xmm[16];
    uint32_t mxcsr;
    uint64_t mmx[8];     /* MMX compartilha st[] no hardware; aqui separado */

    /* interrupcoes */
    int intr_line;
    bool nmi_pending;
    bool halted;
    int irq_inhibit;     /* sombra apos STI/MOV SS */
    int (*intr_ack)(void *opaque);
    void *intr_opaque;
    void (*reset_hook)(void *opaque);

    bool cpuid_lm;       /* x86-64 disponivel */
    /* APIC local (opcional): MSR IA32_APIC_BASE e CR8 = TPR[7:4] */
    void *apic;
    uint64_t (*apic_get_base)(void *apic);
    void (*apic_set_base)(void *apic, uint64_t v);
    uint8_t (*apic_get_tpr)(void *apic);
    void (*apic_set_tpr)(void *apic, uint8_t tpr);
    uint64_t a20_mask;

    x86_tlbe tlb[2][X86_TLB_SIZE];
    uint8_t tlb_g[2][X86_TLB_SIZE]; /* entrada de pagina global (CR4.PGE): sobrevive a troca de CR3 */
    /* entradas preenchidas desde o ultimo esvaziamento ((user << X86_TLB_BITS) | indice): o
     * NTLDR troca de modo (CR0) a cada leitura de disco pela BIOS e esvaziar o TLB inteiro
     * custava ~20% do boot; assim o esvaziamento so visita o que foi usado */
    uint16_t tlb_used[2 * X86_TLB_SIZE];
    uint8_t tlb_inlist[2][X86_TLB_SIZE];
    unsigned tlb_nused;
    bool tlb_listed; /* falso ate o primeiro esvaziamento completo */
    uint64_t fetch_page;
    uint8_t *fetch_host;
    /* janela de busca da instrucao atual (bytes contiguos ja mapeados) */
    const uint8_t *ip_ptr;
    unsigned ip_left;

    jmp_buf jb;
    int exc_depth;
    /* depuracao: rastro circular de instrucoes (MVM_X86_TRACE=n) */
    struct x86_trace { uint64_t rip, rsp, fl, rax; uint16_t cs; uint8_t bytes[6]; } *trace;
    unsigned trace_n, trace_pos;
    bool trace_dumped, trace_jit;
    uint64_t brk;        /* MVM_X86_BREAK: grava RAM e registradores ao chegar neste RIP (tambem com JIT) */
    uint64_t trace_stop; /* MVM_X86_TRACE_STOP: grava o rastro ao chegar neste RIP */
    int64_t trace_countdown; /* > 0: grava o rastro apos tantas instrucoes */
    /* MVM_X86_SAMPLE=n: registra CS:RIP e IRQs a cada 2^n instrucoes */
    uint64_t sample_mask, sample_icount;
    uint32_t irq_hist[256];
    mvm_vm *vm;
    mvm_space *mem;
    mvm_space *io;
} x86_cpu;

/* nucleo */
_Noreturn void x86_exception(x86_cpu *c, int vec, int has_err, uint32_t err);
static inline _Noreturn void x86_ud(x86_cpu *c) { x86_exception(c, EXC_UD, 0, 0); }
static inline _Noreturn void x86_gp(x86_cpu *c, uint32_t e) { x86_exception(c, EXC_GP, 1, e); }

uint64_t x86_read_slow(x86_cpu *c, uint64_t lin, unsigned size, int user);
void x86_write_slow(x86_cpu *c, uint64_t lin, uint64_t v, unsigned size, int user);
void x86_probe_write(x86_cpu *c, uint64_t lin, unsigned size);

static inline int x86_user(const x86_cpu *c) { return c->cpl == 3; }

static inline uint64_t x86_rd_lin(x86_cpu *c, uint64_t lin, unsigned size)
{
    x86_tlbe *e = &c->tlb[x86_user(c)][(lin >> 12) & (X86_TLB_SIZE - 1)];
    if (likely(e->tag_r == (lin & ~0xfffULL)) && likely((lin & 0xfff) + size <= 0x1000))
        return ld_le((const uint8_t *)(e->addend + lin), size);
    return x86_read_slow(c, lin, size, x86_user(c));
}

static inline void x86_wr_lin(x86_cpu *c, uint64_t lin, uint64_t v, unsigned size)
{
    x86_tlbe *e = &c->tlb[x86_user(c)][(lin >> 12) & (X86_TLB_SIZE - 1)];
    if (likely(e->tag_w == (lin & ~0xfffULL)) && likely((lin & 0xfff) + size <= 0x1000)) {
        st_le((uint8_t *)(e->addend + lin), v, size);
        return;
    }
    x86_write_slow(c, lin, v, size, x86_user(c));
}

static inline uint64_t x86_lin(x86_cpu *c, int seg, uint64_t off)
{
    if (c->code64)
        return (seg == S_FS || seg == S_GS) ? c->seg[seg].base + off : off;
    return (uint32_t)(c->seg[seg].base + off);
}

static inline uint64_t x86_reg_read(x86_cpu *c, int idx, int size, bool rex)
{
    if (size == 1) {
        if (!rex && idx >= 4 && idx < 8)
            return (c->r[idx - 4] >> 8) & 0xff;
        return c->r[idx] & 0xff;
    }
    if (size == 8)
        return c->r[idx];
    return c->r[idx] & ((1ULL << (size * 8)) - 1);
}

static inline void x86_reg_write(x86_cpu *c, int idx, int size, uint64_t v, bool rex)
{
    switch (size) {
    case 1:
        if (!rex && idx >= 4 && idx < 8)
            c->r[idx - 4] = (c->r[idx - 4] & ~0xff00ULL) | ((v & 0xff) << 8);
        else
            c->r[idx] = (c->r[idx] & ~0xffULL) | (v & 0xff);
        break;
    case 2: c->r[idx] = (c->r[idx] & ~0xffffULL) | (v & 0xffff); break;
    case 4: c->r[idx] = (uint32_t)v; break;
    default: c->r[idx] = v; break;
    }
}

static inline uint64_t x86_ea_lin(x86_cpu *c, x86_dec *d)
{
    uint64_t off = d->ea;
    if (d->riprel) {
        off += c->rip;
        if (d->asz == 4)
            off = (uint32_t)off;
    }
    return x86_lin(c, d->ea_seg, off);
}

static inline uint64_t x86_rm_read(x86_cpu *c, x86_dec *d, int size)
{
    if (!d->mem)
        return x86_reg_read(c, d->rm, size, d->rex != 0);
    return x86_rd_lin(c, x86_ea_lin(c, d), (unsigned)size);
}

static inline void x86_rm_write(x86_cpu *c, x86_dec *d, int size, uint64_t v)
{
    if (!d->mem) {
        x86_reg_write(c, d->rm, size, v, d->rex != 0);
        return;
    }
    x86_wr_lin(c, x86_ea_lin(c, d), v, (unsigned)size);
}
uint8_t x86_fetch8_slow(x86_cpu *c);
uint32_t x86_fetch32_slow(x86_cpu *c);
/* abre a janela de busca para a instrucao em RIP (chamado no inicio de cada instrucao) */
static inline void x86_fetch_begin(x86_cpu *c)
{
    uint64_t lin = c->code64 ? c->rip : (uint32_t)(c->seg[S_CS].base + c->rip);
    if (likely((lin & ~0xfffULL) == c->fetch_page)) {
        unsigned off = (unsigned)(lin & 0xfff), left = 0x1000 - off;
        if (left > 15)
            left = 15;
        if (!c->code64 && c->csz == 2) {
            unsigned lim = 0x10000 - (unsigned)(c->rip & 0xffff);
            if (left > lim)
                left = lim;
        }
        c->ip_ptr = c->fetch_host + off;
        c->ip_left = left;
    } else {
        c->ip_left = 0;
    }
}
static inline uint8_t x86_fetch8(x86_cpu *c)
{
    if (likely(c->ip_left)) {
        c->ip_left--;
        c->rip++;
        return *c->ip_ptr++;
    }
    return x86_fetch8_slow(c);
}
static inline uint32_t x86_fetch32(x86_cpu *c)
{
    if (likely(c->ip_left >= 4)) {
        c->ip_left -= 4;
        c->rip += 4;
        uint32_t v = (uint32_t)ld_le(c->ip_ptr, 4);
        c->ip_ptr += 4;
        return v;
    }
    return x86_fetch32_slow(c);
}
uint64_t x86_get_flags(x86_cpu *c);
void x86_set_arith_flags(x86_cpu *c, uint64_t fl);

/* x87 (x86_fpu.c) */
void x87_exec(x86_cpu *c, x86_dec *d, int op);
void x87_exec_at(x86_cpu *c, x86_dec *d, int op, uint64_t lin);
void x87_reset(x86_cpu *c);
void x87_fxsave_regs(x86_cpu *c, uint8_t *buf);  /* area de 128 bytes (st0-7) + cabecalho */
void x87_fxrstor_regs(x86_cpu *c, const uint8_t *buf);
void x87_to_ext(double v, uint8_t out[10]);
double x87_from_ext(const uint8_t in[10]);

/* SSE/MMX (x86_sse.c): retorna false se o opcode nao for tratado */
bool sse_exec(x86_cpu *c, x86_dec *d, int op);
void sse_fxsave(x86_cpu *c, uint64_t lin, bool rex_w);
void sse_fxrstor(x86_cpu *c, uint64_t lin, bool rex_w);

/* construtores */
void *x86_cpu_new(mvm_vm *vm, bool lm);
extern const mvm_cpu_ops x86_cpu_ops;
void x86_set_intr(void *cpu, int level);
void x86_set_intr_ack(void *cpu, int (*ack)(void *), void *opaque);
void x86_set_a20(void *cpu, bool on);
void x86_set_apic(void *cpu, void *apic, uint64_t (*get_base)(void *), void (*set_base)(void *, uint64_t),
                  uint8_t (*get_tpr)(void *), void (*set_tpr)(void *, uint8_t));
/* estados iniciais */
void x86_setup_realmode(void *cpu, uint16_t cs, uint16_t ip);
void x86_setup_flat32(void *cpu, uint32_t eip, uint32_t gdt_base, uint32_t esi);
void x86_setup_long64(void *cpu, uint64_t rip, uint64_t cr3, uint32_t gdt_base, uint64_t rsi);

#endif
