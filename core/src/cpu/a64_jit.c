/*
 * JIT AArch64 -> AArch64 para o convidado ARM64 (so em host aarch64).
 *
 * Convidado e host tem o mesmo conjunto de instrucoes: a maior parte das instrucoes de dados
 * e emitida quase igual, trocando so os campos de registrador (Rd/Rn/Rm/Ra) por temporarios do
 * host que recebem os registradores do convidado (guardados em c->x[] / c->sp[]). Os flags NZCV
 * do convidado ficam no NZCV do host durante a execucao dos blocos (B.cond, CSEL, ADC, CCMP
 * funcionam nativos); vao para c->nzcv nas saidas e antes de chamar C.
 *
 * Acessos a memoria: teste da TLB do interpretador inline (sem mexer nos flags: eor + cbnz) e
 * o proprio load/store do convidado na forma [Xaddend, Xva]; falha de TLB, E/S ou pagina
 * cruzada vai para a64_read_slow/a64_write_slow (que podem gerar falta com longjmp para o
 * a64_run; c->cur e c->nzcv sao gravados antes).
 *
 * Blocos: chave (VA, PA, EL/SPSel/FP), nunca atravessam pagina; terminam num desvio ou antes
 * da primeira instrucao nao traduzida, que o interpretador executa (a64_run). Saidas diretas
 * para a mesma pagina fisica sao encadeadas (BL do stub vira B para o bloco).
 *
 * Codigo automodificavel: paginas com blocos ficam marcadas em mem->code_bits; a TLB manda as
 * escritas nelas pelo caminho lento e space_write/space_ram_ptr chamam code_hook, que invalida
 * os blocos da pagina (de outra thread, ex.: DMA, a invalidacao fica pendente ate a proxima
 * entrada no JIT). Blocos invalidados nao sao liberados (so no esvaziamento total), entao um
 * bloco em execucao pode terminar com seguranca.
 *
 * Registradores do host: x19 = CPU, x20-x24 = temporarios do convidado (preservados em
 * chamadas), x25 = TLB do EL atual, x27 = NZCV salvo em chamadas, x28 = sair da fatia (c->exit_flag),
 * x29 = orcamento de instrucoes; x9-x17 = rascunho.
 *
 * Ambiente: MVM_JIT=0 desliga; MVM_JIT_STATS=1 mostra as instrucoes que mais param blocos.
 */
#include "cpu_arm64.h"

#include <stddef.h>
#include <stdlib.h>

#if defined(__aarch64__)

#include "a64_asm.h"

#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif

/* stubs no meio: BL/B alcancam +-128 MiB, entao cada metade tem menos que isso */
#define CODE_SIZE (128u << 20)
#define MAX_BLOCKS 524288
#define BLOCK_MAX_INSNS 128
#define HASH_BITS 19
#define HASH_SIZE (1u << HASH_BITS)
#define MAX_COLD 256
#define JC_BITS 16
#define JC_SIZE (1u << JC_BITS)

/* cache de saltos: VA + flags -> codigo, validado pela TLB de execucao (PA da pagina) */
typedef struct {
    uint64_t va;
    uint32_t flags, pad;
    uint64_t pa; /* pagina fisica */
    uint32_t *code;
} jc_ent;

/* registradores fixos */
#define RC 19
#define T0 20 /* Rd / Rt */
#define T1 21 /* Rn / Rt2 */
#define T2 22 /* endereco */
#define T3 23 /* Rm */
#define T4 24 /* Ra */
#define RTLB 25
#define RJC 26
#define RFL 27
#define REXIT 28
#define RBUD 29

#define OFF(f) ((uint32_t)offsetof(a64_cpu, f))
#define OFF_X(n) (OFF(x) + 8u * (unsigned)(n))
#define OFF_SP(i) (OFF(sp) + 8u * (unsigned)(i))
#define OFF_V(n) (OFF(v) + 16u * (unsigned)(n))
#define DMB_ISH 0xD5033BBFu
#define VT0 16 /* temporarios de vetor do host */
#define VT1 17

typedef struct jblock jblock;

/* registro de saida, guardado no proprio codigo logo depois do BL (alinhado a 4 bytes):
 * PC de destino e bloco de origem; o BL a trocar por B fica na palavra anterior */
typedef struct {
    uint64_t target;
    jblock *blk;
} exit_rec;
static inline uint64_t rec_target(const void *r) { uint64_t v; memcpy(&v, r, 8); return v; }
static inline jblock *rec_blk(const void *r) { jblock *b; memcpy(&b, (const uint8_t *)r + 8, sizeof(b)); return b; }
static inline uint32_t *rec_patch(void *r) { return (uint32_t *)r - 1; }

struct jblock {
    uint64_t va, pa;
    uint32_t flags;
    uint32_t ninsn;
    uint32_t *code;
    jblock *hnext, *pnext;
    uint32_t *map; /* instrucoes SIMD/FP nativas: (deslocamento no codigo << 16) | indice (SIGILL) */
    uint16_t nmap;
    bool valid, interp;
};

/* trecho frio (caminho lento de memoria), emitido depois do corpo do bloco */
enum { COLD_LD, COLD_ST, COLD_LDP, COLD_STP, COLD_HELPER };
typedef struct {
    int kind;
    uint32_t *fix;  /* cbnz a apontar para o trecho */
    uint32_t *back; /* onde continuar */
    uint64_t cur;
    unsigned size, opc; /* opc do load (sinal) */
    uint32_t insn;      /* COLD_HELPER: a instrucao inteira vai para o interpretador */
} cold_rec;

struct a64_jit {
    a64_cpu *c;
    mvm_space *mem;
    uint32_t *code, *code_end, *code_start, *pos;
    uint32_t *r_end, *r2_start; /* fim da regiao atual; inicio da segunda (depois dos stubs) */
    uint32_t *stubs;
    void (*enter)(a64_cpu *c, uint32_t *code, a64_tlbe *tlb, _Atomic int *cpu_exit, jc_ent *jc);
    uint32_t *exit_stub, *stub_exit_rec, *stub_lookup, *stub_dead;
    uint32_t *st_ld[4][4], *st_ldp[2][2], *st_st[4], *st_stp[2]; /* caminhos lentos */
    jc_ent *jc;
    uint16_t *jc_cnt; /* entradas do cache por pagina fisica */
    jblock *blocks;
    unsigned nblocks;
    jblock **hash; /* HASH_SIZE baldes */
    jblock **pages; /* lista de blocos por pagina fisica */
    uint64_t npages;
    uint8_t *bits;
    uint64_t *pend; /* paginas a invalidar (vindas de outra thread) */
    _Atomic int pending;
    struct a64_jit_group *group;
    uint64_t n_flush;
    bool crc32;
    /* traducao atual */
    cold_rec cold[MAX_COLD];
    int ncold;
    uint32_t flags;
    int el;
    unsigned sp_idx;
    uint32_t *blk_code;
    unsigned cur_idx;
    uint32_t map[BLOCK_MAX_INSNS];
    unsigned nmap;
    /* estatisticas */
    bool stats;
    unsigned maxn;  /* MVM_JIT_MAXN: instrucoes por bloco (depuracao) */
    uint32_t off;   /* MVM_JIT_NO: classes desligadas (bits, depuracao) */
    bool nohelper;  /* MVM_JIT_NOHELPER: instrucoes nao traduzidas param o bloco */
    bool noresume;  /* MVM_JIT_NORESUME: sempre volta ao C depois de instrucoes de controle */
    uint64_t why[8];
    uint64_t n_next;
    uint64_t n_tr, n_enter, n_chain, n_inval, n_insns, n_dyn, n_xpage, tr_ns, n_tr_insns;
    uint32_t stop_op[64];
    uint64_t stop_cnt[64];
};

static __thread struct a64_jit *tls_jit;

/* Varias CPUs: um JIT por CPU. mem->code_bits e a uniao das paginas com codigo traduzido por
 * qualquer CPU (a TLB de todas manda as escritas nelas pelo caminho lento) e o code_hook do
 * grupo invalida os blocos de cada CPU que tem codigo na pagina: na hora, se a escrita veio da
 * thread dela; senao fica pendente e a CPU sai da fatia para invalidar antes de seguir. */
struct a64_jit_group {
    struct a64_jit *jits[MVM_MAX_CPUS];
    _Atomic int n;
    uint8_t *bits;
    uint64_t npages;
};
static pthread_mutex_t group_lock = PTHREAD_MUTEX_INITIALIZER;
int64_t host_clock_ns(void);

/* ------------------------------------------------------------ emissores extras */

static inline void e_mrs_nzcv(a64 *a, int rt) { a64_put(a, 0xD53B4200u | (uint32_t)rt); }
static inline void e_mrs_fpsr(a64 *a, int rt) { a64_put(a, 0xD53B4420u | (uint32_t)rt); }
static inline void e_msr_fpsr(a64 *a, int rt) { a64_put(a, 0xD51B4420u | (uint32_t)rt); }
/* flags acumulados pelas operacoes FP nativas (FPSR do host) -> c->fpsr; zera o do host */
static void e_merge_fpsr(a64 *a)
{
    e_mrs_fpsr(a, 9);
    a64_ldr(a, 4, 10, RC, (uint32_t)offsetof(a64_cpu, fpsr));
    a64_logic(a, 0, 1, 0, 10, 10, 9);
    a64_str(a, 4, 10, RC, (uint32_t)offsetof(a64_cpu, fpsr));
    e_msr_fpsr(a, XZR);
}
static inline void e_msr_nzcv(a64 *a, int rt) { a64_put(a, 0xD51B4200u | (uint32_t)rt); }
static inline void e_tbz(a64 *a, int nz, int rt, unsigned bit, uint32_t *target)
{
    a64_put(a, 0x36000000u | ((bit >> 5) << 31) | ((uint32_t)nz << 24) | ((bit & 31) << 19) |
                   (((uint32_t)(target - a->p) & 0x3fff) << 5) | (uint32_t)rt);
}
/* ADD Xd, Xn, Rm, <ext> #amt (ext: 2 UXTW, 3 UXTX/LSL, 6 SXTW, 7 SXTX) */
static inline void e_add_ext(a64 *a, int rd, int rn, int rm, unsigned opt, unsigned amt)
{
    a64_put(a, 0x8B200000u | ((uint32_t)rm << 16) | (opt << 13) | (amt << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
/* load/store com registrador: [Xn, Xm] com o size/opc/V do convidado */
static inline void e_ldst_regoff(a64 *a, unsigned size, unsigned v, unsigned opc, int rt, int rn, int rm)
{
    a64_put(a, (size << 30) | 0x38200800u | (v << 26) | (opc << 22) | ((uint32_t)rm << 16) | (3u << 13) |
                   ((uint32_t)rn << 5) | (uint32_t)rt);
}
/* LDP/STP [Xn] (deslocamento 0) com opc/V/L do convidado */
static inline void e_ldstp(a64 *a, unsigned opc, unsigned v, unsigned L, int rt, int rt2, int rn)
{
    a64_put(a, (opc << 30) | 0x29000000u | (v << 26) | (L << 22) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) |
                   (uint32_t)rt);
}
/* corrige um desvio emitido por e_tbz/a64_bcond/a64_cbz (TBZ tem imediato de 14 bits) */
static inline void e_patch(uint32_t *at, uint32_t *target)
{
    if ((*at & 0x7e000000u) == 0x36000000u) {
        int32_t d = (int32_t)(target - at);
        *at = (*at & ~(0x3fffu << 5)) | (((uint32_t)d & 0x3fff) << 5);
    } else {
        a64_patch(at, target);
    }
}
static inline void e_call(a64 *a, void *fn)
{
    a64_mov_imm(a, 9, (uint64_t)(uintptr_t)fn);
    a64_blr(a, 9);
}
static inline uint32_t setf(uint32_t insn, int shift, int reg)
{
    return (insn & ~(31u << shift)) | ((uint32_t)reg << shift);
}

/* ------------------------------------------------------------ registradores do convidado */

enum { CTX_NONE = 0, CTX_ZR, CTX_SP };

/* registrador de origem: devolve o registrador do host com o valor */
static int src(struct a64_jit *j, a64 *a, int ctx, unsigned g, int tmp)
{
    if (g == 31) {
        if (ctx == CTX_ZR)
            return XZR;
        a64_ldr(a, 8, tmp, RC, OFF_SP(j->sp_idx));
        return tmp;
    }
    a64_ldr(a, 8, tmp, RC, OFF_X(g));
    return tmp;
}

static void dst(struct a64_jit *j, a64 *a, int ctx, unsigned g, int host)
{
    if (g == 31) {
        if (ctx == CTX_SP)
            a64_str(a, 8, host, RC, OFF_SP(j->sp_idx));
        return;
    }
    a64_str(a, 8, host, RC, OFF_X(g));
}

/* ------------------------------------------------------------ helpers chamados pelo codigo */

/* ------------------------------------------------------------ blocos */

static unsigned hash_of(uint64_t va, uint64_t pa) { return (unsigned)((va >> 2) ^ (pa >> 9) ^ (va >> 21)) & (HASH_SIZE - 1); }

static void jc_clear(struct a64_jit *j)
{
    for (unsigned i = 0; i < JC_SIZE; i++)
        j->jc[i].va = ~0ULL;
    if (j->jc_cnt)
        memset(j->jc_cnt, 0, j->npages * sizeof(uint16_t));
}

static unsigned jc_hash(uint64_t va) { return (unsigned)((va ^ (va >> 18)) >> 2) & (JC_SIZE - 1); }

static void group_page_dropped(struct a64_jit *j, uint64_t pg);

static void jit_flush(struct a64_jit *j)
{
    memset(j->hash, 0, HASH_SIZE * sizeof(jblock *));
    memset(j->pages, 0, j->npages * sizeof(jblock *));
    /* as paginas so desta CPU saem do mapa comum */
    for (uint64_t w = 0; w < (j->npages + 7) / 8; w++) {
        uint8_t m = __atomic_exchange_n(&j->bits[w], 0, __ATOMIC_SEQ_CST);
        while (m) {
            int b = __builtin_ctz(m);
            m &= (uint8_t)(m - 1);
            group_page_dropped(j, w * 8 + (uint64_t)b);
        }
    }
    jc_clear(j);
    j->nblocks = 0;
    j->pos = j->code;
    j->r_end = j->stubs;
    j->c->jit_exit = NULL;
    j->n_flush++;
}

/* nenhuma CPU tem mais codigo na pagina: ela volta a aceitar escritas rapidas (quando a TLB
 * for refeita). Se outra CPU marcou a pagina ao mesmo tempo, o bit do grupo volta. */
static void group_page_dropped(struct a64_jit *j, uint64_t pg)
{
    struct a64_jit_group *g = j->group;
    if (!g)
        return;
    uint8_t bit = (uint8_t)(1u << (pg & 7));
    int n = atomic_load(&g->n);
    for (int k = 0; k < n; k++)
        if (__atomic_load_n(&g->jits[k]->bits[pg >> 3], __ATOMIC_SEQ_CST) & bit)
            return;
    __atomic_fetch_and(&g->bits[pg >> 3], (uint8_t)~bit, __ATOMIC_SEQ_CST);
    for (int k = 0; k < n; k++)
        if (__atomic_load_n(&g->jits[k]->bits[pg >> 3], __ATOMIC_SEQ_CST) & bit) {
            __atomic_fetch_or(&g->bits[pg >> 3], bit, __ATOMIC_SEQ_CST);
            mvm_vm *vm = j->c->vm;
            for (int i = 0; i < vm->ncpus; i++)
                if (vm->vcpu[i].cpu)
                    a64_tlb_protect_remote(vm->vcpu[i].cpu, pg << 12);
            return;
        }
}

static void inval_page(struct a64_jit *j, uint64_t pg)
{
    if (pg >= j->npages)
        return;
    jblock *b = j->pages[pg];
    if (!b)
        return;
    for (; b; b = b->pnext) {
        if (!b->valid)
            continue;
        b->valid = false;
        if (!b->interp) { /* entrada do bloco -> stub que volta ao C (cache de saltos, encadeamentos) */
            *b->code = 0x94000000u | ((uint32_t)(int32_t)(j->stub_dead - b->code) & 0x3ffffff); /* BL */
            __builtin___clear_cache((char *)b->code, (char *)(b->code + 1));
        }
        unsigned h = hash_of(b->va, b->pa);
        for (jblock **pp = &j->hash[h]; *pp; pp = &(*pp)->hnext)
            if (*pp == b) {
                *pp = b->hnext;
                break;
            }
        j->n_inval++;
    }
    j->pages[pg] = NULL;
    uint8_t bit = (uint8_t)(1u << (pg & 7));
    __atomic_fetch_and(&j->bits[pg >> 3], (uint8_t)~bit, __ATOMIC_SEQ_CST);
    group_page_dropped(j, pg);

    void *e = j->c->jit_exit;
    if (e && !rec_blk(e)->valid)
        j->c->jit_exit = NULL;
}

static void code_hook(void *opaque, uint64_t pa, uint64_t len)
{
    struct a64_jit_group *g = opaque;
    int n = atomic_load(&g->n);
    for (uint64_t p = pa >> 12; p <= (pa + (len ? len - 1 : 0)) >> 12; p++) {
        if (p >= g->npages)
            break;
        uint8_t bit = (uint8_t)(1u << (p & 7));
        for (int k = 0; k < n; k++) {
            struct a64_jit *j = g->jits[k];
            if (!(__atomic_load_n(&j->bits[p >> 3], __ATOMIC_SEQ_CST) & bit))
                continue;
            if (tls_jit == j) {
                inval_page(j, p);
            } else { /* outra thread (outra CPU, DMA): a CPU dona invalida antes de seguir */
                __atomic_fetch_or(&j->pend[p >> 6], 1ULL << (p & 63), __ATOMIC_SEQ_CST);
                atomic_store(&j->pending, 1);
                vm_cpu_kick(j->c->vm, j->c->index);
            }
        }
    }
}

static void process_pending(struct a64_jit *j)
{
    atomic_store(&j->pending, 0);
    for (uint64_t w = 0; w < (j->npages + 63) / 64; w++) {
        uint64_t m = __atomic_exchange_n(&j->pend[w], 0, __ATOMIC_ACQ_REL);
        while (m) {
            int b = __builtin_ctzll(m);
            m &= m - 1;
            inval_page(j, w * 64 + (uint64_t)b);
        }
    }
}

/* marca a pagina como codigo: escritas passam a ir pelo caminho lento */
static void mark_code_page(struct a64_jit *j, uint64_t pa)
{
    uint64_t pg = pa >> 12;
    if (pg >= j->npages || (j->bits[pg >> 3] >> (pg & 7)) & 1)
        return;
    uint8_t bit = (uint8_t)(1u << (pg & 7));
    __atomic_fetch_or(&j->bits[pg >> 3], bit, __ATOMIC_SEQ_CST);
    a64_cpu *c = j->c;
    struct a64_jit_group *g = j->group;
    if (g && !(__atomic_fetch_or(&g->bits[pg >> 3], bit, __ATOMIC_SEQ_CST) & bit)) {
        /* pagina nova no mapa comum: as outras CPUs passam a gravar nela pelo caminho lento */
        mvm_vm *vm = c->vm;
        for (int i = 0; i < vm->ncpus; i++)
            if (vm->vcpu[i].cpu && vm->vcpu[i].cpu != (void *)c)
                a64_tlb_protect_remote(vm->vcpu[i].cpu, pa & ~0xfffULL);
    }
    for (int el = 0; el < 2; el++)
        for (unsigned i = 0; i < A64_TLB_SIZE; i++) {
            a64_tlbe *e = &c->tlb[el][i];
            if (!(e->tag_w & TLB_INVALID) && e->pa == (pa & ~0xfffULL))
                e->tag_w |= TLB_IO;
        }
}

static jblock *lookup(struct a64_jit *j, uint64_t va, uint64_t pa, uint32_t flags)
{
    for (jblock *b = j->hash[hash_of(va, pa)]; b; b = b->hnext)
        if (b->va == va && b->pa == pa && b->flags == flags)
            return b;
    return NULL;
}

/* ------------------------------------------------------------ traducao */

enum { TR_OK, TR_END, TR_STOP };

/* imediato logico valido (DecodeBitMasks sem UNDEFINED) */
static bool bitmask_ok(unsigned sf, unsigned n, unsigned immr, unsigned imms)
{
    (void)immr;
    if (!sf && n)
        return false;
    unsigned v = (n << 6) | (~imms & 0x3f);
    if (!v)
        return false;
    int len = 31 - __builtin_clz(v);
    if (len < 1)
        return false;
    unsigned levels = (1u << len) - 1;
    if ((imms & levels) == levels)
        return false;
    return true;
}

/* desvio direto para target: BL stub_exit_rec + ponteiro do registro */
static void emit_exit_far(struct a64_jit *j, a64 *a, uint64_t target);
static void emit_exit(struct a64_jit *j, a64 *a, jblock *b, uint64_t target)
{
    if ((target ^ b->va) >> 12) {
        emit_exit_far(j, a, target);
        return;
    }
    a64_bl(a, j->stub_exit_rec);
    uint64_t p = (uint64_t)(uintptr_t)b;
    a64_put(a, (uint32_t)target);
    a64_put(a, (uint32_t)(target >> 32));
    a64_put(a, (uint32_t)p);
    a64_put(a, (uint32_t)(p >> 32));
}

/* saida com PC em registrador (desvio indireto) */
static void emit_exit_dyn(struct a64_jit *j, a64 *a, int reg)
{
    a64_mov(a, 1, 10, reg);
    a64_movz(a, 0, 11, (uint16_t)j->flags, 0);
    a64_b(a, j->stub_lookup);
}

/* desvio direto para outra pagina: pelo cache de saltos */
static void emit_exit_far(struct a64_jit *j, a64 *a, uint64_t target)
{
    a64_mov_imm(a, 10, target);
    a64_movz(a, 0, 11, (uint16_t)j->flags, 0);
    a64_b(a, j->stub_lookup);
}

static void add_cold(struct a64_jit *j, int kind, uint32_t *fix, uint32_t *back, uint64_t cur, unsigned size, unsigned opc)
{
    cold_rec *r = &j->cold[j->ncold++];
    r->kind = kind;
    r->fix = fix;
    r->back = back;
    r->cur = cur;
    r->size = size;
    r->opc = opc;
}

/* teste de TLB para [T2, T2+span): x16 = entrada; cai em cbnz para o trecho frio.
 * Retorna o cbnz a corrigir. */
static uint32_t *emit_tlb(a64 *a, bool write, unsigned span)
{
    a64_ubfx(a, 16, T2, 12, A64_TLB_BITS);
    a64_addsub_reg(a, 1, 0, 0, 16, 16, 16, 2); /* x16 *= 5 */
    a64_addsub_reg(a, 1, 0, 0, 16, RTLB, 16, 3); /* x16 = tlb + idx*40 */
    a64_ldr(a, 8, 17, 16, write ? (uint32_t)offsetof(a64_tlbe, tag_w) : (uint32_t)offsetof(a64_tlbe, tag_r));
    a64_add_imm(a, 9, T2, span - 1);
    a64_logic_imm(a, 1, 0, 9, 9, A64_MASK_PAGE_HI);
    a64_logic(a, 1, 2, 0, 9, 9, 17); /* eor */
    uint32_t *fix = a64_here(a);
    a64_cbnz(a, 1, 9, fix);
    a64_ldr(a, 8, 17, 16, (uint32_t)offsetof(a64_tlbe, addend));
    return fix;
}

/* load inteiro de 1 registrador: valor em T0 */
static void emit_load(struct a64_jit *j, a64 *a, unsigned size, unsigned opc, uint64_t cur)
{
    uint32_t *fix = emit_tlb(a, false, 1u << size);
    e_ldst_regoff(a, size, 0, opc, T0, 17, T2);
    add_cold(j, COLD_LD, fix, a64_here(a), cur, size, opc);
}

/* store inteiro de 1 registrador: valor em 'val' (pode ser XZR) */
static void emit_store(struct a64_jit *j, a64 *a, unsigned size, int val, uint64_t cur)
{
    uint32_t *fix = emit_tlb(a, true, 1u << size);
    e_ldst_regoff(a, size, 0, 0, val, 17, T2);
    add_cold(j, COLD_ST, fix, a64_here(a), cur, size, (unsigned)val);
}

static void emit_helper(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur, bool end);
/* trecho frio de cada acesso: x10 = PC da instrucao, BL para o stub compartilhado, volta */
static void emit_cold(struct a64_jit *j, a64 *a)
{
    for (int i = 0; i < j->ncold; i++) {
        cold_rec *r = &j->cold[i];
        a64_patch(r->fix, a64_here(a));
        if (r->kind == COLD_HELPER) {
            emit_helper(j, a, r->insn, r->cur, false);
            a64_b(a, r->back);
            continue;
        }
        uint32_t *stub;
        switch (r->kind) {
        case COLD_LD: stub = j->st_ld[r->size][r->opc]; break;
        case COLD_LDP: stub = j->st_ldp[r->size - 2][r->opc == 1]; break;
        case COLD_ST: stub = j->st_st[r->size]; break;
        default: stub = j->st_stp[r->size - 2]; break;
        }
        a64_mov_imm(a, 10, r->cur);
        a64_bl(a, stub);
        a64_b(a, r->back);
    }
    j->ncold = 0;
}

/* stub do caminho lento: grava cur (x10) e NZCV, chama a64_read_slow/a64_write_slow com o EL
 * atual, devolve o resultado em T0 (T1) e restaura NZCV. Endereco em T2, valores em T0/T1. */
static void e_push_lr(a64 *a) { a64_put(a, 0xF81F0FFEu); } /* str x30, [sp, #-16]! */
static void e_pop_lr(a64 *a) { a64_put(a, 0xF84107FEu); }  /* ldr x30, [sp], #16 */
static uint32_t *build_slow_stub(struct a64_jit *j, a64 *a, int kind, unsigned size, unsigned opc)
{
    (void)j;
    uint32_t *start = a64_here(a);
    unsigned bytes = 1u << size;
    e_push_lr(a);
    a64_str(a, 8, 10, RC, OFF(cur));
    e_mrs_nzcv(a, RFL);
    a64_lsr_imm(a, 9, RFL, 28);
    a64_str(a, 4, 9, RC, OFF(nzcv));
    switch (kind) {
    case COLD_LD:
    case COLD_LDP: {
        int n = kind == COLD_LDP ? 2 : 1;
        for (int k = 0; k < n; k++) {
            a64_mov(a, 1, 0, RC);
            if (k)
                a64_add_imm(a, 1, T2, bytes);
            else
                a64_mov(a, 1, 1, T2);
            a64_movz(a, 0, 2, (uint16_t)bytes, 0);
            a64_ldr(a, 4, 3, RC, OFF(el));
            e_call(a, (void *)a64_read_slow);
            int rt = k ? T1 : T0;
            if (kind == COLD_LDP) {
                if (opc == 1)
                    a64_sext(a, rt, 0, 4);
                else
                    a64_mov(a, 1, rt, 0);
            } else if (opc == 2) {
                a64_sext(a, rt, 0, (int)bytes);
            } else if (opc == 3) {
                a64_sbfm(a, 0, rt, 0, 0, (int)bytes * 8 - 1);
            } else {
                a64_mov(a, 1, rt, 0);
            }
        }
        break;
    }
    case COLD_ST:
        a64_mov(a, 1, 0, RC);
        a64_mov(a, 1, 1, T2);
        a64_mov(a, 1, 2, T0);
        a64_movz(a, 0, 3, (uint16_t)bytes, 0);
        a64_ldr(a, 4, 4, RC, OFF(el));
        e_call(a, (void *)a64_write_slow);
        break;
    default: /* STP: segundo antes do primeiro (como o interpretador) */
        a64_mov(a, 1, 0, RC);
        a64_add_imm(a, 1, T2, bytes);
        a64_mov(a, 1, 2, T1);
        a64_movz(a, 0, 3, (uint16_t)bytes, 0);
        a64_ldr(a, 4, 4, RC, OFF(el));
        e_call(a, (void *)a64_write_slow);
        a64_mov(a, 1, 0, RC);
        a64_mov(a, 1, 1, T2);
        a64_mov(a, 1, 2, T0);
        a64_movz(a, 0, 3, (uint16_t)bytes, 0);
        a64_ldr(a, 4, 4, RC, OFF(el));
        e_call(a, (void *)a64_write_slow);
        break;
    }
    e_msr_nzcv(a, RFL);
    e_pop_lr(a);
    a64_ret(a);
    return start;
}

/* instrucao de dados emitida quase igual: ctx de cada campo (Rd, Rn, Rm, Ra) */
static void dp_verbatim(struct a64_jit *j, a64 *a, uint32_t insn, int cd, int cn, int cm, int ca, bool reads_rd)
{
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31, ra = (insn >> 10) & 31;
    uint32_t out = insn;
    if (cn)
        out = setf(out, 5, src(j, a, cn, rn, T1));
    if (cm)
        out = setf(out, 16, src(j, a, cm, rm, T3));
    if (ca)
        out = setf(out, 10, src(j, a, ca, ra, T4));
    if (cd == CTX_NONE) { /* CCMP/CCMN: bits 0-4 sao o NZCV imediato */
        a64_put(a, out);
        return;
    }
    int hd;
    if (rd == 31 && cd == CTX_ZR) {
        hd = XZR;
    } else {
        hd = T0;
        if (reads_rd)
            a64_ldr(a, 8, T0, RC, rd == 31 ? OFF_SP(j->sp_idx) : OFF_X(rd));
    }
    out = setf(out, 0, hd);
    a64_put(a, out);
    if (hd != XZR)
        dst(j, a, cd, rd, T0);
}

static uint64_t sext(uint64_t v, unsigned bits) { return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits)); }

/* endereco de load/store de registrador em T2; devolve false se a forma nao e traduzida.
 * wb: 0 = sem escrita de volta, 1 = pos (T1 = novo valor da base), 2 = pre (base = T2) */
static int ldst_addr(struct a64_jit *j, a64 *a, uint32_t insn, unsigned scale, int *wb)
{
    unsigned rn = (insn >> 5) & 31;
    *wb = 0;
    int base = src(j, a, CTX_SP, rn, T2);
    if (insn & (1u << 24)) { /* imediato sem sinal */
        uint64_t off = (uint64_t)((insn >> 10) & 0xfff) << scale;
        if (off < 4096) {
            a64_add_imm(a, T2, base, (uint32_t)off);
        } else {
            a64_mov_imm(a, 9, off);
            a64_add(a, T2, base, 9);
        }
        return 1;
    }
    if (!(insn & (1u << 21))) {
        int64_t imm = (int64_t)sext((insn >> 12) & 0x1ff, 9);
        unsigned mode = (insn >> 10) & 3;
        if (mode == 2)
            return 0; /* LDTR/STTR */
        if (mode == 1) { /* pos: acessa a base, depois base += imm */
            if (imm >= 0)
                a64_add_imm(a, T1, base, (uint32_t)imm);
            else
                a64_sub_imm(a, T1, base, (uint32_t)-imm);
            *wb = 1;
            return 1;
        }
        if (imm >= 0)
            a64_add_imm(a, T2, base, (uint32_t)imm);
        else
            a64_sub_imm(a, T2, base, (uint32_t)-imm);
        if (mode == 3)
            *wb = 2;
        return 1;
    }
    if (((insn >> 10) & 3) != 2)
        return 0; /* atomicos / PAuth */
    unsigned opt = (insn >> 13) & 7, rm = (insn >> 16) & 31;
    if (!(opt & 2))
        return 0;
    int hm = src(j, a, CTX_ZR, rm, T3);
    e_add_ext(a, T2, base, hm, opt, (insn & (1u << 12)) ? scale : 0);
    return 1;
}

static void ldst_writeback(struct a64_jit *j, a64 *a, uint32_t insn, int wb)
{
    unsigned rn = (insn >> 5) & 31;
    if (wb == 1)
        dst(j, a, CTX_SP, rn, T1);
    else if (wb == 2)
        dst(j, a, CTX_SP, rn, T2);
}

static int tr_ldst_reg(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur)
{
    unsigned size = insn >> 30, opc = (insn >> 22) & 3, rt = insn & 31;
    bool imm9 = !(insn & (1u << 24)) && !(insn & (1u << 21));
    unsigned mode = (insn >> 10) & 3;
    if (opc == 3 && size >= 2)
        return TR_STOP;
    if (opc == 2 && size == 3) { /* PRFM: NOP (pre/pos indexado nao existe) */
        if (imm9 && (mode == 1 || mode == 3))
            return TR_STOP;
        if (imm9 && mode == 2)
            return TR_STOP;
        if (!(insn & (1u << 24)) && (insn & (1u << 21)) && ((insn >> 10) & 3) != 2)
            return TR_STOP;
        return TR_OK;
    }
    int wb;
    if (!ldst_addr(j, a, insn, size, &wb))
        return TR_STOP;
    if (opc == 0) {
        int v = src(j, a, CTX_ZR, rt, T0);
        if (v == XZR) {
            a64_mov(a, 1, T0, XZR);
            v = T0;
        }
        emit_store(j, a, size, v, cur);
    } else {
        emit_load(j, a, size, opc, cur);
        dst(j, a, CTX_ZR, rt, T0);
    }
    ldst_writeback(j, a, insn, wb);
    return TR_OK;
}

static int tr_ldst_pair(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur)
{
    unsigned opc = insn >> 30, mode = (insn >> 23) & 3, L = (insn >> 22) & 1;
    unsigned rt2 = (insn >> 10) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    if (opc == 3 || (opc == 1 && !L))
        return TR_STOP;
    unsigned scale = 2 + (opc >> 1), bytes = 1u << scale;
    int64_t off = (int64_t)sext((insn >> 15) & 0x7f, 7) * (int64_t)bytes;
    int base = src(j, a, CTX_SP, rn, T2);
    if (mode == 1)
        a64_mov(a, 1, T2, base);
    else if (off >= 0)
        a64_add_imm(a, T2, base, (uint32_t)off);
    else
        a64_sub_imm(a, T2, base, (uint32_t)-off);
    if (!L) { /* valores em T0/T1 (o trecho frio usa os mesmos) */
        int v0 = src(j, a, CTX_ZR, rt, T0), v1 = src(j, a, CTX_ZR, rt2, T1);
        if (v0 == XZR)
            a64_mov(a, 1, T0, XZR);
        if (v1 == XZR)
            a64_mov(a, 1, T1, XZR);
    }
    uint32_t *fix = emit_tlb(a, !L, 2 * bytes);
    a64_add(a, 17, 17, T2);
    if (L) {
        e_ldstp(a, opc, 0, 1, T0, T1, 17);
        add_cold(j, COLD_LDP, fix, a64_here(a), cur, scale, opc);
        dst(j, a, CTX_ZR, rt, T0);
        dst(j, a, CTX_ZR, rt2, T1);
    } else {
        e_ldstp(a, opc, 0, 0, T0, T1, 17);
        add_cold(j, COLD_STP, fix, a64_here(a), cur, scale, opc);
    }
    if (mode == 1 || mode == 3) {
        if (mode == 1) {
            if (off >= 0)
                a64_add_imm(a, T2, T2, (uint32_t)off);
            else
                a64_sub_imm(a, T2, T2, (uint32_t)-off);
        }
        dst(j, a, CTX_SP, rn, T2);
    }
    return TR_OK;
}

/* classes para MVM_JIT_NO: 1 dados imediato, 2 dados registrador, 4 load/store, 8 par,
 * 16 desvios condicionais, 32 desvios diretos/indiretos */
static int tr_insn2(struct a64_jit *j, a64 *a, jblock *b, uint32_t insn, uint64_t cur);
static int tr_insn1(struct a64_jit *j, a64 *a, jblock *b, uint32_t insn, uint64_t cur);
static void count_stop(struct a64_jit *j, uint32_t insn);
/* ---------------- loads/stores SIMD/FP (V = 1): acesso nativo; caminho lento = interpretador */
static inline void e_ldr_q_off(a64 *a, int qt, int rn, uint32_t off) { a64_put(a, 0x3DC00000u | ((off / 16) << 10) | ((uint32_t)rn << 5) | (uint32_t)qt); }
static inline void e_str_q_off(a64 *a, int qt, int rn, uint32_t off) { a64_put(a, 0x3D800000u | ((off / 16) << 10) | ((uint32_t)rn << 5) | (uint32_t)qt); }

static int cold_helper(struct a64_jit *j, uint32_t *fix, uint64_t cur, uint32_t insn)
{
    cold_rec *r = &j->cold[j->ncold];
    r->kind = COLD_HELPER;
    r->fix = fix;
    r->back = NULL;
    r->cur = cur;
    r->insn = insn;
    return j->ncold++;
}

static int tr_simd_ldst_reg(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur)
{
    unsigned size = insn >> 30, opc = (insn >> 22) & 3, rt = insn & 31;
    unsigned scale = size | ((opc & 2) << 1);
    if (scale > 4)
        return TR_STOP;
    int wb;
    if (!ldst_addr(j, a, insn, scale, &wb))
        return TR_STOP;
    unsigned bytes = 1u << scale;
    bool load = opc & 1;
    if (!load)
        e_ldr_q_off(a, VT0, RC, OFF_V(rt));
    uint32_t *fix = emit_tlb(a, !load, bytes);
    e_ldst_regoff(a, size, 1, opc, VT0, 17, T2);
    if (load)
        e_str_q_off(a, VT0, RC, OFF_V(rt));
    int ci = cold_helper(j, fix, cur, insn);
    ldst_writeback(j, a, insn, wb);
    j->cold[ci].back = a64_here(a);
    return TR_OK;
}

static int tr_simd_ldst_pair(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur)
{
    unsigned opc = insn >> 30, mode = (insn >> 23) & 3, L = (insn >> 22) & 1;
    unsigned rt2 = (insn >> 10) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    if (opc == 3)
        return TR_STOP;
    unsigned scale = 2 + opc, bytes = 1u << scale;
    int64_t off = (int64_t)sext((insn >> 15) & 0x7f, 7) * (int64_t)bytes;
    int base = src(j, a, CTX_SP, rn, T2);
    if (mode == 1)
        a64_mov(a, 1, T2, base);
    else if (off >= 0 && off < 4096)
        a64_add_imm(a, T2, base, (uint32_t)off);
    else if (off < 0 && -off < 4096)
        a64_sub_imm(a, T2, base, (uint32_t)-off);
    else {
        a64_mov_imm(a, 9, (uint64_t)off);
        a64_add(a, T2, base, 9);
    }
    if (!L) {
        e_ldr_q_off(a, VT0, RC, OFF_V(rt));
        e_ldr_q_off(a, VT1, RC, OFF_V(rt2));
    }
    uint32_t *fix = emit_tlb(a, !L, 2 * bytes);
    a64_add(a, 17, 17, T2);
    e_ldstp(a, opc, 1, L, VT0, VT1, 17);
    if (L) {
        e_str_q_off(a, VT0, RC, OFF_V(rt));
        e_str_q_off(a, VT1, RC, OFF_V(rt2));
    }
    int ci = cold_helper(j, fix, cur, insn);
    if (mode == 1 || mode == 3) {
        if (mode == 1) {
            if (off >= 0)
                a64_add_imm(a, T2, T2, (uint32_t)off);
            else
                a64_sub_imm(a, T2, T2, (uint32_t)-off);
        }
        dst(j, a, CTX_SP, rn, T2);
    }
    j->cold[ci].back = a64_here(a);
    return TR_OK;
}

static int tr_simd_literal(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur)
{
    unsigned opc = insn >> 30, rt = insn & 31;
    if (opc == 3)
        return TR_STOP;
    uint64_t addr = cur + (sext((insn >> 5) & 0x7ffff, 19) << 2);
    a64_mov_imm(a, T2, addr);
    unsigned bytes = 4u << opc;
    uint32_t *fix = emit_tlb(a, false, bytes);
    /* LDR S/D/Q [x17, T2]: size/opc da forma com registrador */
    unsigned size = opc == 2 ? 0 : 2 + opc, ropc = opc == 2 ? 3 : 1;
    e_ldst_regoff(a, size, 1, ropc, VT0, 17, T2);
    e_str_q_off(a, VT0, RC, OFF_V(rt));
    int ci = cold_helper(j, fix, cur, insn);
    j->cold[ci].back = a64_here(a);
    return TR_OK;
}

/* ---------------- SIMD/FP de dados: emitidas quase iguais com Vd/Vn/Vm/Va em v16-v19 do host.
 * So classes da base ARMv8.0 que o interpretador implementa (sem FP16, dot product, RDM, FCMLA,
 * cripto); FP so com FPCR do convidado = 0 (o do host). Uma codificacao nao alocada que escape
 * vira UNDEF do convidado pelo tratador de SIGILL (tabela b->map). */
#define VD 16
#define VN 17
#define VM 18
#define VA 19
enum { F_VD = 1, F_VN = 2, F_VM = 4, F_VA = 8, F_GD = 16, F_GN = 32, F_NODST = 64 };

static void simd_emit(struct a64_jit *j, a64 *a, uint32_t insn, unsigned f)
{
    unsigned rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31, ra = (insn >> 10) & 31;
    uint32_t out = insn;
    if (f & F_VN) {
        e_ldr_q_off(a, VN, RC, OFF_V(rn));
        out = setf(out, 5, VN);
    } else if (f & F_GN) {
        out = setf(out, 5, src(j, a, CTX_ZR, rn, T1));
    }
    if (f & F_VM) {
        e_ldr_q_off(a, VM, RC, OFF_V(rm));
        out = setf(out, 16, VM);
    }
    if (f & F_VA) {
        e_ldr_q_off(a, VA, RC, OFF_V(ra));
        out = setf(out, 10, VA);
    }
    int gd = -1;
    if (f & F_VD) {
        e_ldr_q_off(a, VD, RC, OFF_V(rd)); /* muitas leem Vd (INS, MLA, BSL, ORR imediato...) */
        out = setf(out, 0, VD);
    } else if (f & F_GD) {
        gd = rd == 31 ? XZR : T0;
        out = setf(out, 0, gd);
    }
    j->map[j->nmap++] = ((uint32_t)(a64_here(a) - j->blk_code) << 16) | j->cur_idx;
    a64_put(a, out);
    if (f & F_VD)
        e_str_q_off(a, VD, RC, OFF_V(rd));
    else if (gd == T0)
        a64_str(a, 8, T0, RC, OFF_X(rd));
}

static int tr_fp(struct a64_jit *j, a64 *a, uint32_t insn)
{
    unsigned ftype = (insn >> 22) & 3;
    if ((insn >> 31) & 1 || (insn >> 29) & 1) /* M, S */
        return TR_STOP;
    bool st = ftype <= 1; /* S ou D */
    if (insn & (1u << 24)) { /* FMADD/FMSUB/FNMADD/FNMSUB */
        if (!st)
            return TR_STOP;
        simd_emit(j, a, insn, F_VD | F_VN | F_VM | F_VA);
        return TR_OK;
    }
    if (!(insn & (1u << 21)))
        return TR_STOP; /* ponto fixo */
    switch ((insn >> 10) & 3) {
    case 1: /* FCCMP/FCCMPE: bits 0-4 = NZCV imediato */
        if (!st)
            return TR_STOP;
        simd_emit(j, a, insn, F_VN | F_VM);
        return TR_OK;
    case 2: /* 2 fontes */
        if (!st || ((insn >> 12) & 15) > 8)
            return TR_STOP;
        simd_emit(j, a, insn, F_VD | F_VN | F_VM);
        return TR_OK;
    case 3: /* FCSEL */
        if (!st)
            return TR_STOP;
        simd_emit(j, a, insn, F_VD | F_VN | F_VM);
        return TR_OK;
    }
    if (insn & (1u << 12)) { /* FMOV imediato */
        if (!st || ((insn >> 5) & 31))
            return TR_STOP;
        simd_emit(j, a, insn, F_VD);
        return TR_OK;
    }
    if (((insn >> 10) & 0xf) == 8 && !((insn >> 14) & 3)) { /* FCMP/FCMPE */
        unsigned op2 = insn & 31;
        if (!st || (op2 & 7))
            return TR_STOP;
        simd_emit(j, a, insn, (op2 & 8) ? F_VN : F_VN | F_VM);
        return TR_OK;
    }
    if (((insn >> 10) & 0x1f) == 0x10) { /* 1 fonte */
        unsigned opc = (insn >> 15) & 63;
        bool ok;
        if (opc <= 3)
            ok = st;
        else if (opc == 4 || opc == 5 || opc == 7)
            ok = ftype != 2 && !(ftype == 0 && opc == 4) && !(ftype == 1 && opc == 5) && !(ftype == 3 && opc == 7);
        else if (opc >= 8 && opc <= 15)
            ok = st && opc != 13;
        else
            ok = false;
        if (!ok)
            return TR_STOP;
        simd_emit(j, a, insn, F_VD | F_VN);
        return TR_OK;
    }
    if (!((insn >> 10) & 0x3f)) { /* conversao FP <-> inteiro */
        unsigned sf = insn >> 31, rmode = (insn >> 19) & 3, opc = (insn >> 16) & 7;
        (void)sf;
        sf = (insn >> 31) & 1;
        if (opc == 6 || opc == 7) { /* FMOV */
            bool ok = (rmode == 0 && ((sf == 0 && ftype == 0) || (sf == 1 && ftype == 1))) ||
                      (rmode == 1 && sf == 1 && ftype == 2);
            if (!ok)
                return TR_STOP;
            simd_emit(j, a, insn, opc == 6 ? (F_GD | F_VN) : (F_VD | F_GN));
            return TR_OK;
        }
        if (!st)
            return TR_STOP;
        if (opc <= 1) { /* FCVT{N,P,M,Z}{S,U} */
            simd_emit(j, a, insn, F_GD | F_VN);
            return TR_OK;
        }
        if (rmode)
            return TR_STOP;
        if (opc == 4 || opc == 5) { /* FCVTA{S,U} */
            simd_emit(j, a, insn, F_GD | F_VN);
            return TR_OK;
        }
        simd_emit(j, a, insn, F_VD | F_GN); /* SCVTF/UCVTF */
        return TR_OK;
    }
    return TR_STOP;
}

static int tr_simd(struct a64_jit *j, a64 *a, uint32_t insn)
{
    if ((insn >> 28) & 1 && !((insn >> 30) & 1))
        return tr_fp(j, a, insn);
    unsigned U = (insn >> 29) & 1, size = (insn >> 22) & 3;
    const unsigned V3 = F_VD | F_VN | F_VM, V2 = F_VD | F_VN;
    if ((insn & 0x9f200400) == 0x0e200400 || (insn & 0xdf200400) == 0x5e200400) { /* 3 iguais */
        simd_emit(j, a, insn, V3);
        return TR_OK;
    }
    if ((insn & 0x9f200c00) == 0x0e200000) { /* 3 diferentes */
        if (((insn >> 12) & 15) == 15)
            return TR_STOP;
        simd_emit(j, a, insn, V3);
        return TR_OK;
    }
    if ((insn & 0x9f3e0c00) == 0x0e200800 || (insn & 0xdf3e0c00) == 0x5e200800) { /* 2 misc */
        simd_emit(j, a, insn, V2);
        return TR_OK;
    }
    if ((insn & 0x9f3e0c00) == 0x0e300800) { /* atraves das faixas */
        unsigned opc = (insn >> 12) & 31;
        bool fp = opc == 12 || opc == 15;
        if (!(opc == 3 || opc == 10 || opc == 26 || opc == 27 || (fp && U)))
            return TR_STOP;
        simd_emit(j, a, insn, V2);
        return TR_OK;
    }
    if ((insn & 0x9fe08400) == 0x0e000400) { /* copia (vetor) */
        unsigned op = (insn >> 29) & 1, imm4 = (insn >> 11) & 15;
        if (!((insn >> 16) & 15)) /* imm5 sem tamanho */
            return TR_STOP;
        if (op) { /* INS (elemento) */
            simd_emit(j, a, insn, V2);
            return TR_OK;
        }
        switch (imm4) {
        case 0: simd_emit(j, a, insn, V2); return TR_OK;          /* DUP elemento */
        case 1: simd_emit(j, a, insn, F_VD | F_GN); return TR_OK; /* DUP geral */
        case 3: simd_emit(j, a, insn, F_VD | F_GN); return TR_OK; /* INS geral */
        case 5: case 7: simd_emit(j, a, insn, F_GD | F_VN); return TR_OK; /* SMOV/UMOV */
        default: return TR_STOP;
        }
    }
    if ((insn & 0xdfe08400) == 0x5e000400) { /* copia escalar: so DUP (MOV) */
        if ((insn >> 29) & 1 || ((insn >> 11) & 15) || !((insn >> 16) & 15))
            return TR_STOP;
        simd_emit(j, a, insn, V2);
        return TR_OK;
    }
    if ((insn & 0x9f000400) == 0x0f000000 || (insn & 0xdf000400) == 0x5f000000) { /* por elemento */
        unsigned opc = (insn >> 12) & 15;
        bool fp = (!U && (opc == 1 || opc == 5 || opc == 9)) || (U && opc == 9);
        bool in = (!U && (opc == 2 || opc == 3 || opc == 6 || opc == 7 || opc == 8 || opc == 10 || opc == 11 ||
                          opc == 12 || opc == 13)) ||
                  (U && (opc == 0 || opc == 2 || opc == 4 || opc == 6 || opc == 10));
        if (fp && size < 2)
            return TR_STOP;
        if (!fp && !in)
            return TR_STOP;
        if (!fp && (size == 0 || size == 3))
            return TR_STOP;
        simd_emit(j, a, insn, V3);
        return TR_OK;
    }
    if ((insn & 0x9ff80400) == 0x0f000400) { /* imediato modificado */
        unsigned cmode = (insn >> 12) & 15, op = (insn >> 29) & 1, Q = (insn >> 30) & 1;
        if (cmode == 15 && (op ? !Q : ((insn >> 11) & 1)))
            return TR_STOP;
        simd_emit(j, a, insn, F_VD);
        return TR_OK;
    }
    if ((insn & 0x9f800400) == 0x0f000400 || (insn & 0xdf800400) == 0x5f000400) { /* deslocamento */
        simd_emit(j, a, insn, V2);
        return TR_OK;
    }
    if ((insn & 0xbf208c00) == 0x0e000000) { /* TBL/TBX de 1 registrador */
        if ((insn >> 13) & 3)
            return TR_STOP;
        simd_emit(j, a, insn, V3);
        return TR_OK;
    }
    if ((insn & 0xbf208c00) == 0x0e000800 || (insn & 0xbf208400) == 0x2e000000) { /* ZIP/UZP/TRN, EXT */
        simd_emit(j, a, insn, V3);
        return TR_OK;
    }
    if ((insn & 0xdf3e0c00) == 0x5e300800) { /* pares escalar */
        unsigned opc = (insn >> 12) & 31;
        bool ok = (!U && opc == 27 && size == 3) || (U && (opc == 12 || opc == 13 || opc == 15));
        if (!ok)
            return TR_STOP;
        simd_emit(j, a, insn, V2);
        return TR_OK;
    }
    return TR_STOP;
}

/* SIGILL no codigo traduzido (codificacao que o host nao aceita): UNDEF do convidado */
static struct sigaction old_ill;
static void sigill_handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    struct a64_jit *j = tls_jit;
    uint32_t *pc = (uint32_t *)(uintptr_t)uc->uc_mcontext.pc;
    if (j && pc >= j->code && pc < j->code_end) {
        jblock *best = NULL;
        for (unsigned i = 0; i < j->nblocks; i++) {
            jblock *b = &j->blocks[i];
            if (b->code <= pc && (!best || b->code > best->code))
                best = b;
        }
        if (best && best->map) {
            uint32_t off = (uint32_t)(pc - best->code);
            for (unsigned i = 0; i < best->nmap; i++)
                if ((best->map[i] >> 16) == off) {
                    a64_cpu *c = j->c;
                    c->cur = best->va + 4u * (best->map[i] & 0xffff);
                    c->pc = c->cur + 4;
                    c->nzcv = (uint32_t)(uc->uc_mcontext.pstate >> 28) & 0xf;
                    sigset_t m;
                    sigemptyset(&m);
                    sigaddset(&m, SIGILL);
                    pthread_sigmask(SIG_UNBLOCK, &m, NULL);
                    LOGW("JIT ARM64: instrucao %08x em %llx recusada pelo host: UNDEF no convidado",
                         *(uint32_t *)(uintptr_t)uc->uc_mcontext.pc, (unsigned long long)c->cur);
                    a64_undef(c);
                }
        }
    }
    if (old_ill.sa_flags & SA_SIGINFO) {
        if (old_ill.sa_sigaction)
            old_ill.sa_sigaction(sig, si, ucv);
    } else if (old_ill.sa_handler != SIG_IGN && old_ill.sa_handler != SIG_DFL) {
        old_ill.sa_handler(sig);
    } else {
        signal(SIGILL, SIG_DFL);
        raise(SIGILL);
    }
}

#define SRK(op0, op1, crn, crm, op2) (((op0) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
/* ---------------- LDXR/LDAXR/STXR/STLXR (1 registrador): monitor do interpretador (1 CPU) */
static int tr_exclusive(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur)
{
    unsigned size = insn >> 30, L = (insn >> 22) & 1, o1 = (insn >> 21) & 1, o2 = (insn >> 23) & 1;
    unsigned rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    if (o2 || o1) /* LDAR/STLR tratados antes; par/CAS: interpretador */
        return TR_STOP;
    if (j->c->smp) /* varias CPUs: o interpretador faz compare-and-swap de verdade */
        return TR_STOP;
    unsigned bytes = 1u << size;
    a64_mov(a, 1, T2, src(j, a, CTX_SP, rn, T2));
    if (L) {
        emit_load(j, a, size, 1, cur);
        dst(j, a, CTX_ZR, rt, T0);
        a64_str(a, 8, T2, RC, OFF(excl_addr));
        a64_str(a, 8, T0, RC, OFF(excl_val));
        a64_movz(a, 0, 9, (uint16_t)bytes, 0);
        a64_str(a, 4, 9, RC, OFF(excl_size));
        a64_movz(a, 0, 9, 1, 0);
        a64_str(a, 1, 9, RC, OFF(excl_valid));
        return TR_OK;
    }
    /* STXR: sucesso se o monitor esta ativo no mesmo endereco e tamanho */
    a64_ldr(a, 1, 9, RC, OFF(excl_valid));
    uint32_t *f1 = a64_here(a);
    a64_cbz(a, 0, 9, f1);
    a64_ldr(a, 8, 10, RC, OFF(excl_addr));
    a64_logic(a, 1, 2, 0, 10, 10, T2);
    uint32_t *f2 = a64_here(a);
    a64_cbnz(a, 1, 10, f2);
    a64_ldr(a, 4, 10, RC, OFF(excl_size));
    a64_movz(a, 0, 11, (uint16_t)bytes, 0);
    a64_logic(a, 0, 2, 0, 10, 10, 11);
    uint32_t *f3 = a64_here(a);
    a64_cbnz(a, 0, 10, f3);
    int v = src(j, a, CTX_ZR, rt, T0);
    if (v == XZR)
        a64_mov(a, 1, T0, XZR);
    emit_store(j, a, size, T0, cur);
    a64_movz(a, 0, T4, 0, 0);
    uint32_t *done = a64_here(a);
    a64_b(a, done);
    e_patch(f1, a64_here(a));
    e_patch(f2, a64_here(a));
    e_patch(f3, a64_here(a));
    a64_movz(a, 0, T4, 1, 0);
    a64_patch(done, a64_here(a));
    a64_str(a, 1, XZR, RC, OFF(excl_valid));
    dst(j, a, CTX_ZR, rs, T4);
    return TR_OK;
}

/* ---------------- MRS/MSR frequentes sem chamar o interpretador */
static int tr_sysreg(struct a64_jit *j, a64 *a, uint32_t insn)
{
    unsigned L = (insn >> 21) & 1, rt = insn & 31;
    uint32_t key = (insn >> 5) & 0xffff; /* op0:op1:CRn:CRm:op2 */
    bool el1 = j->el == 1;
    int off = -1;
    switch (key) {
    case SRK(3, 3, 13, 0, 2): off = (int)OFF(tpidr0); break;              /* TPIDR_EL0 */
    case SRK(3, 3, 13, 0, 3): if (L || el1) off = (int)OFF(tpidrro0); break; /* TPIDRRO_EL0 */
    case SRK(3, 0, 13, 0, 4): if (el1) off = (int)OFF(tpidr1); break;    /* TPIDR_EL1 */
    case SRK(3, 0, 4, 1, 0): if (el1) off = (int)OFF_SP(0); break;       /* SP_EL0 */
    case SRK(3, 3, 4, 2, 0): /* NZCV: e o NZCV do host */
        if (L) {
            if (rt != 31) {
                e_mrs_nzcv(a, T0);
                a64_str(a, 8, T0, RC, OFF_X(rt));
            }
        } else {
            e_msr_nzcv(a, src(j, a, CTX_ZR, rt, T0));
        }
        return TR_OK;
    case SRK(3, 0, 4, 2, 2): /* CurrentEL */
        if (!L || !el1)
            return TR_STOP;
        if (rt != 31) {
            a64_movz(a, 1, T0, (uint16_t)(j->el << 2), 0);
            a64_str(a, 8, T0, RC, OFF_X(rt));
        }
        return TR_OK;
    case SRK(3, 3, 4, 2, 1): /* DAIF (leitura) */
        if (!L || !el1)
            return TR_STOP;
        if (rt != 31) {
            a64_ldr(a, 4, T0, RC, OFF(daif));
            a64_lsl_imm(a, T0, T0, 6);
            a64_str(a, 8, T0, RC, OFF_X(rt));
        }
        return TR_OK;
    }
    if (off < 0)
        return TR_STOP;
    if (L) {
        if (rt != 31) {
            a64_ldr(a, 8, T0, RC, (uint32_t)off);
            a64_str(a, 8, T0, RC, OFF_X(rt));
        }
    } else {
        a64_str(a, 8, src(j, a, CTX_ZR, rt, T0), RC, (uint32_t)off);
    }
    return TR_OK;
}


/* instrucao executada pelo interpretador dentro do bloco: o bloco termina depois dela se ela
 * pode mudar o estado de controle (MMU, EL, DAIF, PC, CPACR...) */
static bool helper_ends_block(uint32_t insn)
{
    if ((insn & 0xff000000) == 0xd4000000) /* SVC/HVC/SMC/BRK/HLT */
        return true;
    if ((insn & 0xfe000000) == 0xd6000000) /* desvios por registrador (ERET, PAuth) */
        return true;
    if ((insn & 0xffc00000) != 0xd5000000)
        return false; /* load/store, SIMD/FP, dados: nao mudam o controle */
    unsigned L = (insn >> 21) & 1, op0 = (insn >> 19) & 3, op1 = (insn >> 16) & 7;
    unsigned crn = (insn >> 12) & 15, crm = (insn >> 8) & 15, op2 = (insn >> 5) & 7;
    if (op0 == 0) {
        if (crn == 3 && op1 == 3) /* barreiras: ISB termina */
            return op2 == 6;
        if (crn == 4) /* MSR imediato: so DAIFSet continua */
            return !(op1 == 3 && op2 == 6);
        return true;
    }
    if (op0 == 1) { /* SYS: IC e TLBI terminam; DC/AT continuam */
        if (L)
            return true;
        if (crn == 7)
            return crm == 5 || crm == 1;
        return true;
    }
    if (L) /* MRS */
        return false;
    switch (SRK(op0, op1, crn, crm, op2)) {
    case SRK(3, 3, 13, 0, 2): case SRK(3, 0, 13, 0, 4): case SRK(3, 3, 13, 0, 3): /* TPIDR* */
    case SRK(3, 0, 4, 1, 0): /* SP_EL0 */
    case SRK(3, 0, 4, 0, 1): case SRK(3, 0, 4, 0, 0): /* ELR, SPSR */
    case SRK(3, 0, 5, 2, 0): case SRK(3, 0, 6, 0, 0): /* ESR, FAR */
    case SRK(3, 0, 5, 1, 0): case SRK(3, 0, 5, 1, 1): case SRK(3, 0, 7, 4, 0): /* AFSR0/1, PAR */
    case SRK(3, 0, 13, 0, 1): /* CONTEXTIDR */
    case SRK(3, 3, 4, 2, 0): /* NZCV */
    case SRK(2, 0, 0, 2, 2): /* MDSCR */
    case SRK(3, 0, 12, 0, 0): /* VBAR */
    case SRK(3, 0, 14, 1, 0): /* CNTKCTL */
    case SRK(3, 3, 14, 2, 0): case SRK(3, 3, 14, 2, 1): case SRK(3, 3, 14, 2, 2): /* CNTP */
    case SRK(3, 3, 14, 3, 0): case SRK(3, 3, 14, 3, 1): case SRK(3, 3, 14, 3, 2): /* CNTV */
        return false;
    default:
        return true;
    }
}

static jblock *lookup(struct a64_jit *j, uint64_t va, uint64_t pa, uint32_t flags);
static unsigned jc_hash(uint64_t va);

/* chamado do codigo traduzido depois de instrucoes de controle e em faltas do cache de saltos:
 * o bloco ja traduzido para c->pc, sem sair para o C. NULL = sair (interrupcao pendente, CPU
 * parada, invalidacoes pendentes, bloco ainda nao traduzido). Pode gerar falta de busca. */
static uint32_t *a64_jit_next(a64_cpu *c)
{
    struct a64_jit *j = c->jit;
    if ((c->irq_line && !(c->daif & 2)) || (c->fiq_line && !(c->daif & 1)) || c->halted ||
        atomic_load_explicit(c->exit_flag, memory_order_relaxed) ||
        atomic_load_explicit(&j->pending, memory_order_relaxed))
        return NULL;
    uint64_t pc = c->pc;
    c->cur = pc;
    a64_tlbe *e = a64_tlb_exec(c, pc);
    if (e->tag_x & TLB_IO)
        return NULL;
    uint64_t pa = e->pa | (pc & 0xfff);
    uint32_t flags = (uint32_t)c->el | ((uint32_t)c->spsel << 1) | (a64_fp_enabled(c) ? 4u : 0u) |
                     (c->fpcr == 0 ? 8u : 0u);
    jc_ent *q = &j->jc[jc_hash(pc)];
    if (q->va == pc && q->flags == flags && q->pa == (pa & ~0xfffULL)) { /* ja no cache de saltos */
        c->jit_tlb = &c->tlb[c->el][0];
        j->n_next++;
        return q->code;
    }
    jblock *b = lookup(j, pc, pa, flags);
    if (!b || b->interp)
        return NULL;
    q->va = pc;
    q->flags = flags;
    q->pa = pa & ~0xfffULL;
    q->code = b->code;
    c->jit_tlb = &c->tlb[c->el][0];
    j->n_next++;
    return b->code;
}

/* depois de uma chamada a C: tenta seguir direto para o proximo bloco (x0) */
static void emit_resume(struct a64_jit *j, a64 *a)
{
    a64_mov(a, 1, 0, RC);
    e_call(a, (void *)a64_jit_next);
    a64_ldr(a, 4, 9, RC, OFF(nzcv));
    a64_lsl_imm(a, 9, 9, 28);
    e_msr_nzcv(a, 9);
    uint32_t *f = a64_here(a);
    a64_cbz(a, 1, 0, f);
    a64_ldr(a, 8, RTLB, RC, OFF(jit_tlb));
    a64_br(a, 0);
    a64_patch(f, a64_here(a));
}

static void emit_helper(struct a64_jit *j, a64 *a, uint32_t insn, uint64_t cur, bool end)
{
    a64_mov_imm(a, 9, cur);
    a64_str(a, 8, 9, RC, OFF(cur));
    a64_add_imm(a, 9, 9, 4);
    a64_str(a, 8, 9, RC, OFF(pc));
    e_mrs_nzcv(a, 9);
    a64_lsr_imm(a, 9, 9, 28);
    a64_str(a, 4, 9, RC, OFF(nzcv));
    if ((insn & 0xffdfffe0u) == 0xd51b4420u) /* MRS/MSR FPSR: junta os flags nativos antes */
        e_merge_fpsr(a);
    a64_mov(a, 1, 0, RC);
    a64_movz(a, 0, 1, (uint16_t)insn, 0);
    a64_movk(a, 0, 1, (uint16_t)(insn >> 16), 1);
    bool svc = (insn & 0xffe0001fu) == 0xd4000001u;
    e_call(a, svc ? (void *)a64_svc_helper : (void *)a64_exec_helper);
    a64_ldr(a, 4, 9, RC, OFF(nzcv)); /* a instrucao pode ter mudado NZCV */
    a64_lsl_imm(a, 9, 9, 28);
    e_msr_nzcv(a, 9);
    if (end) { /* segue para o bloco do PC que a instrucao deixou, ou volta ao C */
        if (!j->noresume)
            emit_resume(j, a);
        a64_movz(a, 0, 9, 3, 0);
        a64_str(a, 4, 9, RC, OFF(jit_why));
        a64_str(a, 8, XZR, RC, OFF(jit_exit));
        a64_b(a, j->exit_stub);
    }
}

static int tr_insn(struct a64_jit *j, a64 *a, jblock *b, uint32_t insn, uint64_t cur)
{
    uint32_t *mark = a64_here(a);
    int mark_cold = j->ncold;
    unsigned mark_map = j->nmap;
    int r = tr_insn1(j, a, b, insn, cur);
    if (r != TR_STOP || j->nohelper)
        return r;
    a->p = mark; /* descarta o que a traducao parcial emitiu */
    j->ncold = mark_cold;
    j->nmap = mark_map;
    if ((insn & 0xfffff0ff) == 0xd503207f) /* WFI: o interpretador para a CPU */
        return TR_STOP;
    if (j->stats)
        count_stop(j, insn);
    bool end = helper_ends_block(insn);
    emit_helper(j, a, insn, cur, end);
    return end ? TR_END : TR_OK;
}

static int tr_insn1(struct a64_jit *j, a64 *a, jblock *b, uint32_t insn, uint64_t cur)
{
    if (j->off) {
        unsigned op0 = (insn >> 25) & 0xf, cls;
        if (op0 == 8 || op0 == 9) cls = 1;
        else if (op0 == 5 || op0 == 0xd) cls = 2;
        else if ((insn & 0x3e000000) == 0x28000000) cls = 8;
        else if ((insn & 0x0a000000) == 0x08000000) cls = 4;
        else if ((insn & 0xff000010) == 0x54000000 || (insn & 0x7c000000) == 0x34000000) cls = 16;
        else cls = 32;
        if (j->off & cls)
            return TR_STOP;
    }
    return tr_insn2(j, a, b, insn, cur);
}

static int tr_insn2(struct a64_jit *j, a64 *a, jblock *b, uint32_t insn, uint64_t cur)
{
    unsigned op0 = (insn >> 25) & 0xf;
    unsigned sf = insn >> 31;

    /* ---------------- dados com imediato */
    if (op0 == 0x8 || op0 == 0x9) {
        unsigned grp = (insn >> 23) & 7;
        unsigned rd = insn & 31;
        switch (grp) {
        case 0: case 1: { /* ADR/ADRP */
            uint64_t imm = sext(((insn >> 5) & 0x7ffff) << 2 | ((insn >> 29) & 3), 21);
            uint64_t v = (insn >> 31) ? (cur & ~0xfffULL) + (imm << 12) : cur + imm;
            if (rd != 31) {
                a64_mov_imm(a, T0, v);
                a64_str(a, 8, T0, RC, OFF_X(rd));
            }
            return TR_OK;
        }
        case 2: { /* ADD/SUB imediato */
            bool S = (insn >> 29) & 1;
            dp_verbatim(j, a, insn, S ? CTX_ZR : CTX_SP, CTX_SP, 0, 0, false);
            return TR_OK;
        }
        case 3:
            return TR_STOP; /* com tags (MTE) */
        case 4: { /* logica imediato */
            unsigned n = (insn >> 22) & 1, immr = (insn >> 16) & 63, imms = (insn >> 10) & 63, opc = (insn >> 29) & 3;
            if (!bitmask_ok(sf, n, immr, imms))
                return TR_STOP;
            dp_verbatim(j, a, insn, opc == 3 ? CTX_ZR : CTX_SP, CTX_ZR, 0, 0, false);
            return TR_OK;
        }
        case 5: { /* MOVN/MOVZ/MOVK */
            unsigned opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
            if (opc == 1 || (!sf && hw >= 2))
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, 0, 0, 0, opc == 3);
            return TR_OK;
        }
        case 6: { /* bitfield */
            unsigned opc = (insn >> 29) & 3, n = (insn >> 22) & 1, immr = (insn >> 16) & 63, imms = (insn >> 10) & 63;
            if (opc == 3 || n != sf || (!sf && (immr >= 32 || imms >= 32)))
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, 0, 0, opc == 1);
            return TR_OK;
        }
        default: { /* EXTR */
            unsigned op21 = (insn >> 29) & 3, n = (insn >> 22) & 1, o0 = (insn >> 21) & 1, imms = (insn >> 10) & 63;
            if (op21 || o0 || n != sf || (!sf && imms >= 32))
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, CTX_ZR, 0, false);
            return TR_OK;
        }
        }
    }

    /* ---------------- dados com registradores */
    if (op0 == 0x5 || op0 == 0xd) {
        if ((insn & 0x1f000000) == 0x0a000000) { /* logica deslocada */
            if (!sf && (insn & (1u << 15)))
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, CTX_ZR, 0, false);
            return TR_OK;
        }
        if ((insn & 0x1f200000) == 0x0b000000) { /* add/sub deslocado */
            if (((insn >> 22) & 3) == 3 || (!sf && (insn & (1u << 15))))
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, CTX_ZR, 0, false);
            return TR_OK;
        }
        if ((insn & 0x1f200000) == 0x0b200000) { /* add/sub estendido */
            bool S = (insn >> 29) & 1;
            if (((insn >> 22) & 3) || ((insn >> 10) & 7) > 4)
                return TR_STOP;
            dp_verbatim(j, a, insn, S ? CTX_ZR : CTX_SP, CTX_SP, CTX_ZR, 0, false);
            return TR_OK;
        }
        if ((insn & 0x1fe0fc00) == 0x1a000000) { /* ADC/SBC(S) */
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, CTX_ZR, 0, false);
            return TR_OK;
        }
        if ((insn & 0x1fe00410) == 0x1a400000) { /* CCMN/CCMP: S=1 obrigatorio */
            if (!((insn >> 29) & 1))
                return TR_STOP;
            bool immf = (insn >> 11) & 1;
            dp_verbatim(j, a, insn, 0, CTX_ZR, immf ? 0 : CTX_ZR, 0, false);
            return TR_OK;
        }
        if ((insn & 0x1fe00800) == 0x1a800000) { /* CSEL/CSINC/CSINV/CSNEG */
            if ((insn >> 29) & 1)
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, CTX_ZR, 0, false);
            return TR_OK;
        }
        if ((insn & 0x1f000000) == 0x1b000000) { /* 3 fontes */
            unsigned op54 = (insn >> 29) & 3, op31 = (insn >> 21) & 7, o0 = (insn >> 15) & 1;
            if (op54)
                return TR_STOP;
            bool ok = op31 == 0 || (sf && (op31 == 1 || op31 == 5 || ((op31 == 2 || op31 == 6) && !o0)));
            if (!ok)
                return TR_STOP;
            bool hi = op31 == 2 || op31 == 6;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, CTX_ZR, hi ? 0 : CTX_ZR, false);
            return TR_OK;
        }
        if ((insn & 0x7fe00000) == 0x1ac00000) { /* 2 fontes (S = 0) */
            unsigned opc = (insn >> 10) & 63;
            bool ok = opc == 2 || opc == 3 || (opc >= 8 && opc <= 11);
            if ((opc & 0x38) == 0x10 && j->crc32) { /* CRC32{B,H,W,X}/CRC32C */
                unsigned sz = opc & 3;
                ok = (sz == 3) == (bool)sf;
            }
            if (!ok)
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, CTX_ZR, 0, false);
            return TR_OK;
        }
        if ((insn & 0x7fe00000) == 0x5ac00000) { /* 1 fonte (S = 0) */
            unsigned opc2 = (insn >> 16) & 31, opc = (insn >> 10) & 63;
            if (opc2 || opc > 5 || (opc == 3 && !sf))
                return TR_STOP;
            dp_verbatim(j, a, insn, CTX_ZR, CTX_ZR, 0, 0, false);
            return TR_OK;
        }
        return TR_STOP;
    }

    /* ---------------- loads/stores inteiros */
    if ((insn & 0x0a000000) == 0x08000000) {
        if ((insn & 0x3f800000) == 0x08800000) { /* LDAR/STLR/LDLAR/STLLR */
            /* varias CPUs: barreiras do host em volta (acquire depois da leitura, release antes
             * da escrita e depois dela, para STLR seguido de LDAR nao trocar de ordem) */
            unsigned size = insn >> 30, L = (insn >> 22) & 1, rt = insn & 31;
            a64_mov(a, 1, T2, src(j, a, CTX_SP, (insn >> 5) & 31, T2));
            if (L) {
                emit_load(j, a, size, 1, cur);
                if (j->c->smp)
                    a64_put(a, DMB_ISH);
                dst(j, a, CTX_ZR, rt, T0);
            } else {
                int v = src(j, a, CTX_ZR, rt, T0);
                if (v == XZR) {
                    a64_mov(a, 1, T0, XZR);
                    v = T0;
                }
                if (j->c->smp)
                    a64_put(a, DMB_ISH);
                emit_store(j, a, size, v, cur);
                if (j->c->smp)
                    a64_put(a, DMB_ISH);
            }
            return TR_OK;
        }
        if ((insn & 0x3f000000) == 0x08000000) /* exclusivos */
            return tr_exclusive(j, a, insn, cur);
        if ((insn & 0x3f000000) == 0x18000000) { /* LDR literal */
            unsigned opc = insn >> 30, rt = insn & 31;
            if (opc == 3)
                return TR_OK; /* PRFM */
            uint64_t addr = cur + (sext((insn >> 5) & 0x7ffff, 19) << 2);
            a64_mov_imm(a, T2, addr);
            emit_load(j, a, opc == 1 ? 3 : 2, opc == 2 ? 2 : 1, cur);
            dst(j, a, CTX_ZR, rt, T0);
            return TR_OK;
        }
        if ((insn & 0x3e000000) == 0x28000000) /* par, V=0 */
            return tr_ldst_pair(j, a, insn, cur);
        if ((insn & 0x3e000000) == 0x38000000) /* registrador, V=0 */
            return tr_ldst_reg(j, a, insn, cur);
        if ((j->flags & 4) && !(j->off & 64)) { /* SIMD/FP habilitado (senao o interpretador gera a armadilha); 64 = MVM_JIT_NO */
            if ((insn & 0x3e000000) == 0x3c000000)
                return tr_simd_ldst_reg(j, a, insn, cur);
            if ((insn & 0x3e000000) == 0x2c000000)
                return tr_simd_ldst_pair(j, a, insn, cur);
            if ((insn & 0x3f000000) == 0x1c000000)
                return tr_simd_literal(j, a, insn, cur);
        }
        return TR_STOP;
    }

    /* ---------------- SIMD/FP de dados (FP habilitado e FPCR = 0) */
    if (((insn >> 25) & 7) == 7) {
        if ((j->flags & 12) != 12 || (j->off & 128))
            return TR_STOP;
        return tr_simd(j, a, insn);
    }

    /* ---------------- desvios e sistema */
    if ((insn & 0x7c000000) == 0x14000000) { /* B/BL */
        uint64_t target = cur + (sext(insn & 0x3ffffff, 26) << 2);
        if (insn >> 31) {
            a64_mov_imm(a, T0, cur + 4);
            a64_str(a, 8, T0, RC, OFF_X(30));
        }
        emit_exit(j, a, b, target);
        return TR_END;
    }
    if ((insn & 0xff000010) == 0x54000000) { /* B.cond */
        uint64_t target = cur + (sext((insn >> 5) & 0x7ffff, 19) << 2);
        unsigned cond = insn & 15;
        if (cond >= 14) {
            emit_exit(j, a, b, target);
            return TR_END;
        }
        uint32_t *br = a64_here(a);
        a64_bcond(a, (int)cond, br);
        emit_exit(j, a, b, cur + 4);
        e_patch(br, a64_here(a));
        emit_exit(j, a, b, target);
        return TR_END;
    }
    if ((insn & 0x7e000000) == 0x34000000 || (insn & 0x7e000000) == 0x36000000) { /* CBZ/CBNZ/TBZ/TBNZ */
        bool tb = (insn & 0x7e000000) == 0x36000000;
        unsigned rt = insn & 31, nz = (insn >> 24) & 1;
        uint64_t target = cur + (tb ? sext((insn >> 5) & 0x3fff, 14) << 2 : sext((insn >> 5) & 0x7ffff, 19) << 2);
        int r = src(j, a, CTX_ZR, rt, T0);
        uint32_t *br = a64_here(a);
        if (tb)
            e_tbz(a, (int)nz, r, ((insn >> 31) << 5) | ((insn >> 19) & 31), br);
        else if (nz)
            a64_cbnz(a, (int)sf, r, br);
        else
            a64_cbz(a, (int)sf, r, br);
        emit_exit(j, a, b, cur + 4);
        e_patch(br, a64_here(a));
        emit_exit(j, a, b, target);
        return TR_END;
    }
    {
        uint32_t m = insn & 0xfffffc1f;
        if (m == 0xd61f0000 || m == 0xd63f0000 || m == 0xd65f0000) { /* BR/BLR/RET */
            int r = src(j, a, CTX_ZR, (insn >> 5) & 31, T1);
            if (r == XZR) {
                a64_mov(a, 1, T1, XZR);
                r = T1;
            }
            if (m == 0xd63f0000) {
                a64_mov_imm(a, T0, cur + 4);
                a64_str(a, 8, T0, RC, OFF_X(30));
            }
            emit_exit_dyn(j, a, r);
            return TR_END;
        }
    }
    if ((insn & 0xfffff01f) == 0xd503201f) { /* hints: so WFI nao e NOP */
        unsigned hint = (insn >> 5) & 0x7f;
        return hint == 3 ? TR_STOP : TR_OK;
    }
    if ((insn & 0xfffff0ff) == 0xd503309f || (insn & 0xfffff0ff) == 0xd50330bf) { /* DSB/DMB */
        if (!j->c->smp)
            return TR_OK;
        if ((insn & 0xfffff0ff) == 0xd503309f) { /* DSB: TLBIs compartilhados pendentes -> interpretador */
            a64_ldr(a, 4, 9, RC, OFF(tlbi_left));
            uint32_t *fix = a64_here(a);
            a64_cbnz(a, 0, 9, fix);
            int ci = cold_helper(j, fix, cur, insn);
            j->cold[ci].back = a64_here(a);
        }
        a64_put(a, DMB_ISH);
        return TR_OK;
    }
    if ((insn & 0xffd00000) == 0xd5100000) /* MRS/MSR (registrador), op0 >= 2 */
        return tr_sysreg(j, a, insn);
    if ((insn & 0xfffff0ff) == 0xd50340df && j->el == 1) { /* MSR DAIFSet, #imm */
        a64_ldr(a, 4, 9, RC, OFF(daif));
        a64_movz(a, 0, 10, (uint16_t)((insn >> 8) & 15), 0);
        a64_logic(a, 0, 1, 0, 9, 9, 10);
        a64_str(a, 4, 9, RC, OFF(daif));
        return TR_OK;
    }
    return TR_STOP;
}

static void count_stop(struct a64_jit *j, uint32_t insn)
{
    uint32_t key = insn & 0xffc00c00u;
    for (int i = 0; i < 64; i++) {
        if (j->stop_cnt[i] && j->stop_op[i] == key) {
            j->stop_cnt[i]++;
            return;
        }
        if (!j->stop_cnt[i]) {
            j->stop_op[i] = key;
            j->stop_cnt[i] = 1;
            return;
        }
    }
}

static jblock *translate(struct a64_jit *j, uint64_t va, uint64_t pa, uint32_t flags, const uint8_t *host)
{
    if ((size_t)(j->r_end - j->pos) < 16384 && j->r_end == j->stubs) { /* primeira metade cheia */
        j->pos = j->r2_start;
        j->r_end = j->code_end;
    }
    if (j->nblocks >= MAX_BLOCKS ||
        (size_t)(j->r_end - j->pos) < 16384)
        jit_flush(j);
    a64_cpu *c = j->c;
    jblock *b = &j->blocks[j->nblocks++];
    memset(b, 0, sizeof(*b));
    b->va = va;
    b->pa = pa;
    b->flags = flags;
    /* cabecalho: VA do bloco (o stub de bloco invalidado le de code - 8) */
    j->pos[0] = (uint32_t)va;
    j->pos[1] = (uint32_t)(va >> 32);
    j->pos += 2;
    b->code = j->pos;
    j->flags = flags;
    j->el = c->el;
    j->sp_idx = (c->el && c->spsel) ? 1 : 0;
    j->ncold = 0;
    j->nmap = 0;
    j->blk_code = b->code;
    j->n_tr++;
    a64 A = {j->pos, j->r_end, false}, *a = &A;

    /* prologo: sai antes de executar se a CPU deve sair ou o orcamento ja acabou (o bloco pode
     * passar um pouco do fim da fatia: melhor que interpretar instrucao a instrucao ate la) */
    a64_ldr(a, 4, 16, REXIT, 0);
    uint32_t *f1 = a64_here(a);
    a64_cbnz(a, 0, 16, f1);
    uint32_t *f2 = a64_here(a);
    e_tbz(a, 1, RBUD, 63, f2);
    uint32_t *sub = a64_here(a);
    a64_sub_imm(a, RBUD, RBUD, 0);

    unsigned n = 0;
    uint64_t pc = va;
    int r = TR_OK;
    while (n < j->maxn && j->ncold < MAX_COLD - 4) {
        uint32_t insn = (uint32_t)ld_le(host + (pc & 0xfff), 4);
        j->cur_idx = n;
        unsigned mark_map = j->nmap;
        uint32_t *mark = a64_here(a);
        int mark_cold = j->ncold;
        r = tr_insn(j, a, b, insn, pc);
        if (r == TR_STOP) {
            a->p = mark; /* descarta o que tiver sido emitido */
            j->ncold = mark_cold;
            j->nmap = mark_map;
            break;
        }
        n++;
        pc += 4;
        if (r == TR_END)
            break;
        if (!(pc & 0xfff)) /* fim da pagina */
            break;
    }
    if (n == 0) {
        b->interp = true;
        b->valid = true;
        a->p = b->code;
        j->ncold = 0;
        mark_code_page(j, pa);
        return b;
    }
    if (r != TR_END)
        emit_exit(j, a, b, pc);
    /* saida do prologo: devolve o orcamento e sai com PC = inicio do bloco */
    e_patch(f1, a64_here(a));
    e_patch(f2, a64_here(a));
    a64_movz(a, 0, 9, 4, 0);
    a64_str(a, 4, 9, RC, OFF(jit_why));
    a64_mov_imm(a, 9, va);
    a64_str(a, 8, 9, RC, OFF(pc));
    a64_str(a, 8, XZR, RC, OFF(jit_exit));
    a64_b(a, j->exit_stub);
    emit_cold(j, a);
    if (j->nmap) { /* tabela para o tratador de SIGILL */
        b->map = a64_here(a);
        b->nmap = (uint16_t)j->nmap;
        for (unsigned i = 0; i < j->nmap; i++)
            a64_put(a, j->map[i]);
    }
    *sub = 0xD1000000u | (n << 10) | (RBUD << 5) | RBUD; /* sub x29, x29, #n */
    if (a->overflow) { /* nao deveria acontecer (16 KiB de folga) */
        jit_flush(j);
        return NULL;
    }
    b->ninsn = n;
    b->valid = true;
    j->pos = a->p;
    __builtin___clear_cache((char *)b->code, (char *)j->pos);
    mark_code_page(j, pa);
    return b;
}

/* ------------------------------------------------------------ execucao */

static void init_pages(struct a64_jit *j);

int64_t a64_jit_run(struct a64_jit *j, int64_t budget)
{
    a64_cpu *c = j->c;
    tls_jit = j;
    if (unlikely(!j->pages))
        init_pages(j);
    if (atomic_load_explicit(&j->pending, memory_order_relaxed))
        process_pending(j);
    uint64_t pc = c->pc;
    c->cur = pc; /* falta de busca: ELR = pc */
    a64_tlbe *e = a64_tlb_exec(c, pc);
    if (e->tag_x & TLB_IO)
        return 0;
    uint64_t pa = e->pa | (pc & 0xfff);
    uint32_t flags = (uint32_t)c->el | ((uint32_t)c->spsel << 1) | (a64_fp_enabled(c) ? 4u : 0u) |
                     (c->fpcr == 0 ? 8u : 0u);
    if (pa >> 12 >= j->npages)
        return 0;
    uint64_t flushes = j->n_flush;
    jblock *b = lookup(j, pc, pa, flags);
    if (!b) {
        int64_t t0 = j->stats ? host_clock_ns() : 0;
        b = translate(j, pc, pa, flags, (const uint8_t *)(e->addend + (pc & ~0xfffULL)));
        if (j->stats) {
            j->tr_ns += (uint64_t)(host_clock_ns() - t0);
            if (b)
                j->n_tr_insns += b->ninsn;
        }
        if (!b)
            return 0;
        unsigned h = hash_of(pc, pa);
        b->hnext = j->hash[h];
        j->hash[h] = b;
        unsigned pg = (unsigned)(pa >> 12);
        b->pnext = j->pages[pg];
        j->pages[pg] = b;
    }
    if (b->interp || budget <= 0)
        return 0;
    /* encadeia a saida anterior (mesma pagina fisica e flags) */
    void *x = c->jit_exit;
    if (x && j->n_flush == flushes) {
        jblock *xb = rec_blk(x);
        uint32_t *pt = rec_patch(x);
        int64_t dist = b->code - pt; /* alcance do B: +-2^25 palavras */
        if (xb->valid && rec_target(x) == pc && xb->flags == flags && (xb->pa >> 12) == (pa >> 12) &&
            dist < (1 << 25) && dist > -(1 << 25)) {
            a64_patch_b(pt, b->code);
            __builtin___clear_cache((char *)pt, (char *)(pt + 1));
            j->n_chain++;
        }
    }
    c->jit_exit = NULL;
    c->jit_budget = budget;
    c->jit_why = 0;
    { /* registra no cache de saltos */
        jc_ent *q = &j->jc[jc_hash(pc)];
        if (q->va != ~0ULL && (q->pa >> 12) < j->npages && j->jc_cnt[q->pa >> 12])
            j->jc_cnt[q->pa >> 12]--;
        q->va = pc;
        q->flags = flags;
        q->pa = pa & ~0xfffULL;
        q->code = b->code;
        if (j->jc_cnt[pa >> 12] < 0xffff)
            j->jc_cnt[pa >> 12]++;
    }
    j->n_enter++;
    j->enter(c, b->code, &c->tlb[c->el][0], c->exit_flag, j->jc);
    int64_t done = budget - c->jit_budget;
    if (j->stats) {
        j->n_insns += (uint64_t)done;
        j->why[c->jit_why & 7]++;
    }
    return done;
}

/* stubs de entrada/saida */
static void build_stubs(struct a64_jit *j)
{
    j->stubs = j->code + (CODE_SIZE / 4) / 2;
    a64 A = {j->stubs, j->stubs + 4096, false}, *a = &A;
    j->enter = (void (*)(a64_cpu *, uint32_t *, a64_tlbe *, _Atomic int *, jc_ent *))(void *)a64_here(a);
    a64_stp_pre(a, 29, 30, XSP, -96);
    a64_stp(a, 19, 20, XSP, 16);
    a64_stp(a, 21, 22, XSP, 32);
    a64_stp(a, 23, 24, XSP, 48);
    a64_stp(a, 25, 26, XSP, 64);
    a64_stp(a, 27, 28, XSP, 80);
    a64_mov(a, 1, RC, 0);
    a64_mov(a, 1, RTLB, 2);
    a64_mov(a, 1, REXIT, 3);
    a64_mov(a, 1, RJC, 4);
    a64_ldr(a, 8, RBUD, RC, OFF(jit_budget));
    a64_ldr(a, 4, 9, RC, OFF(nzcv));
    a64_lsl_imm(a, 9, 9, 28);
    e_msr_nzcv(a, 9);
    e_msr_fpsr(a, XZR);
    a64_br(a, 1);
    /* saida comum: grava NZCV e o orcamento */
    j->exit_stub = a64_here(a);
    e_mrs_nzcv(a, 9);
    a64_lsr_imm(a, 9, 9, 28);
    a64_str(a, 4, 9, RC, OFF(nzcv));
    e_merge_fpsr(a);
    a64_str(a, 8, RBUD, RC, OFF(jit_budget));
    a64_ldp(a, 19, 20, XSP, 16);
    a64_ldp(a, 21, 22, XSP, 32);
    a64_ldp(a, 23, 24, XSP, 48);
    a64_ldp(a, 25, 26, XSP, 64);
    a64_ldp(a, 27, 28, XSP, 80);
    a64_ldp_post(a, 29, 30, XSP, 96);
    a64_ret(a);
    /* saida encadeavel: BL aqui + ponteiro do registro (x30 aponta para ele) */
    j->stub_exit_rec = a64_here(a);
    a64_movz(a, 0, 9, 1, 0);
    a64_str(a, 4, 9, RC, OFF(jit_why));
    a64_str(a, 8, 30, RC, OFF(jit_exit));
    a64_ldr(a, 8, 10, 30, 0); /* LDR com imediato escalado exige alinhamento so do deslocamento */
    a64_str(a, 8, 10, RC, OFF(pc));
    a64_b(a, j->exit_stub);
    /* busca no cache de saltos: x10 = VA, w11 = flags; acerto vai direto ao bloco */
    j->stub_lookup = a64_here(a);
    a64_put(a, 0xCA400000u | (18u << 10) | (10u << 16) | (10u << 5) | 12u); /* eor x12, x10, x10, lsr #18 */
    a64_ubfx(a, 12, 12, 2, JC_BITS);
    a64_addsub_reg(a, 1, 0, 0, 12, RJC, 12, 5); /* x12 = jc + idx*32 */
    a64_ldp(a, 13, 14, 12, 0);
    a64_logic(a, 1, 2, 0, 13, 13, 10);
    uint32_t *m1 = a64_here(a);
    a64_cbnz(a, 1, 13, m1);
    a64_logic(a, 0, 2, 0, 14, 14, 11);
    uint32_t *m2 = a64_here(a);
    a64_cbnz(a, 0, 14, m2);
    a64_ubfx(a, 16, 10, 12, A64_TLB_BITS);
    a64_addsub_reg(a, 1, 0, 0, 16, 16, 16, 2);
    a64_addsub_reg(a, 1, 0, 0, 16, RTLB, 16, 3);
    a64_ldr(a, 8, 17, 16, (uint32_t)offsetof(a64_tlbe, tag_x));
    a64_logic_imm(a, 1, 0, 9, 10, A64_MASK_PAGE_HI);
    a64_logic(a, 1, 2, 0, 9, 9, 17);
    uint32_t *m3 = a64_here(a);
    a64_cbnz(a, 1, 9, m3);
    a64_ldr(a, 8, 17, 16, (uint32_t)offsetof(a64_tlbe, pa));
    a64_ldp(a, 13, 15, 12, 16);
    a64_logic(a, 1, 2, 0, 17, 17, 13);
    uint32_t *m4 = a64_here(a);
    a64_cbnz(a, 1, 17, m4);
    a64_br(a, 15);
    uint32_t *mm[4] = {m1, m2, m3, m4};
    uint32_t *done[4];
    for (int k = 0; k < 4; k++) { /* motivo da falta: 2 VA, 5 flags, 6 TLB, 7 PA */
        a64_patch(mm[k], a64_here(a));
        a64_movz(a, 0, 9, (uint16_t)(k == 0 ? 2 : 4 + k), 0);
        done[k] = a64_here(a);
        a64_b(a, done[k]);
    }
    for (int k = 0; k < 4; k++)
        a64_patch(done[k], a64_here(a));
    /* antes de sair: o bloco pode existir (falta de TLB/colisao no cache): pergunta ao C */
    a64_str(a, 4, 9, RC, OFF(jit_why));
    a64_str(a, 8, 10, RC, OFF(pc));
    if (!j->noresume) {
        e_mrs_nzcv(a, 9);
        a64_lsr_imm(a, 9, 9, 28);
        a64_str(a, 4, 9, RC, OFF(nzcv));
        emit_resume(j, a);
    }
    a64_str(a, 8, XZR, RC, OFF(jit_exit)); /* c->pc ja gravado (x10 pode ter sido usado pelo C) */
    a64_b(a, j->exit_stub);
    for (unsigned sz = 0; sz < 4; sz++) {
        for (unsigned opc = 1; opc < 4; opc++)
            j->st_ld[sz][opc] = build_slow_stub(j, a, COLD_LD, sz, opc);
        j->st_st[sz] = build_slow_stub(j, a, COLD_ST, sz, 0);
    }
    for (unsigned k = 0; k < 2; k++) {
        j->st_ldp[k][0] = build_slow_stub(j, a, COLD_LDP, 2 + k, 0);
        j->st_ldp[k][1] = build_slow_stub(j, a, COLD_LDP, 2 + k, 1);
        j->st_stp[k] = build_slow_stub(j, a, COLD_STP, 2 + k, 0);
    }
    /* bloco invalidado: code[0] virou BL para ca; o VA do bloco esta em code - 8 = x30 - 12 */
    j->stub_dead = a64_here(a);
    a64_put(a, 0xF8400000u | ((uint32_t)(-12 & 0x1ff) << 12) | (30u << 5) | 10u); /* ldur x10, [x30, #-12] */
    a64_str(a, 8, 10, RC, OFF(pc));
    a64_str(a, 8, XZR, RC, OFF(jit_exit));
    a64_movz(a, 0, 9, 0, 0);
    a64_str(a, 4, 9, RC, OFF(jit_why));
    a64_b(a, j->exit_stub);
    __builtin___clear_cache((char *)j->stubs, (char *)a->p);
    j->r2_start = a->p;
    j->code_start = j->pos = j->code;
    j->r_end = j->stubs;
}

struct a64_jit *a64_jit_new(a64_cpu *c)
{
    const char *e = getenv("MVM_JIT");
    if (e && *e == '0')
        return NULL;
    void *mem = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        LOGW("JIT ARM64: memoria executavel indisponivel; usando o interpretador");
        return NULL;
    }
    struct a64_jit *j = calloc(1, sizeof(*j));
    j->c = c;
    j->mem = c->mem;
    j->code = mem;
    j->code_end = (uint32_t *)((uint8_t *)mem + CODE_SIZE);
    j->blocks = calloc(MAX_BLOCKS, sizeof(jblock));
    j->jc = calloc(JC_SIZE, sizeof(jc_ent));
    j->hash = calloc(HASH_SIZE, sizeof(jblock *));
    for (unsigned i = 0; i < JC_SIZE; i++)
        j->jc[i].va = ~0ULL;
    j->stats = getenv("MVM_JIT_STATS") != NULL;
    j->maxn = getenv("MVM_JIT_MAXN") ? (unsigned)atoi(getenv("MVM_JIT_MAXN")) : BLOCK_MAX_INSNS;
    if (j->maxn < 1 || j->maxn > BLOCK_MAX_INSNS)
        j->maxn = BLOCK_MAX_INSNS;
    j->nohelper = getenv("MVM_JIT_NOHELPER") != NULL;
    j->noresume = getenv("MVM_JIT_NORESUME") != NULL;
    j->off = getenv("MVM_JIT_NO") ? (uint32_t)strtoul(getenv("MVM_JIT_NO"), NULL, 0) : 0;
#if defined(__linux__)
    j->crc32 = (getauxval(AT_HWCAP) & (1UL << 7)) != 0; /* HWCAP_CRC32 */
#endif
    build_stubs(j);
    static bool ill_installed;
    if (!ill_installed) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = sigill_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGILL, &sa, &old_ill);
        ill_installed = true;
    }
    return j;
}

/* na primeira execucao (a RAM ja esta mapeada): paginas fisicas cobertas ate o fim da RAM */
static void init_pages(struct a64_jit *j)
{
    mvm_space *m = j->mem;
    uint64_t top = 0;
    for (int i = 0; i < m->n; i++)
        if (m->r[i].host && m->r[i].base + m->r[i].size > top)
            top = m->r[i].base + m->r[i].size;
    j->npages = (top + 0xfff) >> 12;
    j->pages = calloc(j->npages ? j->npages : 1, sizeof(jblock *));
    j->bits = calloc((j->npages + 7) / 8 + 1, 1);
    j->pend = calloc((j->npages + 63) / 64 + 1, sizeof(uint64_t));
    j->jc_cnt = calloc(j->npages ? j->npages : 1, sizeof(uint16_t));
    pthread_mutex_lock(&group_lock);
    struct a64_jit_group *g = m->code_hook == code_hook ? m->code_opaque : NULL;
    if (!g) {
        g = calloc(1, sizeof(*g));
        g->npages = j->npages;
        g->bits = calloc((j->npages + 7) / 8 + 1, 1);
        m->code_bits = g->bits;
        m->code_limit = j->npages << 12;
        m->code_opaque = g;
        m->code_hook = code_hook;
    }
    int k = atomic_load(&g->n);
    g->jits[k] = j;
    atomic_store(&g->n, k + 1);
    j->group = g;
    pthread_mutex_unlock(&group_lock);
    if (k)
        return;
    LOGI("JIT ARM64 ligado (%llu MiB de enderecos fisicos cobertos)", (unsigned long long)(top >> 20));
}

void a64_jit_free(struct a64_jit *j)
{
    if (!j)
        return;
    if (j->stats) {
        LOGI("JIT ARM64: traducao %.2f s, %.1f instrucoes por bloco, %.1f MiB de codigo",
             (double)j->tr_ns / 1e9, j->n_tr ? (double)j->n_tr_insns / (double)j->n_tr : 0.0,
             (double)((uint8_t *)j->pos - (uint8_t *)j->code) / 1048576.0);
        LOGI("JIT ARM64: saidas: encadeavel %llu, cache de saltos (VA %llu, flags %llu, TLB %llu, PA %llu), instrucao de controle %llu, prologo %llu, outras %llu",
             (unsigned long long)j->why[1], (unsigned long long)j->why[2], (unsigned long long)j->why[5],
             (unsigned long long)j->why[6], (unsigned long long)j->why[7], (unsigned long long)j->why[3],
             (unsigned long long)j->why[4], (unsigned long long)j->why[0]);
        LOGI("JIT ARM64: %llu continuacoes sem sair para o C", (unsigned long long)j->n_next);
        LOGI("JIT ARM64: %llu blocos traduzidos, %llu entradas, %llu encadeamentos, %llu invalidacoes, %llu esvaziamentos, %llu instrucoes no JIT",
             (unsigned long long)j->n_tr, (unsigned long long)j->n_enter, (unsigned long long)j->n_chain,
             (unsigned long long)j->n_inval, (unsigned long long)j->n_flush, (unsigned long long)j->n_insns);
        for (int k = 0; k < 25; k++) {
            int best = -1;
            for (int i = 0; i < 64; i++)
                if (j->stop_cnt[i] && (best < 0 || j->stop_cnt[i] > j->stop_cnt[best]))
                    best = i;
            if (best < 0)
                break;
            LOGI("  para em %08x: %llu", j->stop_op[best], (unsigned long long)j->stop_cnt[best]);
            j->stop_cnt[best] = 0;
        }
    }
    struct a64_jit_group *g = j->group;
    if (g) {
        pthread_mutex_lock(&group_lock);
        int n = atomic_load(&g->n), k;
        for (k = 0; k < n && g->jits[k] != j; k++)
            ;
        if (k < n) {
            memmove(&g->jits[k], &g->jits[k + 1], (size_t)(n - k - 1) * sizeof(g->jits[0]));
            atomic_store(&g->n, n - 1);
        }
        if (n - 1 <= 0) {
            if (j->mem->code_opaque == g) {
                j->mem->code_bits = NULL;
                j->mem->code_hook = NULL;
                j->mem->code_opaque = NULL;
            }
            free(g->bits);
            free(g);
        }
        pthread_mutex_unlock(&group_lock);
    }
    munmap(j->code, CODE_SIZE);
    free(j->blocks);
    free(j->pages);
    free(j->bits);
    free(j->pend);
    free(j->jc);
    free(j->hash);
    free(j->jc_cnt);
    free(j);
}

void a64_jit_reset(struct a64_jit *j)
{
    if (j && j->pages)
        jit_flush(j);
}

#else /* host sem aarch64: so o interpretador */

struct a64_jit *a64_jit_new(a64_cpu *c)
{
    (void)c;
    return NULL;
}
void a64_jit_free(struct a64_jit *j) { (void)j; }
int64_t a64_jit_run(struct a64_jit *j, int64_t budget)
{
    (void)j;
    (void)budget;
    return 0;
}
void a64_jit_reset(struct a64_jit *j) { (void)j; }

#endif
