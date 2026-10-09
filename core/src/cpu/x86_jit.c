/*
 * JIT x86 -> AArch64.
 *
 * Traduz blocos basicos de codigo x86 (modo protegido de 32 bits e modo de
 * 64 bits) para codigo nativo AArch64. Instrucoes nao suportadas terminam o
 * bloco e sao executadas pelo interpretador, que continua sendo a referencia.
 *
 * Convencoes do codigo gerado:
 *   x19 = x86_cpu *, x25 = &c->tlb[user][0], x29 = orcamento (c->jit_budget);
 *   x20-x24 temporarios preservados
 *   entre chamadas a funcoes auxiliares; x0-x16 temporarios.
 *   Os registradores do convidado ficam em c->r[] (memoria), os flags no mesmo
 *   formato preguicoso do interpretador (cc_op/cc_size/cc_dst/cc_src1/cc_src2),
 *   e antes de cada instrucao que pode gerar excecao grava-se c->cur_rip: assim
 *   x86_exception() (longjmp) funciona exatamente como no interpretador.
 *
 * Codigo automodificavel: paginas com codigo traduzido ficam marcadas em
 * mem->code_bits; o TLB manda as escritas nelas pelo caminho lento e
 * space_write/space_ram_ptr chamam code_hook, que invalida os blocos da pagina se a
 * escrita acertar um trecho de 64 bytes com codigo traduzido (j->chunks): o kernel do
 * XP mistura codigo e dados na mesma pagina, e invalidar a cada escrita de dado fazia o
 * JIT retraduzir sem parar.
 * Blocos so se encadeiam dentro da mesma pagina fisica e do mesmo modo.
 */
#include "x86_jit.h"

#include "x86_priv.h"

#include <stddef.h>
#include <stdlib.h>

#if defined(__aarch64__)

#include <sys/mman.h>

#include "a64_asm.h"

#define CODE_SIZE (120u << 20) /* desvios B alcancam +-128 MB (blocos -> stub de saida) */
#define MAX_BLOCKS 262144u
#define HASH_BITS 18
#define BLOCK_MAX_INSNS 128

#define RC 19
#define REA 20
#define RA 21
#define RB 22
#define RR 23
#define RT 24
#define RTLB 25
#define RJC 26 /* &j->jc[0] */
#define RBUD 29 /* orcamento de instrucoes (c->jit_budget fica em memoria so nas chamadas e saidas) */
#define JC_SIZE 16384 /* entradas do cache de saltos (potencia de 2; mascara em emit_exit_lookup) */

typedef struct jit_block jit_block;

typedef struct {
    jit_block *blk;
    uint32_t *patch; /* instrucao B que vai para a sequencia de saida (corrigida no encadeamento) */
    uint64_t target;
} jit_exit_rec;

struct jit_block {
    uint64_t lin, phys, rip, csbase;
    uint32_t mode, ninsn;
    uint32_t *code;
    bool valid, interp;
    jit_block *hnext, *pnext;
    jit_exit_rec exit[2];
};

struct x86_jit {
    x86_cpu *c;
    mvm_space *mem;
    uint32_t *code, *code_end, *pos;
    uint32_t *exit_stub;
    void (*enter)(x86_cpu *c, uint32_t *code, void *tlb, void *jc);
    jit_block *blocks;
    unsigned nblocks;
    jit_block *hash[1u << HASH_BITS];
    jit_block **pages;
    uint64_t npages;
    uint8_t *bits;
    uint64_t *chunks; /* por pagina: trechos de 64 bytes com codigo traduzido */
    uint64_t n_trans, n_inval, n_flush, n_chain, n_enter, exit_why[8], ret_interp, ret_budget, ret_nomode, ret_nopage;
    uint64_t nochain[8]; /* estatistica: por que uma saida encadeavel nao foi encadeada */
    unsigned max_insns;     /* MVM_JIT_MAXINSN (depuracao) */
    uint8_t skip[0x200];    /* MVM_JIT_SKIP=op,op,... opcodes deixados ao interpretador */
    bool stats;             /* MVM_JIT_STATS=1 */
    unsigned n_interp_log;
#define COLD_MAX 32768
#define COLD_FIX 1024
    uint32_t cold[COLD_MAX];
    struct { uint32_t *site; unsigned coff; } m2c[COLD_FIX]; /* desvio do bloco -> caminho lento */
    struct { unsigned coff; uint32_t *target; } c2m[COLD_FIX]; /* volta do caminho lento -> bloco */
    unsigned off;           /* MVM_JIT_OFF: bits que desligam otimizacoes (depuracao) */
    /* cache de saltos: RIP -> codigo do bloco (desvios indiretos e entre paginas) */
    /* RIP -> codigo; phys = pagina fisica do bloco, conferida contra o TLB de execucao
     * na consulta, de modo que trocas de CR3/INVLPG nao exigem esvaziar o cache */
    struct { uint64_t key; uint32_t *code; uint64_t phys, pad; } jc[JC_SIZE];
    bool jc_used;   /* ha entradas validas no cache de saltos */
    uint16_t *jc_cnt; /* entradas do cache de saltos por pagina fisica (< npages) */
    uint64_t site_gen;          /* geracao dos caches por ponto de salto (copia em jc[0].pad) */
    uint64_t *pending_site;     /* slot a preencher com o proximo bloco executado */
    uint64_t pending_flush;
    uint64_t n_jit_insns, n_interp, n_interp_nojit, interp_op[0x200];
    uint64_t icall_op[0x400]; /* execucoes de icall por opcode (estatistica) */
};

/* ------------------------------------------------------------ decodificacao */

enum { IMM_NONE, IMM8, IMM16, IMMZ, IMMV, REL8, RELZ, MOFFS };

typedef struct {
    uint64_t rip, next;
    int len;
    int op;                 /* 0x0Fxx para 2 bytes */
    int osz, asz;
    uint8_t rex;
    int seg;                /* -1 = padrao */
    bool rep, repne, p66;
    bool modrm, mem;
    int mod, reg, rm;       /* reg/rm ja com REX */
    int base, index, scale; /* -1 = ausente */
    int64_t disp;
    bool riprel;
    int ea_seg;
    int64_t imm;
    bool icall;             /* executada pelo interpretador dentro do bloco */
} jinsn;

/*
 * Instrucoes executadas chamando o interpretador de dentro do bloco (nao mudam
 * modo, CS, IF nem tabelas do sistema): string, BT*, MUL/DIV, CMPXCHG, SSE, x87...
 * Retorna 1 = com ModRM, 2 = com ModRM e imm8, 3 = sem ModRM, 0 = nao.
 */
static int icall_kind(int op, int reg)
{
    if (op < 0x100) {
        if ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf))
            return 3;
        switch (op) {
        case 0x9c: case 0x9e: case 0x9f: case 0xf5: case 0xf8: case 0xf9: case 0xd7:
            return 3;
        default:
            break;
        }
        if (op >= 0xd8 && op <= 0xdf) /* x87 */
            return 1;
        return 0;
    }
    int o = op & 0xff;
    if (op >= 0x200) /* 0F 38 xx (1) / 0F 3A xx (2) */
        return op >= 0x300 ? 2 : 1;
    switch (o) {
    case 0x31: case 0xa2: return 3; /* RDTSC, CPUID */
    case 0xa3: case 0xab: case 0xb3: case 0xbb: case 0xbc: case 0xbd: case 0xa5: case 0xad:
    case 0xb0: case 0xb1: case 0xc0: case 0xc1: case 0xb8: case 0xae:
        return 1;
    case 0xba: case 0xa4: case 0xac: return 2;
    case 0xc7: return ((reg & 7) == 1 || (reg & 7) == 6 || (reg & 7) == 7) ? 1 : 0; /* CMPXCHG8B/16B, RDRAND, RDSEED */
    default: break;
    }
    /* SSE/MMX */
    if ((o >= 0x10 && o <= 0x17) || (o >= 0x28 && o <= 0x2f) || (o >= 0x50 && o <= 0x6f) || (o >= 0x74 && o <= 0x7f) ||
        (o >= 0xd0 && o <= 0xff) || o == 0xc3)
        return 1;
    if ((o >= 0x70 && o <= 0x73) || (o >= 0xc2 && o <= 0xc6))
        return o == 0xc3 ? 1 : 2;
    return 0;
}

static bool has_modrm(int op)
{
    if (op < 0x100) {
        if (op < 0x40)
            return (op & 7) < 4;
        switch (op) {
        case 0x63: case 0x69: case 0x6b: case 0x80: case 0x81: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8d: case 0x8f: case 0xc0: case 0xc1: case 0xc6: case 0xc7:
        case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xf6: case 0xf7: case 0xfe: case 0xff:
            return true;
        default:
            return false;
        }
    }
    int o = op & 0xff;
    return (o >= 0x40 && o <= 0x4f) || (o >= 0x90 && o <= 0x9f) || o == 0xaf || o == 0xb6 || o == 0xb7 || o == 0xbe ||
           o == 0xbf || (o >= 0x18 && o <= 0x1f) || o == 0x0d;
}

/* tipo de imediato; -1 = opcode nao suportado pelo JIT */
static int imm_kind(int op, int reg)
{
    if (op < 0x40) {
        switch (op & 7) {
        case 0: case 1: case 2: case 3: return IMM_NONE;
        case 4: return IMM8;
        case 5: return IMMZ;
        default: return -1;
        }
    }
    if (op >= 0x40 && op <= 0x5f) return IMM_NONE;
    if (op >= 0x70 && op <= 0x7f) return REL8;
    if (op >= 0x90 && op <= 0x99) return IMM_NONE;
    if (op >= 0xb0 && op <= 0xb7) return IMM8;
    if (op >= 0xb8 && op <= 0xbf) return IMMV;
    switch (op) {
    case 0x63: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8d:
    case 0xc3: case 0xc9: case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xfc: case 0xfd:
        return IMM_NONE;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: return MOFFS;
    case 0x68: case 0x69: case 0x81: case 0xa9: case 0xc7: return IMMZ;
    case 0x6a: case 0x6b: case 0x80: case 0x83: case 0xa8: case 0xc0: case 0xc1: case 0xc6: return IMM8;
    case 0xc2: return IMM16;
    case 0xe8: case 0xe9: return RELZ;
    case 0xeb: return REL8;
    case 0xf6: return reg == 0 ? IMM8 : reg == 1 ? -1 : IMM_NONE;
    case 0xf7: return reg == 0 ? IMMZ : reg == 1 ? -1 : IMM_NONE;
    case 0xfe: return reg <= 1 ? IMM_NONE : -1;
    case 0xff: return (reg <= 2 || reg == 4 || reg == 6) ? IMM_NONE : -1;
    case 0x8f: return reg == 0 ? IMM_NONE : -1;
    default: break;
    }
    if (op >= 0x100) {
        int o = op & 0xff;
        if ((o >= 0x40 && o <= 0x4f) || (o >= 0x90 && o <= 0x9f) || o == 0xaf || o == 0xb6 || o == 0xb7 || o == 0xbe ||
            o == 0xbf || (o >= 0xc8 && o <= 0xcf) || (o >= 0x18 && o <= 0x1f) || o == 0x0d)
            return IMM_NONE;
        if (o >= 0x80 && o <= 0x8f)
            return RELZ;
    }
    return -1;
}

static bool decode(x86_cpu *c, const uint8_t *p, int max, uint64_t rip, jinsn *in)
{
    memset(in, 0, sizeof(*in));
    in->seg = -1;
    in->base = in->index = -1;
    in->rip = rip;
    int i = 0;
    bool p67 = false;
    for (;;) {
        if (i >= max || i >= 15)
            return false;
        uint8_t b = p[i];
        switch (b) {
        case 0x66: in->p66 = true; in->rex = 0; i++; continue;
        case 0x67: p67 = true; in->rex = 0; i++; continue;
        case 0x26: in->seg = S_ES; in->rex = 0; i++; continue;
        case 0x2e: in->seg = S_CS; in->rex = 0; i++; continue;
        case 0x36: in->seg = S_SS; in->rex = 0; i++; continue;
        case 0x3e: in->seg = S_DS; in->rex = 0; i++; continue;
        case 0x64: in->seg = S_FS; in->rex = 0; i++; continue;
        case 0x65: in->seg = S_GS; in->rex = 0; i++; continue;
        case 0xf0: in->rex = 0; i++; continue; /* LOCK: uma CPU so, nada a fazer */
        case 0xf2: in->repne = true; in->rep = false; in->rex = 0; i++; continue;
        case 0xf3: in->rep = true; in->repne = false; in->rex = 0; i++; continue;
        default: break;
        }
        if (c->code64 && (b & 0xf0) == 0x40) {
            in->rex = b;
            i++;
            continue;
        }
        break;
    }
    if (c->code64) {
        in->osz = (in->rex & 8) ? 8 : in->p66 ? 2 : 4;
        in->asz = p67 ? 4 : 8;
        if (in->seg >= 0 && in->seg != S_FS && in->seg != S_GS)
            in->seg = -1;
    } else {
        in->osz = in->p66 ? 2 : 4;
        in->asz = p67 ? 2 : 4;
    }
    if (in->asz == 2)
        return false;
    if (i >= max)
        return false;
    int op = p[i++];
    if (op == 0x0f) {
        if (i >= max)
            return false;
        op = 0x100 | p[i++];
        if (op == 0x138 || op == 0x13a) { /* 0F 38 xx / 0F 3A xx */
            if (i >= max)
                return false;
            op = (op == 0x138 ? 0x200 : 0x300) | p[i++];
        }
    }
    if (c->code64 && op == 0x63 && !(in->rex & 8))
        return false;
    in->op = op;
    /* peek no reg do ModRM (para grupos) */
    int peek_reg = i < max ? (p[i] >> 3) & 7 : 0;
    int ik = icall_kind(op, peek_reg);
    if (!ik && i < max) {
        uint8_t m = p[i];
        if ((op == 0x120 || op == 0x122) && (in->rex & 4) && ((m >> 3) & 7) == 0 && (m >> 6) == 3)
            ik = 1; /* MOV de/para CR8 (IRQL do Windows x64): dentro do bloco */
        else if (op == 0x101 && m == 0xf9)
            ik = 1; /* RDTSCP */
    }
    if (!ik && (op == 0xfa || op == 0xfb))
        ik = 3; /* CLI/STI: interrupcoes so sao entregues entre blocos de qualquer forma */
    /* REP/REPNE: PAUSE (F3 90), "rep ret" (F3 C3), ENDBR (F3 0F 1E) e instrucoes interpretadas */
    if ((in->rep || in->repne) && !ik && !((op == 0x90 || op == 0xc3 || op == 0x11e) && in->rep))
        return false;
    if (ik)
        in->icall = true;
    if (ik == 3) {
        in->len = i;
        in->next = rip + (uint64_t)i;
        if (!c->code64)
            in->next = (uint32_t)in->next;
        return true;
    }
    if (ik || has_modrm(op)) {
        if (i >= max)
            return false;
        uint8_t m = p[i++];
        in->modrm = true;
        in->mod = m >> 6;
        in->reg = ((m >> 3) & 7) | ((in->rex & 4) << 1);
        int rm = m & 7;
        if (in->mod == 3) {
            in->rm = rm | ((in->rex & 1) << 3);
        } else {
            in->mem = true;
            int def = S_DS;
            if (rm == 4) {
                if (i >= max)
                    return false;
                uint8_t sib = p[i++];
                int scale = sib >> 6, index = ((sib >> 3) & 7) | ((in->rex & 2) << 2), base = (sib & 7) | ((in->rex & 1) << 3);
                if (index != 4) {
                    in->index = index;
                    in->scale = scale;
                }
                if ((base & 7) == 5 && in->mod == 0) {
                    if (i + 4 > max)
                        return false;
                    in->disp = (int32_t)(p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24));
                    i += 4;
                } else {
                    in->base = base;
                    if (base == R_SP || base == R_BP)
                        def = S_SS;
                }
            } else if (rm == 5 && in->mod == 0) {
                if (i + 4 > max)
                    return false;
                in->disp = (int32_t)(p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24));
                i += 4;
                if (c->code64)
                    in->riprel = true;
            } else {
                in->base = rm | ((in->rex & 1) << 3);
                if (in->base == R_BP)
                    def = S_SS;
            }
            if (in->mod == 1) {
                if (i >= max)
                    return false;
                in->disp += (int8_t)p[i++];
            } else if (in->mod == 2) {
                if (i + 4 > max)
                    return false;
                in->disp += (int32_t)(p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24));
                i += 4;
            }
            in->ea_seg = in->seg >= 0 ? in->seg : def;
        }
    }
    int k = ik ? (ik == 2 ? IMM8 : IMM_NONE) : imm_kind(op, in->modrm ? (in->reg & 7) : 0);
    if (k < 0)
        return false;
    int isz = 0;
    switch (k) {
    case IMM8: case REL8: isz = 1; break;
    case IMM16: isz = 2; break;
    case IMMZ: isz = in->osz == 2 ? 2 : 4; break;
    case RELZ: isz = in->osz == 2 && !c->code64 ? 2 : 4; break; /* 64 bits: o 66 e ignorado */
    case IMMV: isz = in->osz; break;
    case MOFFS: isz = in->asz; break;
    default: break;
    }
    if (i + isz > max)
        return false;
    uint64_t v = 0;
    for (int n = 0; n < isz; n++)
        v |= (uint64_t)p[i + n] << (8 * n);
    i += isz;
    if (isz && isz < 8 && k != IMM16 && !(k == IMMV) && k != MOFFS && !(op >= 0xb0 && op <= 0xb7))
        v = sext_sz(v, isz);
    if (k == MOFFS) { /* operando de memoria absoluto */
        in->mem = true;
        in->disp = (int64_t)v;
        in->ea_seg = in->seg >= 0 ? in->seg : S_DS;
    }
    in->imm = (int64_t)v;
    in->len = i;
    in->next = rip + (uint64_t)i;
    if (!c->code64)
        in->next = (uint32_t)in->next;
    if ((k == REL8 || k == RELZ) && in->osz == 2 && !c->code64)
        return false; /* desvios de 16 bits: interpretador */
    if (in->riprel)
        in->disp += (int64_t)in->next;
    return true;
}

/* ------------------------------------------------------------ geracao */

/* x->flags_op: cc_op = CC_NONE e os flags aritmeticos ja estao em c->eflags (COMISS/UCOMISD) */
#define JF_EFL 0x40

typedef struct {
    struct x86_jit *j;
    x86_cpu *c;
    a64 a;
    jit_block *b;
    int nexit;
    int flags_op;   /* operacao preguicosa (CC_*) gravada neste bloco, 0 = desconhecida, JF_EFL = em c->eflags */
    bool stored;    /* a instrucao atual escreveu na memoria */
    bool dead;      /* os flags desta instrucao sao sobrescritos antes de serem lidos */
    uint64_t csbase, page_lin;  /* base do CS e pagina linear do bloco */
    unsigned end_off;           /* deslocamento (na pagina) do fim da ultima instrucao */
    uint32_t mode;
    int flags_size;
    bool code64;
    /* cache de registradores do convidado em registradores do host (escopo: um bloco) */
    int8_t rc_of[16];       /* slot do registrador do convidado, -1 = nao cacheado */
    int8_t rc_guest[8];     /* registrador do convidado em cada slot, -1 = livre */
    uint32_t rc_use[8];     /* ultimo uso (LRU) */
    uint32_t rc_clock;
    uint16_t rc_dirty;      /* bit por registrador do convidado: valor so no host */
    int nocache;            /* > 0: trecho condicional, nao aloca slots (acesso direto a memoria) */
    int slowpath;           /* > 0: chamada em caminho condicional (recarrega o cache depois) */
    bool rip_pending;       /* a instrucao atual pode gerar excecao: grava cur_rip antes de chamar C */
    int64_t ccop_mem;       /* valor (cc_op | cc_size << 32) ja gravado em c->cc_op neste bloco, -1 = ?? */
    uint64_t pending_rip;
    bool rc_on;
    /* caminhos lentos fora da linha: emitidos em j->cold e copiados para o fim do bloco */
    unsigned ncold, nm2c, nc2m;
    uint32_t *hot_p, *hot_end;
} jctx;

/* registradores do host usados pelo cache (x0-x3 sao argumentos, x9-x17 temporarios) */
#define RC_N 7
static const int rc_host[RC_N] = {4, 5, 6, 7, 8, 27, 28};

#define OFF(f) ((uint32_t)offsetof(x86_cpu, f))
#define OFF_R(i) ((uint32_t)(offsetof(x86_cpu, r) + 8 * (unsigned)(i)))

static void rc_writeback(jctx *x);
static void rc_reload(jctx *x);

/* chamada a funcao em C: os registradores sujos vao para c->r antes (o helper pode
 * le-los ou gerar excecao) e todos os cacheados sao recarregados depois (x4-x8 nao
 * sao preservados e o helper pode altera-los) */
static void emit_call(jctx *x, const void *fn)
{
    rc_writeback(x);
    if (x->rip_pending) { /* so as funcoes em C geram excecoes: o caminho rapido nao grava cur_rip */
        a64_mov_imm(&x->a, 9, x->pending_rip);
        a64_str(&x->a, 8, 9, RC, OFF(cur_rip));
    }
    a64_mov_imm(&x->a, 16, (uint64_t)(uintptr_t)fn);
    a64_str(&x->a, 8, RBUD, RC, OFF(jit_budget));
    a64_blr(&x->a, 16);
    a64_ldr(&x->a, 8, RBUD, RC, OFF(jit_budget)); /* x86_set_intr pode ter zerado o orcamento */
    x->ccop_mem = -1; /* a funcao pode ter mudado cc_op */
    if (x->slowpath || x->nocache) {
        rc_reload(x); /* caminho condicional: o estado de compilacao nao pode mudar */
    } else { /* chamada incondicional: esvazia o cache (recarrega sob demanda) */
        memset(x->rc_of, -1, sizeof(x->rc_of));
        memset(x->rc_guest, -1, sizeof(x->rc_guest));
        x->rc_dirty = 0;
    }
}

/* a instrucao pode gerar excecao: cur_rip e gravado por emit_call (so nos caminhos que chamam C) */
static void set_cur_rip(jctx *x, jinsn *in)
{
    if (x->j->off & 256) { /* MVM_JIT_OFF=256: grava sempre, como antes */
        a64_mov_imm(&x->a, 9, in->rip);
        a64_str(&x->a, 8, 9, RC, OFF(cur_rip));
        return;
    }
    x->rip_pending = true;
    x->pending_rip = in->rip;
}

static void rc_reset(jctx *x)
{
    memset(x->rc_of, -1, sizeof(x->rc_of));
    memset(x->rc_guest, -1, sizeof(x->rc_guest));
    memset(x->rc_use, 0, sizeof(x->rc_use));
    x->rc_dirty = 0;
    x->nocache = 0;
    x->slowpath = 0;
}

/* grava no c->r os registradores sujos (nao altera o estado de compilacao: usado em
 * saidas e antes de chamadas, que podem estar em caminhos condicionais) */
static void rc_writeback(jctx *x)
{
    for (int k = 0; k < RC_N; k++) {
        int g = x->rc_guest[k];
        if (g >= 0 && (x->rc_dirty >> g) & 1)
            a64_str(&x->a, 8, rc_host[k], RC, OFF_R(g));
    }
}

/* recarrega todos os registradores cacheados (depois de uma chamada) */
static void rc_reload(jctx *x)
{
    for (int k = 0; k < RC_N; k++)
        if (x->rc_guest[k] >= 0)
            a64_ldr(&x->a, 8, rc_host[k], RC, OFF_R(x->rc_guest[k]));
}

/* registrador do host com o registrador g do convidado (64 bits), ou -1 se nao der para
 * cachear agora; load = carrega o valor atual ao alocar */
static int rc_get(jctx *x, int g, bool load)
{
    int k = x->rc_of[g];
    if (k >= 0) {
        x->rc_use[k] = ++x->rc_clock;
        return rc_host[k];
    }
    if (!x->rc_on || x->nocache)
        return -1;
    int v = -1;
    for (int i = 0; i < RC_N; i++)
        if (x->rc_guest[i] < 0) { v = i; break; }
    if (v < 0) { /* LRU */
        v = 0;
        for (int i = 1; i < RC_N; i++)
            if (x->rc_use[i] < x->rc_use[v])
                v = i;
        int old = x->rc_guest[v];
        if ((x->rc_dirty >> old) & 1)
            a64_str(&x->a, 8, rc_host[v], RC, OFF_R(old));
        x->rc_dirty &= (uint16_t)~(1u << old);
        x->rc_of[old] = -1;
    }
    x->rc_guest[v] = (int8_t)g;
    x->rc_of[g] = (int8_t)v;
    x->rc_use[v] = ++x->rc_clock;
    if (load)
        a64_ldr(&x->a, 8, rc_host[v], RC, OFF_R(g));
    return rc_host[v];
}

/* copia src (64 bits) para dst estendendo com zero o tamanho pedido */
static void zext_to(a64 *a, int dst, int src, int size)
{
    switch (size) {
    case 8: if (dst != src) a64_mov(a, 1, dst, src); break;
    case 4: a64_mov(a, 0, dst, src); break;
    case 2: a64_ubfx(a, dst, src, 0, 16); break;
    default: a64_ubfx(a, dst, src, 0, 8); break;
    }
}

/* carrega registrador do convidado (tamanho em bytes, estende com zero) */
static void ld_greg(jctx *x, int dst, int idx, int size, bool rex)
{
    a64 *a = &x->a;
    bool high = size == 1 && !rex && idx >= 4 && idx < 8;
    int g = high ? idx - 4 : idx;
    int h = rc_get(x, g, true);
    if (h >= 0) {
        if (high)
            a64_ubfx(a, dst, h, 8, 8);
        else
            zext_to(a, dst, h, size);
        return;
    }
    if (high) {
        a64_ldr(a, 8, dst, RC, OFF_R(g));
        a64_ubfx(a, dst, dst, 8, 8);
        return;
    }
    a64_ldr(a, size, dst, RC, OFF_R(idx));
}

/* grava registrador do convidado com a semantica do x86 (32 bits zera a parte alta) */
static void st_greg(jctx *x, int src, int idx, int size, bool rex)
{
    a64 *a = &x->a;
    bool high = size == 1 && !rex && idx >= 4 && idx < 8;
    int g = high ? idx - 4 : idx;
    int h = rc_get(x, g, size < 4);
    if (h >= 0) {
        if (high)
            a64_bfi(a, h, src, 8, 8);
        else if (size == 8)
            a64_mov(a, 1, h, src);
        else if (size == 4)
            a64_mov(a, 0, h, src);
        else
            a64_bfi(a, h, src, 0, size * 8);
        x->rc_dirty |= (uint16_t)(1u << g);
        return;
    }
    switch (size) {
    case 8: a64_str(a, 8, src, RC, OFF_R(idx)); break;
    case 4:
        a64_mov(a, 0, 9, src);
        a64_str(a, 8, 9, RC, OFF_R(idx));
        break;
    case 2: a64_str(a, 2, src, RC, OFF_R(idx)); break;
    default:
        if (high) {
            a64_ldr(a, 8, 9, RC, OFF_R(g));
            a64_bfi(a, 9, src, 8, 8);
            a64_str(a, 8, 9, RC, OFF_R(g));
        } else {
            a64_str(a, 1, src, RC, OFF_R(idx));
        }
        break;
    }
}

static void add_const(jctx *x, int rd, int rn, int64_t v)
{
    a64 *a = &x->a;
    if (v == 0) {
        if (rd != rn)
            a64_mov(a, 1, rd, rn);
    } else if (v > 0 && v < 4096) {
        a64_add_imm(a, rd, rn, (uint32_t)v);
    } else if (v < 0 && v > -4096) {
        a64_sub_imm(a, rd, rn, (uint32_t)-v);
    } else if (v > -0x1000000 && v < 0x1000000 && !(x->j->off & 16384)) { /* 2 instrucoes (imediato << 12 + resto) */
        uint64_t m = (uint64_t)(v < 0 ? -v : v);
        a64_addsub_imm(a, 1, v < 0, 0, rd, rn, (uint32_t)(m >> 12), 1);
        if (m & 0xfff)
            a64_addsub_imm(a, 1, v < 0, 0, rd, rd, (uint32_t)(m & 0xfff), 0);
    } else {
        a64_mov_imm(a, 9, (uint64_t)v);
        a64_add(a, rd, rn, 9);
    }
}

/* endereco linear do operando de memoria em REA */
static void emit_ea(jctx *x, jinsn *in)
{
    a64 *a = &x->a;
    bool direct = !(x->j->off & 16384);
    /* base e indice direto dos registradores do cache (sem copiar antes) */
    int hb = -1, hi = -1;
    if (in->base >= 0) {
        hb = direct ? rc_get(x, in->base, true) : -1;
        if (hb < 0) {
            ld_greg(x, REA, in->base, 8, true);
            hb = REA;
        }
    }
    if (in->index >= 0) {
        hi = direct ? rc_get(x, in->index, true) : -1;
        if (hi < 0) {
            ld_greg(x, 10, in->index, 8, true);
            hi = 10;
        }
    }
    if (hb >= 0 && hi >= 0) {
        a64_addsub_reg(a, 1, 0, 0, REA, hb, hi, in->scale);
        add_const(x, REA, REA, in->disp);
    } else if (hb >= 0) {
        add_const(x, REA, hb, in->disp);
    } else if (hi >= 0) {
        if (in->scale)
            a64_lsl_imm(a, REA, hi, in->scale);
        else
            a64_mov(a, 1, REA, hi);
        add_const(x, REA, REA, in->disp);
    } else {
        a64_mov_imm(a, REA, (uint64_t)in->disp);
    }
    if (in->asz == 4)
        a64_mov(a, 0, REA, REA);
    if (!x->code64 || in->ea_seg == S_FS || in->ea_seg == S_GS) {
        a64_ldr(a, 8, 10, RC, (uint32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * (unsigned)in->ea_seg + offsetof(x86_seg, base)));
        a64_add(a, REA, REA, 10);
        if (!x->code64)
            a64_mov(a, 0, REA, REA);
    }
}

static uint64_t jit_rd(x86_cpu *c, uint64_t lin, uint64_t size) { return x86_read_slow(c, lin, (unsigned)size, x86_user(c)); }
static void jit_wr(x86_cpu *c, uint64_t lin, uint64_t v, uint64_t size) { x86_write_slow(c, lin, v, (unsigned)size, x86_user(c)); }

/* x9 = &tlb[idx]; salta para 'slow' se a pagina nao bate (tag em 'tag_off') ou se o acesso cruza a pagina */
static void emit_tlb(jctx *x, int addr, int size, uint32_t tag_off, uint32_t **slow1, uint32_t **slow2)
{
    a64 *a = &x->a;
    a64_ubfx(a, 9, addr, 12, X86_TLB_BITS);
    a64_addsub_reg(a, 1, 0, 0, 9, 9, 9, 2);      /* idx * 5 */
    a64_addsub_reg(a, 1, 0, 0, 9, RTLB, 9, 3);   /* base + idx * 40 */
    a64_ldr(a, 8, 10, 9, tag_off);
    *slow2 = NULL;
    if (size > 1 && !(x->j->off & 512)) {
        /* pagina do ULTIMO byte: se o acesso cruza a pagina ela difere da etiqueta (que e a
         * pagina do primeiro byte, ja que o indice vem dele) e vai para o caminho lento */
        a64_add_imm(a, 11, addr, (uint32_t)(size - 1));
        a64_and_bitmask(a, 11, 11, A64_MASK_PAGE_HI);
        a64_cmp(a, 1, 10, 11);
        *slow1 = a64_here(a);
        a64_bcond(a, A_NE, a64_here(a));
    } else {
        a64_and_bitmask(a, 11, addr, A64_MASK_PAGE_HI);
        a64_cmp(a, 1, 10, 11);
        *slow1 = a64_here(a);
        a64_bcond(a, A_NE, a64_here(a));
        if (size > 1) {
            a64_and_bitmask(a, 11, addr, A64_MASK_PAGE_LO);
            a64_addsub_imm(a, 1, 1, 1, XZR, 11, (uint32_t)(0x1000 - size), 0);
            *slow2 = a64_here(a);
            a64_bcond(a, A_HI, a64_here(a));
        }
    }
    a64_ldr(a, 8, 10, 9, (uint32_t)offsetof(x86_tlbe, addend));
}

/* Caminho lento fora da linha: os desvios s1/s2 do caminho rapido vao para um trecho
 * emitido em j->cold (copiado para depois do fim do bloco), que volta para 'done'. O
 * caminho rapido fica sem o salto por cima da chamada. */
static bool cold_begin(jctx *x, uint32_t *s1, uint32_t *s2)
{
    struct x86_jit *j = x->j;
    if ((j->off & 32768) || x->nm2c + 2 > COLD_FIX || x->nc2m + 1 > COLD_FIX || x->ncold + 256 > COLD_MAX)
        return false;
    j->m2c[x->nm2c].site = s1;
    j->m2c[x->nm2c++].coff = x->ncold;
    if (s2) {
        j->m2c[x->nm2c].site = s2;
        j->m2c[x->nm2c++].coff = x->ncold;
    }
    x->hot_p = x->a.p;
    x->hot_end = x->a.end;
    x->a.p = j->cold + x->ncold;
    x->a.end = j->cold + COLD_MAX;
    return true;
}

static void cold_end(jctx *x, uint32_t *done)
{
    struct x86_jit *j = x->j;
    j->c2m[x->nc2m].coff = (unsigned)(x->a.p - j->cold);
    j->c2m[x->nc2m++].target = done;
    a64_b(&x->a, x->a.p); /* corrigido na copia */
    x->ncold = (unsigned)(x->a.p - j->cold);
    x->a.p = x->hot_p;
    x->a.end = x->hot_end;
}

/* fim de um trecho fora da linha que termina numa saida do bloco (ultima instrucao:
 * B para o stub de saida, corrigido na copia) */
static void cold_end_exit(jctx *x)
{
    struct x86_jit *j = x->j;
    j->c2m[x->nc2m].coff = (unsigned)(x->a.p - j->cold) - 1;
    j->c2m[x->nc2m++].target = j->exit_stub;
    x->ncold = (unsigned)(x->a.p - j->cold);
    x->a.p = x->hot_p;
    x->a.end = x->hot_end;
}

/* copia os caminhos lentos para a posicao atual e corrige os desvios */
static void cold_flush(jctx *x)
{
    struct x86_jit *j = x->j;
    a64 *a = &x->a;
    if (!x->ncold)
        return;
    if (a->p + x->ncold > a->end) {
        a->overflow = true;
        return;
    }
    uint32_t *base = a->p;
    memcpy(base, j->cold, x->ncold * sizeof(uint32_t));
    a->p += x->ncold;
    for (unsigned i = 0; i < x->nm2c; i++)
        a64_patch(j->m2c[i].site, base + j->m2c[i].coff);
    for (unsigned i = 0; i < x->nc2m; i++)
        a64_patch(base + j->c2m[i].coff, j->c2m[i].target);
}

/* dst = memoria[REA] (size bytes, estendido com zero) */
static void emit_load(jctx *x, int dst, int size)
{
    a64 *a = &x->a;
    uint32_t *s1, *s2;
    emit_tlb(x, REA, size, (uint32_t)offsetof(x86_tlbe, tag_r), &s1, &s2);
    a64_ldr_reg(a, size, dst, 10, REA);
    if (cold_begin(x, s1, s2)) {
        uint32_t *done = x->hot_p;
        a64_mov(a, 1, 0, RC);
        a64_mov(a, 1, 1, REA);
        a64_mov_imm(a, 2, (uint64_t)size);
        x->slowpath++;
        emit_call(x, (const void *)jit_rd);
        x->slowpath--;
        if (dst != 0)
            a64_mov(a, 1, dst, 0);
        cold_end(x, done);
        return;
    }
    uint32_t *jdone = a64_here(a);
    a64_b(a, a64_here(a));
    uint32_t *slow = a64_here(a);
    a64_patch(s1, slow);
    if (s2)
        a64_patch(s2, slow);
    a64_mov(a, 1, 0, RC);
    a64_mov(a, 1, 1, REA);
    a64_mov_imm(a, 2, (uint64_t)size);
    x->slowpath++;
    emit_call(x, (const void *)jit_rd);
    x->slowpath--;
    if (dst != 0)
        a64_mov(a, 1, dst, 0);
    a64_patch(jdone, a64_here(a));
}

static void emit_exit_next(jctx *x, uint64_t next, bool chain);

/* memoria[REA] = src (src nao pode ser x0-x3) */
static void emit_store(jctx *x, int src, int size, uint64_t next_rip)
{
    a64 *a = &x->a;
    uint32_t *s1, *s2;
    emit_tlb(x, REA, size, (uint32_t)offsetof(x86_tlbe, tag_w), &s1, &s2);
    a64_str_reg(a, size, src, 10, REA);
    if (cold_begin(x, s1, s2)) {
        uint32_t *done = x->hot_p;
        a64_mov(a, 1, 0, RC);
        a64_mov(a, 1, 1, REA);
        a64_mov(a, 1, 2, src);
        a64_mov_imm(a, 3, (uint64_t)size);
        x->slowpath++;
        emit_call(x, (const void *)jit_wr);
        x->slowpath--;
        cold_end(x, done);
        (void)next_rip;
        x->stored = true; /* verifica codigo automodificavel ao fim da instrucao */
        return;
    }
    uint32_t *jdone = a64_here(a);
    a64_b(a, a64_here(a));
    uint32_t *slow = a64_here(a);
    a64_patch(s1, slow);
    if (s2)
        a64_patch(s2, slow);
    a64_mov(a, 1, 0, RC);
    a64_mov(a, 1, 1, REA);
    a64_mov(a, 1, 2, src);
    a64_mov_imm(a, 3, (uint64_t)size);
    x->slowpath++;
    emit_call(x, (const void *)jit_wr);
    x->slowpath--;
    (void)next_rip;
    x->stored = true; /* verifica codigo automodificavel ao fim da instrucao */
    a64_patch(jdone, a64_here(a));
}

/* flags preguicosos */
static void emit_lazy(jctx *x, int op, int size, int dst, int s1, int s2)
{
    a64 *a = &x->a;
    if (x->dead) {
        x->flags_op = 0;
        return;
    }
    int64_t v = (int64_t)((uint64_t)(uint32_t)op | ((uint64_t)(uint32_t)size << 32));
    if (v != x->ccop_mem || (x->j->off & 1024)) { /* mesma operacao ja gravada neste bloco: nao regrava */
        a64_mov_imm(a, 9, (uint64_t)v);
        a64_str(a, 8, 9, RC, OFF(cc_op));
        x->ccop_mem = v;
    }
    a64_str(a, 8, dst, RC, OFF(cc_dst));
    if (op != CC_LOGIC || (x->j->off & 1024)) { /* LOGIC so usa o resultado */
        a64_str(a, 8, s1, RC, OFF(cc_src1));
        a64_str(a, 8, s2, RC, OFF(cc_src2));
    }
    x->flags_op = op;
    x->flags_size = size;
}

/* x87 ja decodificado pelo JIT: info = modrm | (opcode & 7) << 8 | osz << 16; lin = endereco */
static void jit_x87(x86_cpu *c, uint64_t info, uint64_t lin)
{
    if (c->cr0 & (CR0_EM | CR0_TS))
        x86_exception(c, EXC_NM, 0, 0);
    x86_dec d;
    memset(&d, 0, sizeof(d));
    d.modrm = (uint8_t)info;
    d.mod = d.modrm >> 6;
    d.reg = (d.modrm >> 3) & 7;
    d.rm = d.modrm & 7;
    d.mem = d.mod != 3;
    d.osz = (int)((info >> 16) & 0xff);
    d.seg = -1;
    x87_exec_at(c, &d, 0xd8 + (int)((info >> 8) & 7), lin);
}

void x86_exec_one(x86_cpu *c);
/* executa a instrucao em c->rip pelo interpretador (chamado de dentro de um bloco) */
static void jit_interp1(x86_cpu *c) { x86_exec_one(c); }

/* CF atual em x0 (0/1) */
static uint64_t jit_cf(x86_cpu *c) { return x86_cf(c) ? 1 : 0; }
static uint64_t jit_cond(x86_cpu *c, uint64_t cc) { return x86_cond(c, (int)cc) ? 1 : 0; }

/*
 * Avalia a condicao x86 'cc'. Retorna um codigo de condicao A64 valido para os
 * NZCV atuais, ou -1 se o resultado ficou em w0 (0/1) por meio de x86_cond.
 * Retorna A_AL+1 para "sempre falso" e A_AL para "sempre verdadeiro".
 */
#define COND_W0 (-1)
#define COND_NEVER 15
static int emit_cond(jctx *x, int cc)
{
    a64 *a = &x->a;
    int t = cc >> 1, neg = cc & 1;
    int sz = x->flags_size;
    if (x->flags_op == CC_SUB && t != 5) {
        a64_ldr(a, 8, 0, RC, OFF(cc_src1));
        a64_ldr(a, 8, 1, RC, OFF(cc_src2));
        if (sz == 4) {
            a64_cmp(a, 0, 0, 1);
        } else {
            if (sz < 4) {
                a64_lsl_imm(a, 0, 0, 64 - 8 * sz);
                a64_lsl_imm(a, 1, 1, 64 - 8 * sz);
            }
            a64_cmp(a, 1, 0, 1);
        }
        static const int map[8] = {A_VS, A_LO, A_EQ, A_LS, A_MI, -2, A_LT, A_LE};
        int r = map[t];
        return neg ? (r ^ 1) : r;
    }
    if (x->flags_op == CC_LOGIC && t != 5) {
        a64_ldr(a, 8, 0, RC, OFF(cc_dst));
        if (sz == 4) {
            a64_tst(a, 0, 0, 0);
        } else {
            if (sz < 4)
                a64_lsl_imm(a, 0, 0, 64 - 8 * sz);
            a64_tst(a, 1, 0, 0);
        }
        int r;
        switch (t) {
        case 0: case 1: r = COND_NEVER; break; /* OF=0, CF=0 */
        case 2: case 3: r = A_EQ; break;       /* ZF; CF|ZF = ZF */
        case 4: case 6: r = A_MI; break;       /* SF; SF!=OF = SF */
        default: r = A_LE; break;              /* ZF|SF (V=0 apos TST) */
        }
        if (r == COND_NEVER)
            return neg ? A_AL : COND_NEVER;
        return neg ? (r ^ 1) : r;
    }
    if (x->j->off & 1)
        goto helper;
    if (x->flags_op == JF_EFL && t <= 5) { /* OF, CF, ZF, CF|ZF, SF, PF direto de eflags */
        static const uint32_t bits[6] = {EFL_OF, EFL_CF, EFL_ZF, EFL_CF | EFL_ZF, EFL_SF, EFL_PF};
        a64_ldr(a, 8, 0, RC, OFF(eflags));
        a64_mov_imm(a, 1, bits[t]);
        a64_tst(a, 1, 0, 1);
        return neg ? A_EQ : A_NE;
    }
    if (x->flags_op == JF_EFL)
        goto helper;
    if (x->flags_op && (t == 2 || t == 4)) {
        /* ZF/SF vem sempre do resultado (cc_dst), qualquer que seja a operacao */
        a64_ldr(a, 8, 0, RC, OFF(cc_dst));
        if (sz == 4) {
            a64_tst(a, 0, 0, 0);
        } else {
            if (sz < 4)
                a64_lsl_imm(a, 0, 0, 64 - 8 * sz);
            a64_tst(a, 1, 0, 0);
        }
        int r = t == 2 ? A_EQ : A_MI;
        return neg ? (r ^ 1) : r;
    }
    if (x->flags_op == CC_SZP && t <= 1) { /* CF = aux bit 0, OF = aux bit 1 */
        a64_ldr(a, 8, 0, RC, OFF(cc_aux));
        a64_and_bitmask(a, 0, 0, t == 1 ? ((1u << 12) | (0u << 6) | 0u) : ((1u << 12) | (63u << 6) | 0u));
        a64_cmp(a, 1, 0, XZR);
        return neg ? A_EQ : A_NE;
    }
    if (x->flags_op == CC_ADD && t == 1) { /* CF = resultado < src1 (sem sinal, no tamanho) */
        a64_ldr(a, 8, 0, RC, OFF(cc_dst));
        a64_ldr(a, 8, 1, RC, OFF(cc_src1));
        if (sz == 4) {
            a64_cmp(a, 0, 0, 1);
        } else {
            if (sz < 4) {
                a64_lsl_imm(a, 0, 0, 64 - 8 * sz);
                a64_lsl_imm(a, 1, 1, 64 - 8 * sz);
            }
            a64_cmp(a, 1, 0, 1);
        }
        return neg ? A_HS : A_LO;
    }
helper:
    a64_mov(a, 1, 0, RC);
    a64_mov_imm(a, 1, (uint64_t)cc);
    emit_call(x, (const void *)jit_cond);
    return COND_W0;
}

/* ---- saidas ---- */

/* motivo da saida (estatistica): c->jit_pad */
enum { EX_NONE, EX_JC_DYN, EX_JC_PAGE, EX_CSBASE, EX_CHAIN, EX_NOCHAIN, EX_BUDGET, EX_N };
static const char *const ex_names[EX_N] = {"-", "indireto sem cache", "outra pagina sem cache", "CS com base",
                                           "encadeavel", "nao encadeavel", "orcamento"};
static void emit_exit_why(jctx *x, int why)
{
    a64_mov_imm(&x->a, 9, (uint64_t)why);
    a64_str(&x->a, 4, 9, RC, OFF(jit_pad));
}

static void emit_exit_seq(jctx *x, uint64_t rip_const, int rip_reg, jit_exit_rec *rec)
{
    a64 *a = &x->a;
    emit_exit_why(x, rec ? EX_CHAIN : EX_NOCHAIN);
    if (rip_reg >= 0)
        a64_str(a, 8, rip_reg, RC, OFF(rip));
    else {
        a64_mov_imm(a, 9, rip_const);
        a64_str(a, 8, 9, RC, OFF(rip));
    }
    a64_mov_imm(a, 9, (uint64_t)(uintptr_t)rec);
    a64_str(a, 8, 9, RC, OFF(jit_exit));
    a64_b(a, x->j->exit_stub);
}

/* saida com consulta ao cache de saltos: RIP em rip_reg (ou constante) */
static void emit_exit_lookup(jctx *x, uint64_t rip_const, int rip_reg)
{
    a64 *a = &x->a;
    rc_writeback(x);
    int src = rip_reg;
    if (src < 0) {
        a64_mov_imm(a, 13, rip_const);
        src = 13;
    }
    a64_str(a, 8, src, RC, OFF(rip));
    if (x->csbase) {
        emit_exit_why(x, EX_CSBASE);
        a64_str(a, 8, XZR, RC, OFF(jit_exit));
        a64_b(a, x->j->exit_stub);
        return;
    }
    /* cache deste ponto de salto: {chave, codigo, pagina fisica, geracao} embutido no codigo */
    if ((uintptr_t)a->p & 4)
        a64_put(a, 0xD503201Fu); /* nop: alinha o slot em 8 bytes */
    uint32_t *jover = a64_here(a);
    a64_b(a, a64_here(a));
    uint32_t *slot = a64_here(a);
    for (int k = 0; k < 8; k++)
        a64_put(a, k < 2 || k == 4 || k == 5 ? 0xffffffffu : 0); /* chave = ~0, phys = ~0, geracao = 0 */
    a64_patch(jover, a64_here(a));
    a64_adr(a, 17, slot);
    a64_ldp(a, 10, 11, 17, 0);
    a64_mov_imm(a, 12, (uint64_t)x->mode << 59);
    a64_logic(a, 1, 2, 0, 12, src, 12);
    a64_cmp(a, 1, 10, 12);
    uint32_t *smiss1 = a64_here(a);
    a64_bcond(a, A_NE, a64_here(a));
    a64_ldp(a, 10, 15, 17, 16);                           /* phys, geracao */
    a64_ldr(a, 8, 16, RJC, 24);                           /* geracao atual (jc[0].pad) */
    a64_cmp(a, 1, 15, 16);
    uint32_t *smiss2 = a64_here(a);
    a64_bcond(a, A_NE, a64_here(a));
    a64_ubfx(a, 14, src, 12, X86_TLB_BITS);
    a64_addsub_reg(a, 1, 0, 0, 14, 14, 14, 2);
    a64_addsub_reg(a, 1, 0, 0, 14, RTLB, 14, 3);
    a64_ldr(a, 8, 15, 14, (uint32_t)offsetof(x86_tlbe, tag_x));
    a64_and_bitmask(a, 16, src, A64_MASK_PAGE_HI);
    a64_cmp(a, 1, 15, 16);
    uint32_t *smiss3 = a64_here(a);
    a64_bcond(a, A_NE, a64_here(a));
    a64_ldr(a, 8, 15, 14, (uint32_t)offsetof(x86_tlbe, pa));
    a64_cmp(a, 1, 15, 10);
    uint32_t *smiss4 = a64_here(a);
    a64_bcond(a, A_NE, a64_here(a));
    a64_br(a, 11);
    a64_patch(smiss1, a64_here(a));
    a64_patch(smiss2, a64_here(a));
    a64_patch(smiss3, a64_here(a));
    a64_patch(smiss4, a64_here(a));
    /* cache global */
    a64_logic_sh(a, 1, 2, 9, src, src, 1, 12);            /* x9 = rip ^ (rip >> 12) */
    a64_and_bitmask(a, 9, 9, (1u << 12) | (unsigned)(__builtin_ctz(JC_SIZE) - 1)); /* & (JC_SIZE - 1) */
    a64_addsub_reg(a, 1, 0, 0, 9, RJC, 9, 5);             /* &jc[h] (32 bytes) */
    a64_ldp(a, 10, 11, 9, 0);
    a64_mov_imm(a, 12, (uint64_t)x->mode << 59);
    a64_logic(a, 1, 2, 0, 12, src, 12);                   /* chave = rip ^ (modo << 59) */
    a64_cmp(a, 1, 10, 12);
    uint32_t *miss = a64_here(a);
    a64_bcond(a, A_NE, a64_here(a));
    /* o RIP ainda mapeia (TLB de execucao) para a pagina fisica do bloco? */
    a64_ldr(a, 8, 10, 9, 16);                             /* phys do bloco */
    a64_ubfx(a, 14, src, 12, X86_TLB_BITS);
    a64_addsub_reg(a, 1, 0, 0, 14, 14, 14, 2);            /* idx * 5 */
    a64_addsub_reg(a, 1, 0, 0, 14, RTLB, 14, 3);          /* &tlb[idx] (40 bytes) */
    a64_ldr(a, 8, 15, 14, (uint32_t)offsetof(x86_tlbe, tag_x));
    a64_and_bitmask(a, 16, src, A64_MASK_PAGE_HI);
    a64_cmp(a, 1, 15, 16);
    uint32_t *miss2 = a64_here(a);
    a64_bcond(a, A_NE, a64_here(a));
    a64_ldr(a, 8, 15, 14, (uint32_t)offsetof(x86_tlbe, pa));
    a64_cmp(a, 1, 15, 10);
    uint32_t *miss3 = a64_here(a);
    a64_bcond(a, A_NE, a64_here(a));
    a64_br(a, 11);
    a64_patch(miss, a64_here(a));
    a64_patch(miss2, a64_here(a));
    a64_patch(miss3, a64_here(a));
    emit_exit_why(x, rip_reg < 0 ? EX_JC_PAGE : EX_JC_DYN);
    a64_str(a, 8, 17, RC, OFF(jit_site));
    a64_str(a, 8, XZR, RC, OFF(jit_exit));
    a64_b(a, x->j->exit_stub);
}

/* saida para um RIP constante; chain = pode ser encadeada diretamente */
static void emit_exit_next(jctx *x, uint64_t next, bool chain)
{
    a64 *a = &x->a;
    jit_exit_rec *rec = NULL;
    uint64_t tl = x->code64 ? next : (uint32_t)(x->csbase + next);
    if (chain && (tl & ~0xfffULL) != x->page_lin) { /* outra pagina: cache de saltos */
        emit_exit_lookup(x, next, -1);
        return;
    }
    rc_writeback(x);
    if (chain && x->nexit < 2) {
        rec = &x->b->exit[x->nexit++];
        rec->blk = x->b;
        rec->target = next;
        rec->patch = a64_here(a);
        a64_b(a, a64_here(a) + 1); /* inicialmente cai na sequencia de saida */
    }
    emit_exit_seq(x, next, -1, rec);
}

/* ---- instrucoes ---- */

static int op_size(jinsn *in) { return in->osz; }
static bool rexf(jinsn *in) { return in->rex != 0; }

/* le o operando r/m em dst */
static void load_rm(jctx *x, jinsn *in, int dst, int size)
{
    if (in->mem) {
        emit_ea(x, in);
        emit_load(x, dst, size);
    } else {
        ld_greg(x, dst, in->rm, size, rexf(in));
    }
}

/* grava o resultado no r/m (REA ja calculado se for memoria) */
static void store_rm(jctx *x, jinsn *in, int src, int size)
{
    if (in->mem)
        emit_store(x, src, size, in->next);
    else
        st_greg(x, src, in->rm, size, rexf(in));
}

/* RT = CF atual (0/1) para ADC/SBB */
/* compara x0 e x1 no tamanho dos flags (sem sinal) */
static void cmp_sized(a64 *a, int sz)
{
    if (sz == 4) {
        a64_cmp(a, 0, 0, 1);
        return;
    }
    if (sz < 4) {
        a64_lsl_imm(a, 0, 0, 64 - 8 * sz);
        a64_lsl_imm(a, 1, 1, 64 - 8 * sz);
    }
    a64_cmp(a, 1, 0, 1);
}

/* dst = CF (0/1) calculado em linha quando a operacao preguicosa e conhecida no bloco */
static bool emit_cf_inline(jctx *x, int dst)
{
    a64 *a = &x->a;
    int sz = x->flags_size;
    if (x->j->off & 64)
        return false;
    switch (x->flags_op) {
    case JF_EFL:
        a64_ldr(a, 8, dst, RC, OFF(eflags));
        a64_and_bitmask(a, dst, dst, (1u << 12) | 0u); /* & 1 (CF) */
        return true;
    case CC_LOGIC:
        a64_mov_imm(a, dst, 0);
        return true;
    case CC_SZP: case CC_INC: case CC_DEC:
        a64_ldr(a, 8, dst, RC, OFF(cc_aux));
        a64_and_bitmask(a, dst, dst, (1u << 12) | 0u); /* & 1 */
        return true;
    case CC_ADD: case CC_ADC: /* CF = res < a (ADC com carry: res <= a) */
        a64_ldr(a, 8, 0, RC, OFF(cc_dst));
        a64_ldr(a, 8, 1, RC, OFF(cc_src1));
        break;
    case CC_SUB: case CC_SBB: /* CF = a < b (SBB com borrow: a <= b) */
        a64_ldr(a, 8, 0, RC, OFF(cc_src1));
        a64_ldr(a, 8, 1, RC, OFF(cc_src2));
        break;
    default:
        return false;
    }
    cmp_sized(a, sz);
    if (x->flags_op == CC_ADD || x->flags_op == CC_SUB) {
        a64_cset(a, dst, A_LO);
        return true;
    }
    a64_cset(a, 9, A_LO);
    a64_cset(a, 10, A_LS);
    a64_ldr(a, 8, 11, RC, OFF(cc_aux));
    a64_addsub_imm(a, 1, 1, 1, XZR, 11, 0, 0); /* cmp aux, #0 */
    a64_csel(a, 1, dst, 10, 9, A_NE);
    return true;
}

static void emit_carry(jctx *x)
{
    if (emit_cf_inline(x, RT))
        return;
    a64_mov(&x->a, 1, 0, RC);
    emit_call(x, (const void *)jit_cf);
    a64_mov(&x->a, 1, RT, 0);
}

/* flags de ADC/SBB (depois da escrita: instrucao reiniciavel) */
static void emit_adc_lazy(jctx *x, int aop, int size)
{
    if (!x->dead)
        a64_str(&x->a, 8, RT, RC, OFF(cc_aux));
    emit_lazy(x, aop == 2 ? CC_ADC : CC_SBB, size, RR, RA, RB);
}

/* operacao ALU x86 (0=add 1=or 4=and 5=sub 6=xor 7=cmp) em RA op RB -> RR, com flags */
static void emit_alu(jctx *x, int op, int size)
{
    a64 *a = &x->a;
    switch (op) {
    case 0: a64_add(a, RR, RA, RB); emit_lazy(x, CC_ADD, size, RR, RA, RB); break;
    case 1: a64_logic(a, 1, 1, 0, RR, RA, RB); emit_lazy(x, CC_LOGIC, size, RR, XZR, XZR); break;
    case 4: a64_logic(a, 1, 0, 0, RR, RA, RB); emit_lazy(x, CC_LOGIC, size, RR, XZR, XZR); break;
    case 6: a64_logic(a, 1, 2, 0, RR, RA, RB); emit_lazy(x, CC_LOGIC, size, RR, XZR, XZR); break;
    case 2: case 3: /* ADC/SBB: CF de entrada (RT, preservado) */
        if (op == 2) {
            a64_add(a, RR, RA, RB);
            a64_add(a, RR, RR, RT);
        } else {
            a64_sub(a, RR, RA, RB);
            a64_sub(a, RR, RR, RT);
        }
        break;
    default: a64_sub(a, RR, RA, RB); emit_lazy(x, CC_SUB, size, RR, RA, RB); break;
    }
}

/* ADD/SUB/AND/OR/XOR registrador, imediato (32/64 bits) direto no registrador do cache,
 * com as formas de imediato do AArch64: 1 instrucao em vez de copia + constante + copia.
 * ADD/SUB so com os flags mortos; as logicas guardam o resultado para os flags. */
static bool emit_alu_imm_reg(jctx *x, int idx, int aop, int s, int64_t imm)
{
    a64 *a = &x->a;
    if ((x->j->off & 16384) || (s != 4 && s != 8))
        return false;
    int sf = s == 8;
    uint64_t v = sf ? (uint64_t)imm : (uint64_t)(uint32_t)imm;
    uint32_t enc = 0;
    int kind; /* 0 = add/sub imm12, 1 = logica */
    int sub = 0, opc = 0;
    uint32_t imm12 = 0;
    int sh = 0;
    if (aop == 0 || aop == 5) {
        if (!x->dead)
            return false;
        int64_t sv = sf ? imm : (int64_t)(int32_t)imm;
        sub = aop == 5;
        if (sv < 0) { /* soma de negativo = subtracao */
            sv = -sv;
            sub = !sub;
        }
        if (sv < 0x1000) {
            imm12 = (uint32_t)sv;
        } else if (!(sv & 0xfff) && sv < 0x1000000) {
            imm12 = (uint32_t)(sv >> 12);
            sh = 1;
        } else {
            return false;
        }
        kind = 0;
    } else if (aop == 1 || aop == 4 || aop == 6) {
        opc = aop == 4 ? 0 : aop == 1 ? 1 : 2;
        if (!a64_encode_bitmask(v, sf, &enc))
            return false;
        kind = 1;
    } else {
        return false;
    }
    int h = rc_get(x, idx, true);
    if (h < 0)
        return false;
    if (kind == 0)
        a64_addsub_imm(a, sf, sub, 0, h, h, imm12, sh);
    else
        a64_logic_imm(a, sf, opc, h, h, enc);
    x->rc_dirty |= (uint16_t)(1u << idx);
    if (kind == 1)
        emit_lazy(x, CC_LOGIC, s, h, XZR, XZR);
    else
        x->flags_op = 0; /* flags mortos */
    return true;
}

/* ADD/SUB/AND/OR/XOR/CMP de 32/64 bits com o destino (registrador do convidado dst) e a
 * fonte (registrador do convidado src_idx, ou ja em src_host) nos registradores do cache.
 * Flags mortos: 1 instrucao no proprio registrador. Os flags preguicosos aceitam as fontes
 * com lixo acima do tamanho (quem os le mascara pelo tamanho). */
static bool emit_alu_rr(jctx *x, int aop, int s, int dst, int src_idx, int src_host)
{
    a64 *a = &x->a;
    if (aop == 2 || aop == 3 || (s != 4 && s != 8))
        return false;
    int hs = src_host >= 0 ? src_host : rc_get(x, src_idx, true);
    if (hs < 0)
        return false;
    int hd = rc_get(x, dst, true);
    if (hd < 0)
        return false;
    int sf = s == 8;
    bool logic = aop == 1 || aop == 4 || aop == 6;
    if (x->dead) {
        x->flags_op = 0;
        if (aop == 7)
            return true; /* CMP com flags mortos: nada */
    }
    int rd = x->dead ? hd : RR;
    switch (aop) {
    case 0: a64_addsub_reg(a, sf, 0, 0, rd, hd, hs, 0); break;
    case 5: case 7: a64_addsub_reg(a, sf, 1, 0, rd, hd, hs, 0); break;
    case 4: a64_logic(a, sf, 0, 0, rd, hd, hs); break;
    case 1: a64_logic(a, sf, 1, 0, rd, hd, hs); break;
    default: a64_logic(a, sf, 2, 0, rd, hd, hs); break;
    }
    if (!x->dead) {
        emit_lazy(x, logic ? CC_LOGIC : aop == 0 ? CC_ADD : CC_SUB, s, RR, logic ? XZR : hd, logic ? XZR : hs);
        if (aop == 7)
            return true;
        a64_mov(a, sf, hd, RR);
    }
    x->rc_dirty |= (uint16_t)(1u << dst);
    return true;
}

/* MOV registrador, registrador (32/64 bits) entre os registradores do cache: 1 instrucao */
static bool mov_reg_reg(jctx *x, int dst, int src, int s)
{
    int hs = rc_get(x, src, true);
    if (hs < 0)
        return false;
    int hd = rc_get(x, dst, false);
    if (hd < 0)
        return false;
    if (s == 8) {
        if (hd != hs)
            a64_mov(&x->a, 1, hd, hs);
    } else {
        a64_mov(&x->a, 0, hd, hs); /* 32 bits zera a parte alta, como no x86 */
    }
    x->rc_dirty |= (uint16_t)(1u << dst);
    return true;
}

/* push de RT (tamanho sz) */
static void emit_push(jctx *x, jinsn *in, int src, int sz)
{
    a64 *a = &x->a;
    ld_greg(x, RB, R_SP, 8, true);
    a64_sub_imm(a, RB, RB, (uint32_t)sz);
    if (!x->code64)
        a64_mov(a, 0, RB, RB);
    if (x->code64) {
        a64_mov(a, 1, REA, RB);
    } else {
        a64_ldr(a, 8, 10, RC, (uint32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * S_SS + offsetof(x86_seg, base)));
        a64_add(a, REA, RB, 10);
        a64_mov(a, 0, REA, REA);
    }
    emit_store(x, src, sz, in->next);
    st_greg(x, RB, R_SP, 8, true);
}

/* pop para dst (tamanho sz); RSP so e atualizado depois (instrucao reiniciavel) */
static void emit_pop_load(jctx *x, int dst, int sz)
{
    a64 *a = &x->a;
    ld_greg(x, RB, R_SP, 8, true);
    if (x->code64) {
        a64_mov(a, 1, REA, RB);
    } else {
        a64_ldr(a, 8, 10, RC, (uint32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * S_SS + offsetof(x86_seg, base)));
        a64_add(a, REA, RB, 10);
        a64_mov(a, 0, REA, REA);
    }
    emit_load(x, dst, sz);
    a64_add_imm(a, RB, RB, (uint32_t)sz);
    if (!x->code64)
        a64_mov(a, 0, RB, RB);
}

static void emit_jcc(jctx *x, jinsn *in, int cc, uint64_t target)
{
    a64 *a = &x->a;
    int cond = emit_cond(x, cc);
    if (cond == A_AL) {
        emit_exit_next(x, target, true);
        return;
    }
    if (cond == COND_NEVER) {
        emit_exit_next(x, in->next, true);
        return;
    }
    uint32_t *br = a64_here(a);
    if (cond == COND_W0)
        a64_cbnz(a, 0, 0, a64_here(a));
    else
        a64_bcond(a, cond, a64_here(a));
    emit_exit_next(x, in->next, true);
    a64_patch(br, a64_here(a));
    emit_exit_next(x, target, true);
}

static uint64_t branch_target(jctx *x, jinsn *in)
{
    uint64_t t = in->next + (uint64_t)in->imm;
    return x->code64 ? t : (uint32_t)t;
}

/* condicao em w-registrador booleano (para SETcc/CMOVcc): x0 = 0/1 */
static void cond_to_x0(jctx *x, int cc)
{
    a64 *a = &x->a;
    int cond = emit_cond(x, cc);
    if (cond == COND_W0)
        return;
    if (cond == A_AL)
        a64_mov_imm(a, 0, 1);
    else if (cond == COND_NEVER)
        a64_mov_imm(a, 0, 0);
    else
        a64_cset(a, 0, cond);
}

/* instrucao executada pelo interpretador; sai do bloco se o RIP nao for o esperado
 * (REP interrompido para interrupcoes, desvio) ou se invalidou codigo traduzido */
static void emit_icall(jctx *x, jinsn *in)
{
    a64 *a = &x->a;
    if (x->j->stats) {
        a64_mov_imm(a, 9, (uint64_t)(uintptr_t)&x->j->icall_op[(in->op & 0x3ff)]);
        a64_ldr(a, 8, 10, 9, 0);
        a64_add_imm(a, 10, 10, 1);
        a64_str(a, 8, 10, 9, 0);
    }
    a64_mov_imm(a, 9, in->rip);
    a64_str(a, 8, 9, RC, OFF(rip));
    a64_str(a, 8, 9, RC, OFF(cur_rip));
    a64_mov(a, 1, 0, RC);
    emit_call(x, (const void *)jit_interp1);
    a64_ldr(a, 8, 0, RC, OFF(rip));
    a64_mov_imm(a, 1, in->next);
    a64_cmp(a, 1, 0, 1);
    uint32_t *jsame = a64_here(a);
    a64_bcond(a, A_EQ, a64_here(a));
    emit_exit_lookup(x, 0, 0); /* RIP dinamico (em x0) */
    a64_patch(jsame, a64_here(a));
    x->stored = true;  /* pode ter escrito em codigo traduzido */
    x->flags_op = 0;   /* flags desconhecidos */
}

/* ---- SSE nativo ---- */

enum { SSE_NONE, SSE_LD128, SSE_ST128, SSE_ST128M, SSE_V3, SSE_LDS, SSE_STS, SSE_MOVQ_LD, SSE_MOVQ_ST, SSE_MOVD_LD, SSE_MOVD_ST };

/* classifica instrucoes SSE executadas em NEON/GP; *vop = operacao vetorial, *size = bytes escalares */
static int sse_kind(const jinsn *in, uint32_t *vop, int *size)
{
    if ((in->op & 0xf00) != 0x100)
        return SSE_NONE;
    int o = in->op & 0xff, pfx = in->rep ? 3 : in->repne ? 2 : in->p66 ? 1 : 0;
    uint32_t v = 0;
    int k = SSE_NONE, sz = 0;
    switch (o) {
    case 0x10: case 0x11:
        if (pfx <= 1) k = o == 0x10 ? SSE_LD128 : SSE_ST128;
        else { k = o == 0x10 ? SSE_LDS : SSE_STS; sz = pfx == 3 ? 4 : 8; }
        break;
    case 0x28: if (pfx <= 1) k = SSE_LD128; break;
    case 0x29: if (pfx <= 1) k = SSE_ST128; break;
    case 0x2b: if (pfx <= 1 && in->mem) k = SSE_ST128M; break;
    case 0xe7: if (pfx == 1 && in->mem) k = SSE_ST128M; break;
    case 0x6f: if (pfx == 1 || pfx == 3) k = SSE_LD128; break;
    case 0x7f: if (pfx == 1 || pfx == 3) k = SSE_ST128; break;
    case 0x7e:
        if (pfx == 3) k = SSE_MOVQ_LD;
        else if (pfx == 1) k = SSE_MOVD_ST;
        sz = (in->rex & 8) && pfx == 1 ? 8 : pfx == 3 ? 8 : 4;
        break;
    case 0x6e: if (pfx == 1) { k = SSE_MOVD_LD; sz = (in->rex & 8) ? 8 : 4; } break;
    case 0xd6: if (pfx == 1) k = SSE_MOVQ_ST; break;
    case 0x54: case 0x55: case 0x56: case 0x57:
        if (pfx <= 1) {
            k = SSE_V3;
            v = o == 0x54 ? A64_V_AND : o == 0x55 ? A64_V_BIC : o == 0x56 ? A64_V_ORR : A64_V_EOR;
        }
        break;
    default:
        if (pfx != 1)
            break;
        k = SSE_V3;
        switch (o) {
        case 0xdb: v = A64_V_AND; break;
        case 0xdf: v = A64_V_BIC; break;
        case 0xeb: v = A64_V_ORR; break;
        case 0xef: v = A64_V_EOR; break;
        case 0xfc: v = A64_V_ADD(0); break;
        case 0xfd: v = A64_V_ADD(1); break;
        case 0xfe: v = A64_V_ADD(2); break;
        case 0xd4: v = A64_V_ADD(3); break;
        case 0xf8: v = A64_V_SUB(0); break;
        case 0xf9: v = A64_V_SUB(1); break;
        case 0xfa: v = A64_V_SUB(2); break;
        case 0xfb: v = A64_V_SUB(3); break;
        case 0x74: v = A64_V_CMEQ(0); break;
        case 0x75: v = A64_V_CMEQ(1); break;
        case 0x76: v = A64_V_CMEQ(2); break;
        default: k = SSE_NONE; break;
        }
        break;
    }
    if (vop)
        *vop = v;
    if (size)
        *size = sz;
    return k;
}

#define OFF_X(i) ((uint32_t)(offsetof(x86_cpu, xmm) + 16 * (unsigned)(i)))

/* x12 = &c->xmm[i] */
static void xmm_addr(jctx *x, int i)
{
    uint32_t off = OFF_X(i);
    if (off < 4096) {
        a64_add_imm(&x->a, 12, RC, off);
    } else {
        a64_mov_imm(&x->a, 12, off);
        a64_add(&x->a, 12, RC, 12);
    }
}

/*
 * Emite a instrucao SSE com NEON/GP. Caminho lento (CR0.TS/EM, TLB, cruzamento de
 * pagina) fora de linha pelo interpretador. Retorna false se nao suportada.
 */
static void emit_icall(jctx *x, jinsn *in);
static bool emit_sse(jctx *x, jinsn *in)
{
    uint32_t vop;
    int sz;
    int k = sse_kind(in, &vop, &sz);
    if (k == SSE_NONE)
        return false;
    a64 *a = &x->a;
    uint32_t *slow[4];
    int ns = 0;
    a64_ldr(a, 8, 9, RC, OFF(cr0));
    a64_and_bitmask(a, 9, 9, (1u << 12) | (62u << 6) | 1u); /* TS | EM */
    slow[ns++] = a64_here(a);
    a64_cbnz(a, 1, 9, a64_here(a));
    x->nocache++; /* caminho rapido condicional: nao aloca registradores do cache */
    int rg = in->reg, rm = in->rm;
    bool mem = in->mem;
    uint32_t *s1, *s2;
#define SSE_TLB(bytes, tag)                                                                    \
    do {                                                                                       \
        emit_ea(x, in);                                                                        \
        emit_tlb(x, REA, bytes, (uint32_t)offsetof(x86_tlbe, tag), &s1, &s2);                  \
        slow[ns++] = s1;                                                                       \
        if (s2)                                                                                \
            slow[ns++] = s2;                                                                   \
    } while (0)
    switch (k) {
    case SSE_LD128:
    case SSE_V3:
        if (mem) {
            SSE_TLB(16, tag_r);
            a64_ldr_q_reg(a, 1, 10, REA);
        } else {
            xmm_addr(x, rm);
            a64_ldr_q(a, 1, 12);
        }
        xmm_addr(x, rg);
        if (k == SSE_V3) {
            a64_ldr_q(a, 0, 12);
            if (vop == A64_V_BIC)
                a64_v3(a, vop, 1, 1, 0); /* ~dst & src */
            else
                a64_v3(a, vop, 1, 0, 1);
        }
        a64_str_q(a, 1, 12);
        break;
    case SSE_ST128:
    case SSE_ST128M:
        xmm_addr(x, rg);
        a64_ldr_q(a, 0, 12);
        if (mem) {
            SSE_TLB(16, tag_w);
            a64_str_q_reg(a, 0, 10, REA);
            x->stored = true;
        } else {
            xmm_addr(x, rm);
            a64_str_q(a, 0, 12);
        }
        break;
    case SSE_LDS:     /* MOVSS/MOVSD xmm, xmm/m */
    case SSE_MOVQ_LD: /* MOVQ xmm, xmm/m64 (zera a parte alta) */
        if (mem) {
            SSE_TLB(sz, tag_r);
            a64_ldr_reg(a, sz, 11, 10, REA);
        } else {
            a64_ldr(a, sz, 11, RC, OFF_X(rm));
        }
        if (mem || k == SSE_MOVQ_LD) {
            a64_str(a, 8, 11, RC, OFF_X(rg));
            a64_str(a, 8, XZR, RC, OFF_X(rg) + 8);
        } else {
            a64_str(a, sz, 11, RC, OFF_X(rg));
        }
        break;
    /* stores: valor em x12 (emit_tlb usa x9-x11) */
    case SSE_STS:     /* MOVSS/MOVSD xmm/m, xmm */
    case SSE_MOVQ_ST: /* MOVQ xmm/m64, xmm (registro: zera a parte alta) */
        if (k == SSE_MOVQ_ST)
            sz = 8;
        a64_ldr(a, sz, 12, RC, OFF_X(rg));
        if (mem) {
            SSE_TLB(sz, tag_w);
            a64_str_reg(a, sz, 12, 10, REA);
            x->stored = true;
        } else {
            a64_str(a, sz, 12, RC, OFF_X(rm));
            if (k == SSE_MOVQ_ST)
                a64_str(a, 8, XZR, RC, OFF_X(rm) + 8);
        }
        break;
    case SSE_MOVD_LD: /* MOVD/MOVQ xmm, r/m */
        if (mem) {
            SSE_TLB(sz, tag_r);
            a64_ldr_reg(a, sz, 11, 10, REA);
        } else {
            ld_greg(x, 11, rm, sz, true);
        }
        a64_str(a, 8, 11, RC, OFF_X(rg));
        a64_str(a, 8, XZR, RC, OFF_X(rg) + 8);
        break;
    case SSE_MOVD_ST: /* MOVD/MOVQ r/m, xmm */
        a64_ldr(a, sz, 12, RC, OFF_X(rg));
        if (mem) {
            SSE_TLB(sz, tag_w);
            a64_str_reg(a, sz, 12, 10, REA);
            x->stored = true;
        } else {
            st_greg(x, 12, rm, sz, true);
        }
        break;
    }
#undef SSE_TLB
    x->nocache--;
    uint32_t *jdone = a64_here(a);
    a64_b(a, a64_here(a));
    for (int i = 0; i < ns; i++)
        a64_patch(slow[i], a64_here(a));
    int fop = x->flags_op, fsz = x->flags_size;
    x->slowpath++;
    emit_icall(x, in);
    x->slowpath--;
    x->flags_op = fop; /* SSE de movimento/logica nao altera flags */
    x->flags_size = fsz;
    a64_patch(jdone, a64_here(a));
    return true;
}

/* ---- MMX e operacoes inteiras SSE2 em NEON ----
 * O XP desenha o cursor (e o GDI mistura cores) com MMX: antes cada instrucao ia ao
 * interpretador. MMX = registradores D (64 bits) em c->mmx[]; SSE2 (66) = Q em c->xmm[]. */
enum { VK_NONE, VK_V3, VK_V3SWAP, VK_SHIFT, VK_PACK, VK_MOVQ_LD, VK_MOVQ_ST, VK_MOVD_LD, VK_MOVD_ST };

#define OFF_M(i) ((uint32_t)(offsetof(x86_cpu, mmx) + 8 * (unsigned)((i) & 7)))
#define VQ 0x40000000u /* bit Q: vetor de 128 bits */

static int vec_kind(const jinsn *in, uint32_t *vop, int *arg)
{
    if ((in->op & 0xf00) != 0x100 || in->rep || in->repne)
        return VK_NONE;
    int o = in->op & 0xff;
    bool x = in->p66;
    *arg = 0;
    if (!x) {
        switch (o) {
        case 0x6f: return VK_MOVQ_LD;
        case 0x7f: return VK_MOVQ_ST;
        case 0x6e: *arg = (in->rex & 8) ? 8 : 4; return VK_MOVD_LD;
        case 0x7e: *arg = (in->rex & 8) ? 8 : 4; return VK_MOVD_ST;
        default: break;
        }
    }
    uint32_t v;
    switch (o) {
    case 0xdb: v = 0x0E201C00u; break;                 /* pand */
    case 0xdf: *vop = 0x0E601C00u; return VK_V3SWAP;   /* pandn = src & ~dst */
    case 0xeb: v = 0x0EA01C00u; break;                 /* por */
    case 0xef: v = 0x2E201C00u; break;                 /* pxor */
    case 0xfc: case 0xfd: case 0xfe: v = 0x0E208400u | ((uint32_t)(o - 0xfc) << 22); break; /* padd b/w/d */
    case 0xd4: v = x ? 0x0E208400u | (3u << 22) : 0x5EE08400u; break; /* paddq (mm: .1D nao existe, D escalar) */
    case 0xf8: case 0xf9: case 0xfa: v = 0x2E208400u | ((uint32_t)(o - 0xf8) << 22); break; /* psub b/w/d */
    case 0xfb: v = x ? 0x2E208400u | (3u << 22) : 0x7EE08400u; break; /* psubq */
    case 0x74: case 0x75: case 0x76: v = 0x2E208C00u | ((uint32_t)(o - 0x74) << 22); break; /* pcmpeq */
    case 0x64: case 0x65: case 0x66: v = 0x0E203400u | ((uint32_t)(o - 0x64) << 22); break; /* pcmpgt */
    case 0xdc: case 0xdd: v = 0x2E200C00u | ((uint32_t)(o - 0xdc) << 22); break; /* paddus b/w */
    case 0xd8: case 0xd9: v = 0x2E202C00u | ((uint32_t)(o - 0xd8) << 22); break; /* psubus b/w */
    case 0xec: case 0xed: v = 0x0E200C00u | ((uint32_t)(o - 0xec) << 22); break; /* padds b/w */
    case 0xe8: case 0xe9: v = 0x0E202C00u | ((uint32_t)(o - 0xe8) << 22); break; /* psubs b/w */
    case 0xd5: v = 0x0E209C00u | (1u << 22); break;    /* pmullw */
    case 0x60: case 0x61: case 0x62: v = 0x0E003800u | ((uint32_t)(o - 0x60) << 22); break; /* punpckl */
    case 0x68: case 0x69: case 0x6a: v = 0x0E007800u | ((uint32_t)(o - 0x68) << 22); break; /* punpckh */
    case 0x6c: if (!x) return VK_NONE; v = 0x0E003800u | (3u << 22); break; /* punpcklqdq */
    case 0x6d: if (!x) return VK_NONE; v = 0x0E007800u | (3u << 22); break; /* punpckhqdq */
    case 0xda: v = 0x2E206C00u; break;                 /* pminub */
    case 0xde: v = 0x2E206400u; break;                 /* pmaxub */
    case 0xea: v = 0x0E206C00u | (1u << 22); break;    /* pminsw */
    case 0xee: v = 0x0E206400u | (1u << 22); break;    /* pmaxsw */
    case 0xe0: v = 0x2E201400u; break;                 /* pavgb */
    case 0xe3: v = 0x2E201400u | (1u << 22); break;    /* pavgw */
    case 0x63: *arg = 0; return VK_PACK;               /* packsswb */
    case 0x6b: *arg = 1; return VK_PACK;               /* packssdw */
    case 0x67: *arg = 2; return VK_PACK;               /* packuswb */
    case 0x71: case 0x72: case 0x73: {                 /* shifts por imediato (so registrador) */
        if (in->mem)
            return VK_NONE;
        int r = in->reg & 7;
        if (!(r == 2 || r == 6 || (r == 4 && o != 0x73)))
            return VK_NONE;
        *arg = ((o - 0x71 + 1) << 8) | (r << 4); /* log2(bytes do elemento) e operacao */
        return VK_SHIFT;
    }
    default: return VK_NONE;
    }
    *vop = v | (x ? VQ : 0);
    return VK_V3;
}

/* Vt = registrador MMX/XMM i (D ou Q) */
static void vec_ld(jctx *x, int vt, int i, bool q)
{
    if (q) {
        xmm_addr(x, i);
        a64_ldr_q(&x->a, vt, 12);
    } else {
        a64_put(&x->a, 0xFD400000u | ((OFF_M(i) / 8) << 10) | ((uint32_t)RC << 5) | (uint32_t)vt);
    }
}

static void vec_st(jctx *x, int vt, int i, bool q)
{
    if (q) {
        xmm_addr(x, i);
        a64_str_q(&x->a, vt, 12);
    } else {
        a64_put(&x->a, 0xFD000000u | ((OFF_M(i) / 8) << 10) | ((uint32_t)RC << 5) | (uint32_t)vt);
    }
}

static bool emit_vec(jctx *x, jinsn *in)
{
    uint32_t vop = 0;
    int arg;
    int k = vec_kind(in, &vop, &arg);
    if (k == VK_NONE || OFF_M(7) + 8 > 32760)
        return false;
    a64 *a = &x->a;
    bool q = in->p66;
    int bytes = q ? 16 : 8;
    uint32_t *slow[4];
    int ns = 0;
    a64_ldr(a, 8, 9, RC, OFF(cr0));
    a64_and_bitmask(a, 9, 9, (1u << 12) | (62u << 6) | 1u); /* TS | EM */
    slow[ns++] = a64_here(a);
    a64_cbnz(a, 1, 9, a64_here(a));
    x->nocache++;
    int rg = in->reg, rm = in->rm;
    bool mem = in->mem;
    uint32_t *s1, *s2;
#define VEC_TLB(n, tag)                                                                        \
    do {                                                                                       \
        emit_ea(x, in);                                                                        \
        emit_tlb(x, REA, n, (uint32_t)offsetof(x86_tlbe, tag), &s1, &s2);                      \
        slow[ns++] = s1;                                                                       \
        if (s2)                                                                                \
            slow[ns++] = s2;                                                                   \
    } while (0)
    /* fonte (r/m) em v1 */
#define VEC_SRC()                                                                              \
    do {                                                                                       \
        if (mem) {                                                                             \
            VEC_TLB(bytes, tag_r);                                                             \
            if (q)                                                                             \
                a64_ldr_q_reg(a, 1, 10, REA);                                                  \
            else                                                                               \
                a64_put(a, 0xFC606800u | ((uint32_t)REA << 16) | (10u << 5) | 1u);            \
        } else {                                                                               \
            vec_ld(x, 1, rm, q);                                                               \
        }                                                                                      \
    } while (0)
    switch (k) {
    case VK_V3:
    case VK_V3SWAP:
        VEC_SRC();
        vec_ld(x, 0, rg, q);
        if (k == VK_V3)
            a64_v3(a, vop | (q ? VQ : 0), 0, 0, 1);
        else
            a64_v3(a, vop | (q ? VQ : 0), 0, 1, 0);
        vec_st(x, 0, rg, q);
        break;
    case VK_PACK: {
        static const uint32_t narrow[3] = {0x0E214800u, 0x0E614800u, 0x2E212800u}; /* sqxtn.8b, sqxtn.4h, sqxtun.8b */
        VEC_SRC();
        vec_ld(x, 0, rg, q);
        if (q) {
            a64_put(a, narrow[arg] | (0u << 5) | 2u);       /* v2 = estreita(dst) (metade baixa) */
            a64_put(a, narrow[arg] | VQ | (1u << 5) | 2u);  /* v2 metade alta = estreita(src) */
            vec_st(x, 2, rg, q);
        } else {
            a64_put(a, 0x6E180400u | (1u << 5) | 0u);       /* v0.d[1] = v1.d[0] */
            a64_put(a, narrow[arg] | (0u << 5) | 0u);
            vec_st(x, 0, rg, q);
        }
        break;
    }
    case VK_SHIFT: {
        int lg = arg >> 8, op = (arg >> 4) & 15; /* lg: 1=16 2=32 3=64 bits */
        int es = 8 << lg, n = (int)(in->imm & 0xff);
        int r = rm & (q ? 15 : 7);
        vec_ld(x, 0, r, q);
        if (op == 4 && n >= es)
            n = es; /* psra: preenche com o sinal */
        if (n == 0) {
            /* nada */
        } else if (n >= es && op != 4) {
            a64_v3(a, 0x2E201C00u | (q ? VQ : 0), 0, 0, 0); /* eor: zera */
        } else if (lg == 3 && !q) {                         /* D escalar */
            if (op == 2)
                a64_put(a, 0x7F000400u | ((uint32_t)(128 - n) << 16));
            else
                a64_put(a, 0x5F005400u | ((uint32_t)(64 + n) << 16));
        } else if (op == 2) {
            a64_put(a, 0x2F000400u | (q ? VQ : 0) | ((uint32_t)(2 * es - n) << 16)); /* ushr */
        } else if (op == 4) {
            a64_put(a, 0x0F000400u | (q ? VQ : 0) | ((uint32_t)(2 * es - n) << 16)); /* sshr */
        } else {
            a64_put(a, 0x0F005400u | (q ? VQ : 0) | ((uint32_t)(es + n) << 16));     /* shl */
        }
        vec_st(x, 0, r, q);
        break;
    }
    case VK_MOVQ_LD: /* movq mm, mm/m64 */
        if (mem) {
            VEC_TLB(8, tag_r);
            a64_ldr_reg(a, 8, 11, 10, REA);
        } else {
            a64_ldr(a, 8, 11, RC, OFF_M(rm));
        }
        a64_str(a, 8, 11, RC, OFF_M(rg));
        break;
    case VK_MOVQ_ST: /* movq mm/m64, mm (valor em x12: emit_tlb usa x9-x11) */
        a64_ldr(a, 8, 12, RC, OFF_M(rg));
        if (mem) {
            VEC_TLB(8, tag_w);
            a64_str_reg(a, 8, 12, 10, REA);
            x->stored = true;
        } else {
            a64_str(a, 8, 12, RC, OFF_M(rm));
        }
        break;
    case VK_MOVD_LD: /* movd/movq mm, r/m */
        if (mem) {
            VEC_TLB(arg, tag_r);
            a64_ldr_reg(a, arg, 11, 10, REA);
        } else {
            ld_greg(x, 11, rm, arg, true);
        }
        a64_str(a, 8, 11, RC, OFF_M(rg));
        break;
    case VK_MOVD_ST: /* movd/movq r/m, mm */
        a64_ldr(a, arg, 12, RC, OFF_M(rg));
        if (mem) {
            VEC_TLB(arg, tag_w);
            a64_str_reg(a, arg, 12, 10, REA);
            x->stored = true;
        } else {
            st_greg(x, 12, rm, arg, true);
        }
        break;
    }
#undef VEC_SRC
#undef VEC_TLB
    x->nocache--;
    uint32_t *jdone = a64_here(a);
    a64_b(a, a64_here(a));
    for (int i = 0; i < ns; i++)
        a64_patch(slow[i], a64_here(a));
    int fop = x->flags_op, fsz = x->flags_size;
    x->slowpath++;
    emit_icall(x, in);
    x->slowpath--;
    x->flags_op = fop; /* MMX/SSE inteiro nao altera flags */
    x->flags_size = fsz;
    a64_patch(jdone, a64_here(a));
    return true;
}

/* ---- SSE de ponto flutuante, embaralhamentos e conversoes em NEON ----
 * O Linux (glibc/musl, GTK, cairo, OpenSSL) usa muito PSHUFD, PSRLDQ, PSHUFB, PALIGNR e
 * aritmetica SSE escalar; antes cada uma ia ao interpretador. Resultados NaN, conversoes
 * fora da faixa e comparacoes nao ordenadas vao ao interpretador, que emula exatamente a
 * propagacao de NaN e o "inteiro indefinido" do x86. */
enum { S2_NONE, S2_PSHUF, S2_BSHIFT, S2_PSHUFB, S2_PALIGNR, S2_MOVLH, S2_SHUFP, S2_UNPCKP, S2_FARITH, S2_COMIS,
       S2_PMOVMSKB, S2_CVTSI2F, S2_CVTTF2SI, S2_CVTF2F, S2_CVTDQ2PS };

static int simd2_kind(const jinsn *in)
{
    int op = in->op;
    int pfx = in->rep ? 3 : in->repne ? 2 : in->p66 ? 1 : 0;
    if (op == 0x200) /* PSHUFB mm/xmm */
        return pfx <= 1 ? S2_PSHUFB : S2_NONE;
    if (op == 0x30f) /* PALIGNR */
        return pfx <= 1 ? S2_PALIGNR : S2_NONE;
    if ((op & 0xf00) != 0x100)
        return S2_NONE;
    switch (op & 0xff) {
    case 0x70: return S2_PSHUF;
    case 0x73: return pfx == 1 && !in->mem && ((in->reg & 7) == 3 || (in->reg & 7) == 7) ? S2_BSHIFT : S2_NONE;
    case 0x12: case 0x16: return pfx == 0 || (pfx == 1 && in->mem) ? S2_MOVLH : S2_NONE;
    case 0x13: case 0x17: return pfx <= 1 && in->mem ? S2_MOVLH : S2_NONE;
    case 0xc6: return pfx <= 1 ? S2_SHUFP : S2_NONE;
    case 0x14: case 0x15: return pfx <= 1 ? S2_UNPCKP : S2_NONE;
    case 0x51: case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f: return S2_FARITH;
    case 0x2e: case 0x2f: return pfx <= 1 ? S2_COMIS : S2_NONE;
    case 0xd7: return pfx <= 1 && !in->mem ? S2_PMOVMSKB : S2_NONE;
    case 0x2a: return pfx >= 2 ? S2_CVTSI2F : S2_NONE;
    case 0x2c: return pfx >= 2 ? S2_CVTTF2SI : S2_NONE;
    case 0x5a: return pfx >= 2 ? S2_CVTF2F : S2_NONE;
    case 0x5b: return pfx == 0 ? S2_CVTDQ2PS : S2_NONE;
    default: return S2_NONE;
    }
}

/* LDR/STR St/Dt, [x12] (registrador XMM em memoria) e [x10, REA] (memoria do convidado) */
static void fp_ld_x12(a64 *a, int vt, int sz) { a64_put(a, (sz == 4 ? 0xBD400000u : 0xFD400000u) | (12u << 5) | (uint32_t)vt); }
static void fp_st_x12(a64 *a, int vt, int sz) { a64_put(a, (sz == 4 ? 0xBD000000u : 0xFD000000u) | (12u << 5) | (uint32_t)vt); }
static void fp_ld_mem(a64 *a, int vt, int sz)
{
    a64_put(a, (sz == 4 ? 0xBC606800u : 0xFC606800u) | ((uint32_t)REA << 16) | (10u << 5) | (uint32_t)vt);
}
/* INS Vd.T[i], Vn.T[j] (es = bytes do elemento) */
static void a64_ins(a64 *a, int vd, int i, int vn, int j, int es)
{
    int lg = es == 1 ? 0 : es == 2 ? 1 : es == 4 ? 2 : 3;
    uint32_t imm5 = ((uint32_t)i << (lg + 1)) | (1u << lg), imm4 = (uint32_t)j << lg;
    a64_put(a, 0x6E000400u | (imm5 << 16) | (imm4 << 11) | ((uint32_t)vn << 5) | (uint32_t)vd);
}
static void v_zero(a64 *a, int vd) { a64_v3(a, 0x6E201C00u, vd, vd, vd); }       /* eor .16b */
static void v_mov(a64 *a, int vd, int vn) { a64_v3(a, 0x4EA01C00u, vd, vn, vn); } /* orr .16b */
static void v_ext(a64 *a, int vd, int vn, int vm, int n) { a64_put(a, 0x6E000000u | ((uint32_t)vm << 16) | ((uint32_t)n << 11) | ((uint32_t)vn << 5) | (uint32_t)vd); }

volatile long x86_jit_s2_off; /* depuracao (CLI "jits2 N"): bit k desliga o tipo S2_k */

static bool emit_simd2(jctx *x, jinsn *in)
{
    int k = simd2_kind(in);
    if (k == S2_NONE || OFF_M(7) + 8 > 32760 || ((x86_jit_s2_off >> k) & 1))
        return false;
    a64 *a = &x->a;
    int op = in->op & 0xff, pfx = in->rep ? 3 : in->repne ? 2 : in->p66 ? 1 : 0;
    bool q = pfx == 1 || k == S2_FARITH || k == S2_COMIS || k == S2_MOVLH || k == S2_SHUFP || k == S2_UNPCKP ||
             k == S2_CVTSI2F || k == S2_CVTTF2SI || k == S2_CVTF2F || k == S2_CVTDQ2PS ||
             (k == S2_PSHUF && pfx >= 2);
    int rg = in->reg, rm = in->rm;
    if (!q) { /* MMX */
        rg &= 7;
        rm &= 7;
    }
    bool mem = in->mem;
    int imm = (int)(in->imm & 0xff);
    uint32_t *slow[8];
    int ns = 0;
    a64_ldr(a, 8, 9, RC, OFF(cr0));
    a64_and_bitmask(a, 9, 9, (1u << 12) | (62u << 6) | 1u); /* TS | EM */
    slow[ns++] = a64_here(a);
    a64_cbnz(a, 1, 9, a64_here(a));
    x->nocache++;
    uint32_t *s1, *s2;
#define S2_TLB(n, tag)                                                                         \
    do {                                                                                       \
        emit_ea(x, in);                                                                        \
        emit_tlb(x, REA, n, (uint32_t)offsetof(x86_tlbe, tag), &s1, &s2);                      \
        slow[ns++] = s1;                                                                       \
        if (s2)                                                                                \
            slow[ns++] = s2;                                                                   \
    } while (0)
    /* fonte vetorial completa (16 ou 8 bytes) em v1 */
#define S2_SRC()                                                                               \
    do {                                                                                       \
        if (mem) {                                                                             \
            S2_TLB(q ? 16 : 8, tag_r);                                                         \
            if (q)                                                                             \
                a64_ldr_q_reg(a, 1, 10, REA);                                                  \
            else                                                                               \
                fp_ld_mem(a, 1, 8);                                                            \
        } else {                                                                               \
            vec_ld(x, 1, rm, q);                                                               \
        }                                                                                      \
    } while (0)
    /* fonte escalar (sz bytes) em v1 */
#define S2_SRCS(sz)                                                                            \
    do {                                                                                       \
        if (mem) {                                                                             \
            S2_TLB(sz, tag_r);                                                                 \
            fp_ld_mem(a, 1, sz);                                                               \
        } else {                                                                               \
            xmm_addr(x, rm);                                                                   \
            fp_ld_x12(a, 1, sz);                                                               \
        }                                                                                      \
    } while (0)
#define S2_SLOW_IF(cond)                                                                       \
    do {                                                                                       \
        slow[ns++] = a64_here(a);                                                              \
        a64_bcond(a, cond, a64_here(a));                                                       \
    } while (0)
    bool sets_flags = false;
    switch (k) {
    case S2_PSHUF:
        S2_SRC();
        if (pfx == 1 || pfx == 0) { /* PSHUFD (dwords) / PSHUFW (words do MMX) */
            int es = pfx == 1 ? 4 : 2;
            for (int i = 0; i < 4; i++)
                a64_ins(a, 0, i, 1, (imm >> (2 * i)) & 3, es);
        } else { /* PSHUFLW (F2) / PSHUFHW (F3): so 4 words, o resto copiado */
            int base = pfx == 2 ? 0 : 4;
            v_mov(a, 0, 1);
            for (int i = 0; i < 4; i++)
                a64_ins(a, 0, base + i, 1, base + ((imm >> (2 * i)) & 3), 2);
        }
        vec_st(x, 0, rg, q);
        break;
    case S2_BSHIFT: /* PSRLDQ (/3) e PSLLDQ (/7) em bytes; destino em r/m */
        vec_ld(x, 1, rm, true);
        v_zero(a, 2);
        if (imm > 15)
            v_zero(a, 0);
        else if (imm == 0)
            v_mov(a, 0, 1);
        else if ((in->reg & 7) == 3)
            v_ext(a, 0, 1, 2, imm);
        else
            v_ext(a, 0, 2, 1, 16 - imm);
        vec_st(x, 0, rm, true);
        break;
    case S2_PSHUFB: /* indice com bit 7 = 0; TBL da 0 para indices fora da tabela */
        S2_SRC();
        vec_ld(x, 0, rg, q);
        if (q) {
            a64_put(a, 0x4F04E400u | (0x0Fu << 5) | 2u); /* movi v2.16b, #0x8f */
            a64_v3(a, 0x4E201C00u, 2, 1, 2);              /* and v2.16b, v1, v2 */
            a64_put(a, 0x4E000000u | (2u << 16) | (0u << 5) | 3u); /* tbl v3.16b, {v0.16b}, v2.16b */
        } else {
            a64_put(a, 0x0F04E400u | (0x07u << 5) | 2u);  /* movi v2.8b, #0x87 */
            a64_v3(a, 0x0E201C00u, 2, 1, 2);
            a64_put(a, 0x0E000000u | (2u << 16) | (0u << 5) | 3u); /* tbl v3.8b, {v0.16b}, v2.8b */
        }
        vec_st(x, 3, rg, q);
        break;
    case S2_PALIGNR: /* (dst:src) >> imm bytes */
        S2_SRC();
        vec_ld(x, 0, rg, q);
        v_zero(a, 2);
        if (q) {
            if (imm < 16)
                v_ext(a, 3, 1, 0, imm);
            else if (imm < 32)
                v_ext(a, 3, 0, 2, imm - 16);
            else
                v_zero(a, 3);
        } else {
            a64_ins(a, 1, 1, 0, 0, 8); /* v1 = src (baixo) : dst (alto) */
            if (imm < 16)
                v_ext(a, 3, 1, 2, imm);
            else
                v_zero(a, 3);
        }
        vec_st(x, 3, rg, q);
        break;
    case S2_MOVLH: {
        bool hi = op == 0x16 || op == 0x17;
        if (op == 0x13 || op == 0x17) { /* MOVLPS/MOVHPS m64, xmm (valor em x12) */
            a64_ldr(a, 8, 12, RC, OFF_X(rg) + (hi ? 8 : 0));
            S2_TLB(8, tag_w);
            a64_str_reg(a, 8, 12, 10, REA);
            x->stored = true;
        } else {
            if (mem) {
                S2_TLB(8, tag_r);
                a64_ldr_reg(a, 8, 11, 10, REA);
            } else { /* MOVHLPS (12) / MOVLHPS (16) */
                a64_ldr(a, 8, 11, RC, OFF_X(rm) + (hi ? 0 : 8));
            }
            a64_str(a, 8, 11, RC, OFF_X(rg) + (hi ? 8 : 0));
        }
        break;
    }
    case S2_SHUFP:
        S2_SRC();
        vec_ld(x, 0, rg, true);
        if (pfx == 0) {
            a64_ins(a, 2, 0, 0, imm & 3, 4);
            a64_ins(a, 2, 1, 0, (imm >> 2) & 3, 4);
            a64_ins(a, 2, 2, 1, (imm >> 4) & 3, 4);
            a64_ins(a, 2, 3, 1, (imm >> 6) & 3, 4);
        } else {
            a64_ins(a, 2, 0, 0, imm & 1, 8);
            a64_ins(a, 2, 1, 1, (imm >> 1) & 1, 8);
        }
        vec_st(x, 2, rg, true);
        break;
    case S2_UNPCKP: { /* ZIP1/ZIP2 .4S (ps) ou .2D (pd) */
        S2_SRC();
        vec_ld(x, 0, rg, true);
        uint32_t zip = (op == 0x14 ? 0x4E803800u : 0x4E807800u) | (pfx == 1 ? (1u << 22) : 0);
        a64_v3(a, zip, 2, 0, 1);
        vec_st(x, 2, rg, true);
        break;
    }
    case S2_FARITH: {
        bool dbl = pfx == 1 || pfx == 2, scalar = pfx >= 2;
        int sz = dbl ? 8 : 4;
        bool minmax = op == 0x5d || op == 0x5f;
        uint32_t dbit = dbl ? (1u << 22) : 0;
        if (scalar) {
            S2_SRCS(sz);
            xmm_addr(x, rg);
            fp_ld_x12(a, 0, sz);
            if (minmax) {
                /* MIN: a < b ? a : b; MAX: a > b ? a : b (NaN ou iguais: b) */
                if (op == 0x5d)
                    a64_v3(a, 0x7EA0E400u | dbit, 3, 1, 0);
                else
                    a64_v3(a, 0x7EA0E400u | dbit, 3, 0, 1);
                a64_v3(a, 0x2E601C00u, 3, 0, 1); /* bsl v3.8b */
                fp_st_x12(a, 3, sz);
                break;
            }
            uint32_t base;
            switch (op) {
            case 0x58: base = 0x1E202800u; break;
            case 0x59: base = 0x1E200800u; break;
            case 0x5c: base = 0x1E203800u; break;
            case 0x5e: base = 0x1E201800u; break;
            default: base = 0x1E21C000u; break; /* sqrt */
            }
            if (op == 0x51)
                a64_put(a, base | dbit | (1u << 5) | 2u);
            else
                a64_v3(a, base | dbit, 2, 0, 1);
            a64_put(a, 0x1E202000u | dbit | (2u << 16) | (2u << 5)); /* fcmp v2, v2: NaN -> interpretador */
            S2_SLOW_IF(A_VS);
            fp_st_x12(a, 2, sz);
            break;
        }
        S2_SRC();
        vec_ld(x, 0, rg, true);
        if (minmax) {
            if (op == 0x5d)
                a64_v3(a, 0x6EA0E400u | dbit, 3, 1, 0);
            else
                a64_v3(a, 0x6EA0E400u | dbit, 3, 0, 1);
            a64_v3(a, 0x6E601C00u, 3, 0, 1); /* bsl v3.16b */
            vec_st(x, 3, rg, true);
            break;
        }
        switch (op) {
        case 0x58: a64_v3(a, 0x4E20D400u | dbit, 2, 0, 1); break;
        case 0x59: a64_v3(a, 0x6E20DC00u | dbit, 2, 0, 1); break;
        case 0x5c: a64_v3(a, 0x4EA0D400u | dbit, 2, 0, 1); break;
        case 0x5e: a64_v3(a, 0x6E20FC00u | dbit, 2, 0, 1); break;
        default: a64_put(a, 0x6EA1F800u | dbit | (1u << 5) | 2u); break; /* fsqrt */
        }
        /* algum elemento NaN -> interpretador */
        a64_v3(a, 0x4E20E400u | dbit, 3, 2, 2); /* fcmeq v3, v2, v2 */
        a64_put(a, 0x6EB1A800u | (3u << 5) | 3u); /* uminv s3, v3.4s */
        a64_put(a, 0x1E260000u | (3u << 5) | 9u);  /* fmov w9, s3 */
        slow[ns++] = a64_here(a);
        a64_cbz(a, 0, 9, a64_here(a));
        vec_st(x, 2, rg, true);
        break;
    }
    case S2_COMIS: {
        int sz = pfx ? 8 : 4;
        uint32_t dbit = pfx ? (1u << 22) : 0;
        S2_SRCS(sz);
        xmm_addr(x, rg);
        fp_ld_x12(a, 0, sz);
        a64_put(a, 0x1E202000u | dbit | (1u << 16) | (0u << 5)); /* fcmp v0, v1 */
        S2_SLOW_IF(A_VS);                                        /* nao ordenado: interpretador */
        a64_cset(a, 9, A_MI);                                    /* CF = a < b */
        a64_cset(a, 10, A_EQ);                                   /* ZF = a == b */
        a64_logic_sh(a, 1, 1, 9, 9, 10, 0, 6);                   /* orr x9, x9, x10, lsl #6 */
        a64_ldr(a, 8, 10, RC, OFF(eflags));
        a64_mov_imm(a, 11, ~(uint64_t)EFL_ARITH);
        a64_logic(a, 1, 0, 0, 10, 10, 11);                       /* and */
        a64_logic(a, 1, 1, 0, 10, 10, 9);                        /* orr */
        a64_str(a, 8, 10, RC, OFF(eflags));
        a64_str(a, 8, XZR, RC, OFF(cc_op));                      /* CC_NONE */
        x->ccop_mem = -1; /* so no caminho rapido */
        sets_flags = true;
        break;
    }
    case S2_PMOVMSKB: /* bit 7 de cada byte -> registrador */
        vec_ld(x, 1, rm, q);
        a64_put(a, 0x6F090400u | (1u << 5) | 1u); /* ushr v1.16b, v1.16b, #7 */
        a64_put(a, 0x6F191400u | (1u << 5) | 1u); /* usra v1.8h, v1.8h, #7 */
        a64_put(a, 0x6F321400u | (1u << 5) | 1u); /* usra v1.4s, v1.4s, #14 */
        a64_put(a, 0x6F641400u | (1u << 5) | 1u); /* usra v1.2d, v1.2d, #28 */
        a64_put(a, 0x0E013C00u | (1u << 5) | 9u); /* umov w9, v1.b[0] */
        if (q) {
            a64_put(a, 0x0E113C00u | (1u << 5) | 10u); /* umov w10, v1.b[8] */
            a64_logic_sh(a, 0, 1, 9, 9, 10, 0, 8);
        }
        st_greg(x, 9, in->reg, 4, true);
        break;
    case S2_CVTSI2F: { /* CVTSI2SS/SD xmm, r/m32/64 */
        int isz = (in->rex & 8) ? 8 : 4;
        if (mem) {
            S2_TLB(isz, tag_r);
            a64_ldr_reg(a, isz, 11, 10, REA);
        } else {
            ld_greg(x, 11, rm, isz, true);
        }
        uint32_t cv = (isz == 8 ? 0x9E220000u : 0x1E220000u) | (pfx == 2 ? (1u << 22) : 0);
        a64_put(a, cv | (11u << 5) | 2u); /* scvtf */
        xmm_addr(x, rg);
        fp_st_x12(a, 2, pfx == 2 ? 8 : 4);
        break;
    }
    case S2_CVTTF2SI: { /* CVTTSS2SI/CVTTSD2SI r32/64, xmm/m */
        int sz = pfx == 2 ? 8 : 4, isz = (in->rex & 8) ? 8 : 4;
        uint32_t dbit = pfx == 2 ? (1u << 22) : 0;
        S2_SRCS(sz);
        a64_put(a, 0x1E202000u | dbit | (1u << 16) | (1u << 5)); /* fcmp v1, v1 */
        S2_SLOW_IF(A_VS);
        a64_put(a, (isz == 8 ? 0x9E380000u : 0x1E380000u) | dbit | (1u << 5) | 9u); /* fcvtzs */
        /* saturou (fora da faixa): o x86 da 0x80..0; o interpretador resolve */
        a64_mov_imm(a, 10, isz == 8 ? 0x7fffffffffffffffULL : 0x7fffffffULL);
        a64_cmp(a, isz == 8, 9, 10);
        S2_SLOW_IF(A_EQ);
        a64_mov_imm(a, 10, isz == 8 ? 0x8000000000000000ULL : 0x80000000ULL);
        a64_cmp(a, isz == 8, 9, 10);
        S2_SLOW_IF(A_EQ);
        st_greg(x, 9, in->reg, isz, true);
        break;
    }
    case S2_CVTF2F: /* CVTSS2SD (F3) / CVTSD2SS (F2) */
        S2_SRCS(pfx == 3 ? 4 : 8);
        a64_put(a, (pfx == 3 ? 0x1E22C000u : 0x1E624000u) | (1u << 5) | 2u);
        xmm_addr(x, rg);
        fp_st_x12(a, 2, pfx == 3 ? 8 : 4);
        break;
    case S2_CVTDQ2PS:
        S2_SRC();
        a64_put(a, 0x4E21D800u | (1u << 5) | 2u); /* scvtf v2.4s, v1.4s */
        vec_st(x, 2, rg, true);
        break;
    }
#undef S2_SLOW_IF
#undef S2_SRCS
#undef S2_SRC
#undef S2_TLB
    x->nocache--;
    uint32_t *jdone = a64_here(a);
    a64_b(a, a64_here(a));
    for (int i = 0; i < ns; i++)
        a64_patch(slow[i], a64_here(a));
    int fop = x->flags_op, fsz = x->flags_size;
    x->slowpath++;
    emit_icall(x, in);
    x->slowpath--;
    a64_patch(jdone, a64_here(a));
    if (sets_flags) {
        x->flags_op = JF_EFL; /* nos dois caminhos: CC_NONE e flags em c->eflags */
    } else {
        x->flags_op = fop;
        x->flags_size = fsz;
    }
    return true;
}

/* ---- instrucoes "icall" feitas em codigo nativo ---- */

/* CF = cf, demais flags aritmeticos preservados */
static void jit_set_cf(x86_cpu *c, uint64_t cf)
{
    x86_set_arith_flags(c, (x86_arith_flags(c) & ~(uint64_t)EFL_CF) | (cf ? EFL_CF : 0));
}

/* ZF = zf, demais flags aritmeticos preservados (BSF/BSR) */
static void jit_set_zf(x86_cpu *c, uint64_t zf)
{
    x86_set_arith_flags(c, (x86_arith_flags(c) & ~(uint64_t)EFL_ZF) | (zf ? EFL_ZF : 0));
}

/* 0 = nao suportada; 1 = barreira (LFENCE/MFENCE/SFENCE); 2 = BT*; 3 = CMPXCHG; 4 = XADD; 5 = BSF/BSR */
static int native_icall_kind(const jinsn *in)
{
    if (!in->rep && !in->repne) {
        if (in->op == 0x1b0 || in->op == 0x1b1)
            return 3;
        if (in->op == 0x1c0 || in->op == 0x1c1)
            return 4;
        if ((in->op == 0x1bc || in->op == 0x1bd) && in->osz >= 2)
            return 5;
    }
    if (in->op == 0x1ae && in->mod == 3 && (in->reg & 7) >= 5)
        return 1;
    if (in->op == 0x1ba && (in->reg & 7) >= 4)
        return 2;
    if ((in->op == 0x1a3 || in->op == 0x1ab || in->op == 0x1b3 || in->op == 0x1bb) && !in->mem)
        return 2;
    return 0;
}

/* CMPXCHG r/m, r: compara rAX com o destino (flags de CMP); igual -> destino = fonte,
 * diferente -> rAX = destino. Em memoria a escrita (valor novo ou o antigo) vem antes
 * de alterar rAX, como no interpretador: a instrucao pode ser reiniciada apos #PF. */
static void emit_cmpxchg(jctx *x, jinsn *in)
{
    a64 *a = &x->a;
    int s = in->op == 0x1b0 ? 1 : in->osz;
    load_rm(x, in, RA, s);
    ld_greg(x, RB, R_AX, s, false);
    ld_greg(x, RT, in->reg, s, rexf(in));
    a64_cmp(a, 1, RB, RA);
    if (in->mem) {
        a64_csel(a, 1, RT, RT, RA, A_EQ);
        store_rm(x, in, RT, s);
    } else {
        uint32_t *jne = a64_here(a);
        a64_bcond(a, A_NE, a64_here(a));
        x->nocache++;
        store_rm(x, in, RT, s);
        x->nocache--;
        a64_patch(jne, a64_here(a));
    }
    a64_cmp(a, 1, RB, RA);
    uint32_t *jeq = a64_here(a);
    a64_bcond(a, A_EQ, a64_here(a));
    x->nocache++;
    st_greg(x, RA, R_AX, s, false);
    x->nocache--;
    a64_patch(jeq, a64_here(a));
    a64_sub(a, RR, RB, RA);
    emit_lazy(x, CC_SUB, s, RR, RB, RA);
}

/* XADD r/m, r: destino = destino + fonte, fonte = destino antigo (flags de ADD) */
static void emit_xadd(jctx *x, jinsn *in)
{
    a64 *a = &x->a;
    int s = in->op == 0x1c0 ? 1 : in->osz;
    load_rm(x, in, RA, s);
    ld_greg(x, RB, in->reg, s, rexf(in));
    a64_add(a, RR, RA, RB);
    if (in->mem) { /* grava a memoria antes de alterar o registrador fonte */
        store_rm(x, in, RR, s);
        st_greg(x, RA, in->reg, s, rexf(in));
    } else {
        st_greg(x, RA, in->reg, s, rexf(in));
        store_rm(x, in, RR, s);
    }
    emit_lazy(x, CC_ADD, s, RR, RA, RB);
}

/* BSF/BSR: fonte 0 -> ZF = 1 e destino intacto; senao ZF = 0 e destino = indice */
static void emit_bsf_bsr(jctx *x, jinsn *in)
{
    a64 *a = &x->a;
    int sz = in->osz;
    load_rm(x, in, RA, sz);
    uint32_t *jz = a64_here(a);
    a64_cbz(a, 1, RA, a64_here(a));
    if (in->op == 0x1bc) {
        a64_rbit(a, 9, RA);
        a64_clz(a, RR, 9);
    } else {
        a64_clz(a, 9, RA);
        a64_mov_imm(a, RR, 63);
        a64_sub(a, RR, RR, 9);
    }
    x->nocache++;
    st_greg(x, RR, in->reg, sz, rexf(in));
    x->nocache--;
    a64_mov_imm(a, RT, 0);
    uint32_t *jdone = a64_here(a);
    a64_b(a, a64_here(a));
    a64_patch(jz, a64_here(a));
    a64_mov_imm(a, RT, 1);
    a64_patch(jdone, a64_here(a));
    if (!x->dead) {
        a64_mov(a, 1, 0, RC);
        a64_mov(a, 1, 1, RT);
        emit_call(x, (const void *)jit_set_zf);
    }
    x->flags_op = 0;
}

static void emit_bt(jctx *x, jinsn *in)
{
    a64 *a = &x->a;
    int sz = in->osz, bits = sz * 8;
    int kind = in->op == 0x1ba ? (in->reg & 7) - 4 : in->op == 0x1a3 ? 0 : in->op == 0x1ab ? 1 : in->op == 0x1b3 ? 2 : 3;
    load_rm(x, in, RA, sz);
    if (in->op == 0x1ba) {
        int b = (int)((uint64_t)in->imm & (uint64_t)(bits - 1));
        a64_ubfx(a, RT, RA, b, 1);
        a64_mov_imm(a, RB, 1ULL << b);
    } else {
        ld_greg(x, RB, in->reg, sz, rexf(in));
        a64_and_bitmask(a, RB, RB, (1u << 12) | (sz == 8 ? 5u : sz == 4 ? 4u : 3u)); /* & (bits - 1) */
        a64_shiftv(a, 1, RT, RA, RB, 1);
        a64_and_bitmask(a, RT, RT, (1u << 12) | 0u);                                  /* & 1 */
        a64_mov_imm(a, 9, 1);
        a64_shiftv(a, 1, RB, 9, RB, 0);
    }
    if (kind) {
        if (kind == 1)
            a64_logic(a, 1, 1, 0, RR, RA, RB);  /* BTS: or */
        else if (kind == 2)
            a64_logic(a, 1, 0, 1, RR, RA, RB);  /* BTR: and not */
        else
            a64_logic(a, 1, 2, 0, RR, RA, RB);  /* BTC: xor */
        store_rm(x, in, RR, sz);
    }
    if (!x->dead) {
        a64_mov(a, 1, 0, RC);
        a64_mov(a, 1, 1, RT);
        emit_call(x, (const void *)jit_set_cf);
    }
    x->flags_op = 0;
}

/* contagem constante de C0/C1/D0/D1 ja mascarada; -1 = CL (D3) */
static int shift_count(jinsn *in, int s)
{
    if (in->op == 0xd2 || in->op == 0xd3)
        return -1;
    unsigned n = in->op >= 0xd0 ? 1 : (unsigned)in->imm & 0xff;
    return (int)(n & (s == 8 ? 63u : 31u));
}

/*
 * SHL/SHR/SAR nativos com RA ja carregado (estendido com zero). Contagem em CL so
 * com flags mortos (contagem 0 preserva os flags); contagem constante gera os flags
 * preguicosos CC_SZP. Retorna false para usar o helper.
 */
static bool emit_shift_native(jctx *x, jinsn *in, int s)
{
    a64 *a = &x->a;
    int r = in->reg & 7, bits = s * 8;
    if (r < 2 && x->dead && s >= 4 && !(x->j->off & 4)) { /* ROL/ROR com flags mortos: so o resultado */
        int sf = s == 8;
        int cnt = shift_count(in, s);
        if (cnt == 0)
            return false;
        if (cnt > 0) {
            int n = cnt % bits;
            a64_ror_imm(a, sf, RR, RA, r == 0 ? (bits - n) % bits : n);
        } else {
            ld_greg(x, RB, R_CX, 1, false);
            if (r == 0)
                a64_addsub_reg(a, 1, 1, 0, RB, XZR, RB, 0); /* ROL n = ROR -n */
            a64_shiftv(a, sf, RR, RA, RB, 3);
        }
        store_rm(x, in, RR, s);
        x->flags_op = 0;
        return true;
    }
    if (r < 4)
        return false;
    int type = r == 5 ? 1 : r == 7 ? 2 : 0;
    int cnt = shift_count(in, s);
    if (cnt == 0)
        return false;
    if (cnt < 0 || x->dead) {
        if (cnt < 0 && !x->dead)
            return false;
        int src = RA;
        if (type == 2) {
            a64_sext(a, RT, RA, s);
            src = RT;
        }
        if (cnt < 0) {
            ld_greg(x, RB, R_CX, 1, false);
            a64_and_bitmask(a, RB, RB, (1u << 12) | (0u << 6) | (s == 8 ? 5u : 4u)); /* & 63 / & 31 */
            a64_shiftv(a, 1, RR, src, RB, type);
        } else if (type == 0) {
            a64_lsl_imm(a, RR, src, cnt);
        } else if (type == 1) {
            a64_lsr_imm(a, RR, src, cnt);
        } else {
            a64_asr_imm(a, RR, src, cnt);
        }
        store_rm(x, in, RR, s);
        x->flags_op = 0;
        return true;
    }
    if (cnt >= bits)
        return false;
    /* RR = resultado, RT = CF | OF << 1 */
    if (type == 0) {
        a64_lsl_imm(a, RR, RA, cnt);
        a64_ubfx(a, RT, RA, bits - cnt, 1);          /* CF = ultimo bit expulso */
        a64_ubfx(a, RB, RR, bits - 1, 1);            /* OF = MSB(r) ^ CF */
        a64_logic(a, 1, 2, 0, RB, RB, RT);
    } else if (type == 1) {
        a64_lsr_imm(a, RR, RA, cnt);
        a64_ubfx(a, RT, RA, cnt - 1, 1);
        a64_ubfx(a, RB, RA, bits - 1, 1);            /* OF = MSB(original) */
    } else {
        a64_sext(a, RB, RA, s);
        a64_asr_imm(a, RR, RB, cnt);
        a64_ubfx(a, RT, RB, cnt - 1, 1);
        a64_mov(a, 1, RB, XZR);                      /* OF = 0 */
    }
    a64_logic_sh(a, 1, 1, RT, RT, RB, 0, 1);         /* RT |= OF << 1 */
    store_rm(x, in, RR, s);
    a64_mov_imm(a, 9, (uint64_t)CC_SZP | ((uint64_t)(uint32_t)s << 32));
    a64_str(a, 8, 9, RC, OFF(cc_op));
    a64_str(a, 8, RR, RC, OFF(cc_dst));
    a64_str(a, 8, RT, RC, OFF(cc_aux));
    x->ccop_mem = (int64_t)((uint64_t)CC_SZP | ((uint64_t)(uint32_t)s << 32));
    x->flags_op = CC_SZP;
    x->flags_size = s;
    return true;
}

/* INC/DEC de RA (ja carregado; REA valido se for memoria): CF preservado em cc_aux */
static void emit_incdec(jctx *x, jinsn *in, int dec, int s)
{
    a64 *a = &x->a;
    if (!x->dead)
        emit_carry(x);
    a64_mov_imm(a, RB, 1);
    if (!dec)
        a64_add(a, RR, RA, RB);
    else
        a64_sub(a, RR, RA, RB);
    if (in)
        store_rm(x, in, RR, s);
    if (!x->dead)
        a64_str(a, 8, RT, RC, OFF(cc_aux)); /* depois da escrita: a instrucao continua reiniciavel */
    emit_lazy(x, dec ? CC_DEC : CC_INC, s, RR, RA, RB);
}

/*
 * Emite uma instrucao. Retorna 1 se o bloco continua, 0 se a instrucao
 * terminou o bloco (desvio).
 */
static int emit_insn(jctx *x, jinsn *in)
{
    a64 *a = &x->a;
    int op = in->op, sz = op_size(in);
    bool rex = rexf(in);
    if (in->icall && (emit_sse(x, in) || (!(x->j->off & 64) && emit_vec(x, in)) ||
                      (!(x->j->off & 128) && emit_simd2(x, in))))
        return 1;
    if (in->icall) {
        int nk = native_icall_kind(in);
        if ((nk >= 2 && (x->j->off & 2)) || (nk == 1 && (x->j->off & 8)))
            nk = 0;
        if (nk == 1)
            return 1; /* barreiras: uma vCPU, sem efeito */
        if (nk >= 2) {
            if (in->mem)
                set_cur_rip(x, in); /* a falta de pagina precisa apontar para esta instrucao */
            if (nk == 2)
                emit_bt(x, in);
            else if (nk == 3)
                emit_cmpxchg(x, in);
            else if (nk == 4)
                emit_xadd(x, in);
            else
                emit_bsf_bsr(x, in);
            return 1;
        }
    }
    if (in->icall && in->op >= 0xd8 && in->op <= 0xdf && !(x->j->off & 4096)) {
        /* x87 sem passar pela decodificacao do interpretador */
        set_cur_rip(x, in);
        if (in->mem)
            emit_ea(x, in);
        uint64_t modrm = ((uint64_t)in->mod << 6) | ((uint64_t)(in->reg & 7) << 3) | (uint64_t)(in->rm & 7);
        a64_mov(a, 1, 0, RC);
        a64_mov_imm(a, 1, modrm | ((uint64_t)(in->op & 7) << 8) | ((uint64_t)in->osz << 16));
        if (in->mem)
            a64_mov(a, 1, 2, REA);
        else
            a64_mov_imm(a, 2, 0);
        emit_call(x, (const void *)jit_x87);
        x->stored = in->mem; /* FST/FSTP/FIST... podem escrever em codigo traduzido */
        x->flags_op = 0;     /* FCOMI/FUCOMI mudam os flags */
        return 1;
    }
    if (in->icall) {
        emit_icall(x, in);
        return 1;
    }
    if (in->mem || op == 0x50 || (op >= 0x50 && op <= 0x5f) || op == 0x68 || op == 0x6a || op == 0xc3 || op == 0xc2 ||
        op == 0xe8 || op == 0xc9 || op == 0x8f || (op == 0xff && (in->reg & 7) >= 2))
        set_cur_rip(x, in);

    /* ALU classico */
    if (op < 0x40) {
        int aop = op >> 3, form = op & 7;
        int s = (form & 1) ? sz : 1;
        if (aop == 2 || aop == 3) { /* ADC/SBB */
            if (form == 4 || form == 5) {
                s = form == 4 ? 1 : sz;
                ld_greg(x, RA, R_AX, s, false);
                a64_mov_imm(a, RB, (uint64_t)in->imm);
            } else if (form <= 1) {
                load_rm(x, in, RA, s);
                ld_greg(x, RB, in->reg, s, rex);
            } else {
                load_rm(x, in, RB, s);
                ld_greg(x, RA, in->reg, s, rex);
            }
            emit_carry(x);
            emit_alu(x, aop, s);
            if (form == 4 || form == 5)
                st_greg(x, RR, R_AX, s, false);
            else if (form <= 1)
                store_rm(x, in, RR, s);
            else
                st_greg(x, RR, in->reg, s, rex);
            emit_adc_lazy(x, aop, s);
            return 1;
        }
        if (form == 5 && aop != 7 && emit_alu_imm_reg(x, R_AX, aop, sz, in->imm))
            return 1;
        if (!(x->j->off & 16384) && form <= 3 && (form & 1) && sz >= 4) {
            if (form == 1 && !in->mem && emit_alu_rr(x, aop, sz, in->rm, in->reg, -1))
                return 1;
            if (form == 3 && !in->mem && emit_alu_rr(x, aop, sz, in->reg, in->rm, -1))
                return 1;
            if (form == 3 && in->mem) { /* reg op= [mem]: a fonte vem da memoria em RB */
                emit_ea(x, in);
                emit_load(x, RB, sz);
                if (emit_alu_rr(x, aop, sz, in->reg, -1, RB))
                    return 1;
                ld_greg(x, RA, in->reg, sz, rex);
                emit_alu(x, aop, sz);
                if (aop != 7)
                    st_greg(x, RR, in->reg, sz, rex);
                return 1;
            }
        }
        if (form == 4 || form == 5) {
            s = form == 4 ? 1 : sz;
            ld_greg(x, RA, R_AX, s, false);
            a64_mov_imm(a, RB, (uint64_t)in->imm);
            emit_alu(x, aop, s);
            if (aop != 7)
                st_greg(x, RR, R_AX, s, false);
            return 1;
        }
        if (form <= 1) { /* r/m op= reg */
            load_rm(x, in, RA, s);
            ld_greg(x, RB, in->reg, s, rex);
            emit_alu(x, aop, s);
            if (aop != 7)
                store_rm(x, in, RR, s);
        } else {        /* reg op= r/m */
            load_rm(x, in, RB, s);
            ld_greg(x, RA, in->reg, s, rex);
            emit_alu(x, aop, s);
            if (aop != 7)
                st_greg(x, RR, in->reg, s, rex);
        }
        return 1;
    }
    if (op >= 0x40 && op <= 0x4f) { /* INC/DEC r (so em 32 bits; em 64 bits sao REX) */
        int r = op & 7;
        ld_greg(x, RA, r, sz, false);
        emit_incdec(x, NULL, op >= 0x48, sz);
        st_greg(x, RR, r, sz, false);
        return 1;
    }
    switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        int r = (op & 7) | ((in->rex & 1) << 3);
        int s = x->code64 ? (in->p66 && !(in->rex & 8) ? 2 : 8) : sz;
        ld_greg(x, RT, r, s, rex);
        emit_push(x, in, RT, s);
        return 1;
    }
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
        int r = (op & 7) | ((in->rex & 1) << 3);
        int s = x->code64 ? (in->p66 && !(in->rex & 8) ? 2 : 8) : sz;
        emit_pop_load(x, RT, s);
        st_greg(x, RB, R_SP, 8, true);
        st_greg(x, RT, r, s, rex); /* POP RSP: o valor lido vence */
        return 1;
    }
    case 0x63: /* MOVSXD */
        load_rm(x, in, RA, 4);
        a64_sext(a, RA, RA, 4);
        st_greg(x, RA, in->reg, 8, rex);
        return 1;
    case 0x68: case 0x6a: {
        int s = x->code64 ? (in->p66 && !(in->rex & 8) ? 2 : 8) : sz;
        a64_mov_imm(a, RT, (uint64_t)in->imm);
        emit_push(x, in, RT, s);
        return 1;
    }
    case 0x69: case 0x6b: case 0x1af: {
        if (op == 0x1af) {
            load_rm(x, in, RB, sz);
            ld_greg(x, RA, in->reg, sz, rex);
        } else {
            load_rm(x, in, RA, sz);
            a64_mov_imm(a, RB, (uint64_t)in->imm);
        }
        a64_mov(a, 1, 0, RC);
        a64_mov(a, 1, 1, RA);
        a64_mov(a, 1, 2, RB);
        a64_mov_imm(a, 3, (uint64_t)sz);
        emit_call(x, (const void *)x86_jit_imul2);
        st_greg(x, 0, in->reg, sz, rex);
        x->flags_op = 0;
        return 1;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        emit_jcc(x, in, op & 15, branch_target(x, in));
        return 0;
    case 0x80: case 0x81: case 0x83: {
        int aop = in->reg & 7;
        int s = op == 0x80 ? 1 : sz;
        if (op != 0x80 && !in->mem && aop != 7 && emit_alu_imm_reg(x, in->rm, aop, s, in->imm))
            return 1;
        load_rm(x, in, RA, s);
        a64_mov_imm(a, RB, (uint64_t)in->imm);
        if (aop == 2 || aop == 3)
            emit_carry(x);
        emit_alu(x, aop, s);
        if (aop != 7)
            store_rm(x, in, RR, s);
        if (aop == 2 || aop == 3)
            emit_adc_lazy(x, aop, s);
        return 1;
    }
    case 0xa0: case 0xa1: { /* MOV AL/eAX, moffs */
        int s = op == 0xa0 ? 1 : sz;
        emit_ea(x, in);
        emit_load(x, RA, s);
        st_greg(x, RA, R_AX, s, false);
        return 1;
    }
    case 0xa2: case 0xa3: { /* MOV moffs, AL/eAX */
        int s = op == 0xa2 ? 1 : sz;
        ld_greg(x, RB, R_AX, s, false);
        emit_ea(x, in);
        emit_store(x, RB, s, in->next);
        return 1;
    }
    case 0xfc: case 0xfd: /* CLD/STD */
        a64_ldr(a, 8, 9, RC, OFF(eflags));
        a64_mov_imm(a, 10, EFL_DF);
        a64_logic(a, 1, op == 0xfc ? 0 : 1, op == 0xfc ? 1 : 0, 9, 9, 10); /* BIC / ORR */
        a64_str(a, 8, 9, RC, OFF(eflags));
        return 1;
    case 0x84: case 0x85: {
        int s = op == 0x84 ? 1 : sz;
        load_rm(x, in, RA, s);
        ld_greg(x, RB, in->reg, s, rex);
        emit_alu(x, 4, s);
        return 1;
    }
    case 0x86: case 0x87: {
        int s = op == 0x86 ? 1 : sz;
        load_rm(x, in, RA, s);
        ld_greg(x, RB, in->reg, s, rex);
        store_rm(x, in, RB, s);
        st_greg(x, RA, in->reg, s, rex);
        return 1;
    }
    case 0x88: case 0x89: {
        int s = op == 0x88 ? 1 : sz;
        if (!(x->j->off & 16384) && s >= 4) { /* direto entre os registradores do cache */
            if (in->mem) {
                emit_ea(x, in);
                int hs = rc_get(x, in->reg, true);
                if (hs >= 0) {
                    emit_store(x, hs, s, in->next);
                    return 1;
                }
            } else if (mov_reg_reg(x, in->rm, in->reg, s)) {
                return 1;
            }
        }
        ld_greg(x, RB, in->reg, s, rex);
        if (in->mem)
            emit_ea(x, in);
        store_rm(x, in, RB, s);
        return 1;
    }
    case 0x8a: case 0x8b: {
        int s = op == 0x8a ? 1 : sz;
        if (!(x->j->off & 16384) && s >= 4) {
            if (in->mem) {
                emit_ea(x, in);
                int hd = rc_get(x, in->reg, false);
                if (hd >= 0) { /* carrega direto no registrador de destino */
                    emit_load(x, hd, s);
                    x->rc_dirty |= (uint16_t)(1u << in->reg);
                    return 1;
                }
                emit_load(x, RA, s);
                st_greg(x, RA, in->reg, s, rex);
                return 1;
            }
            if (mov_reg_reg(x, in->reg, in->rm, s))
                return 1;
        }
        load_rm(x, in, RA, s);
        st_greg(x, RA, in->reg, s, rex);
        return 1;
    }
    case 0x8d: { /* LEA: so o deslocamento, sem base de segmento */
        if (!in->mem)
            return -1;
        jinsn t = *in;
        t.ea_seg = S_DS;
        bool c64 = x->code64;
        x->code64 = true; /* evita somar a base do segmento */
        emit_ea(x, &t);
        x->code64 = c64;
        if (in->asz == 4 || !c64)
            a64_mov(a, 0, REA, REA);
        st_greg(x, REA, in->reg, sz, rex);
        return 1;
    }
    case 0x8f: { /* POP r/m */
        int s = x->code64 ? (in->p66 && !(in->rex & 8) ? 2 : 8) : sz;
        if (in->mem)
            return -1;
        emit_pop_load(x, RT, s);
        st_greg(x, RB, R_SP, 8, true);
        st_greg(x, RT, in->rm, s, rex);
        return 1;
    }
    case 0x90:
        if (in->rex & 1) { /* XCHG r8, rax */
            ld_greg(x, RA, 8, sz, rex);
            ld_greg(x, RB, R_AX, sz, rex);
            st_greg(x, RB, 8, sz, rex);
            st_greg(x, RA, R_AX, sz, rex);
        }
        return 1;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        int r = (op & 7) | ((in->rex & 1) << 3);
        ld_greg(x, RA, r, sz, rex);
        ld_greg(x, RB, R_AX, sz, rex);
        st_greg(x, RB, r, sz, rex);
        st_greg(x, RA, R_AX, sz, rex);
        return 1;
    }
    case 0x98: /* CBW/CWDE/CDQE */
        ld_greg(x, RA, R_AX, sz / 2, false);
        a64_sext(a, RA, RA, sz / 2);
        st_greg(x, RA, R_AX, sz, false);
        return 1;
    case 0x99: /* CWD/CDQ/CQO */
        ld_greg(x, RA, R_AX, sz, false);
        if (sz < 8)
            a64_sext(a, RA, RA, sz);
        a64_sbfm(a, 1, RA, RA, 63, 63); /* ASR #63 */
        st_greg(x, RA, R_DX, sz, false);
        return 1;
    case 0xa8: case 0xa9: {
        int s = op == 0xa8 ? 1 : sz;
        ld_greg(x, RA, R_AX, s, false);
        a64_mov_imm(a, RB, (uint64_t)in->imm);
        emit_alu(x, 4, s);
        return 1;
    }
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
        a64_mov_imm(a, RA, (uint64_t)in->imm & 0xff);
        st_greg(x, RA, (op & 7) | ((in->rex & 1) << 3), 1, rex);
        return 1;
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
        a64_mov_imm(a, RA, (uint64_t)in->imm);
        st_greg(x, RA, (op & 7) | ((in->rex & 1) << 3), sz, rex);
        return 1;
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        int s = (op & 1) ? sz : 1;
        load_rm(x, in, RA, s);
        if (emit_shift_native(x, in, s))
            return 1;
        if (op == 0xd2 || op == 0xd3)
            ld_greg(x, RB, R_CX, 1, false);
        else
            a64_mov_imm(a, RB, op >= 0xd0 ? 1 : (uint64_t)in->imm & 0xff);
        a64_mov(a, 1, 0, RC);
        a64_mov_imm(a, 1, (uint64_t)(in->reg & 7) | ((uint64_t)s << 8));
        a64_mov(a, 1, 2, RA);
        a64_mov(a, 1, 3, RB);
        emit_call(x, (const void *)x86_jit_shift);
        a64_mov(a, 1, RR, 0);
        store_rm(x, in, RR, s);
        x->flags_op = 0;
        return 1;
    }
    case 0xc2: case 0xc3: { /* RET */
        int s = x->code64 ? 8 : sz;
        emit_pop_load(x, RT, s);
        if (op == 0xc2)
            add_const(x, RB, RB, in->imm & 0xffff);
        if (!x->code64)
            a64_mov(a, 0, RB, RB);
        st_greg(x, RB, R_SP, 8, true);
        emit_exit_lookup(x, 0, RT);
        return 0;
    }
    case 0xc6: case 0xc7: {
        if ((in->reg & 7) != 0)
            return -1;
        int s = op == 0xc6 ? 1 : sz;
        a64_mov_imm(a, RB, (uint64_t)in->imm);
        if (in->mem)
            emit_ea(x, in);
        store_rm(x, in, RB, s);
        return 1;
    }
    case 0xc9: { /* LEAVE: RSP = RBP; POP RBP */
        int s = x->code64 ? (in->p66 && !(in->rex & 8) ? 2 : 8) : sz;
        ld_greg(x, RB, R_BP, 8, true);
        if (!x->code64)
            a64_mov(a, 0, RB, RB);
        if (x->code64) {
            a64_mov(a, 1, REA, RB);
        } else {
            a64_ldr(a, 8, 10, RC, (uint32_t)(offsetof(x86_cpu, seg) + sizeof(x86_seg) * S_SS + offsetof(x86_seg, base)));
            a64_add(a, REA, RB, 10);
            a64_mov(a, 0, REA, REA);
        }
        emit_load(x, RT, s);
        a64_add_imm(a, RB, RB, (uint32_t)s);
        if (!x->code64)
            a64_mov(a, 0, RB, RB);
        st_greg(x, RB, R_SP, 8, true);
        st_greg(x, RT, R_BP, s, false);
        return 1;
    }
    case 0xe8: { /* CALL rel */
        int s = x->code64 ? 8 : sz;
        a64_mov_imm(a, RT, in->next);
        emit_push(x, in, RT, s);
        emit_exit_next(x, branch_target(x, in), true);
        return 0;
    }
    case 0xe9: case 0xeb:
        emit_exit_next(x, branch_target(x, in), true);
        return 0;
    case 0xf6: case 0xf7: {
        int s = op == 0xf6 ? 1 : sz;
        int r = in->reg & 7;
        if ((r == 4 || r == 5) && s >= 4 && !(x->j->off & 64)) { /* MUL/IMUL de 32/64 bits nativos */
            bool sgn = r == 5;
            if (in->mem)
                set_cur_rip(x, in);
            load_rm(x, in, RA, s);
            ld_greg(x, RB, R_AX, s, false);
            if (s == 8) {
                a64_madd(a, 1, RR, RB, RA, XZR);  /* parte baixa */
                a64_mulh(a, sgn, RT, RB, RA);     /* parte alta */
                if (sgn) {
                    a64_asr_imm(a, 9, RR, 63);
                    a64_cmp(a, 1, RT, 9);
                } else {
                    a64_cmp(a, 1, RT, XZR);
                }
            } else {
                a64_mull(a, sgn, 9, RB, RA);     /* produto de 64 bits */
                a64_mov(a, 0, RR, 9);
                a64_lsr_imm(a, RT, 9, 32);
                if (sgn) {
                    a64_sext(a, 10, 9, 4);
                    a64_cmp(a, 1, 9, 10);
                } else {
                    a64_cmp(a, 1, RT, XZR);
                }
            }
            if (!x->dead) { /* cc_aux = CF | OF << 1 = estouro * 3 (x11: st_greg so usa x9) */
                a64_cset(a, 11, A_NE);
                a64_addsub_reg(a, 1, 0, 0, 11, 11, 11, 1); /* x11 += x11 << 1 */
            }
            st_greg(x, RR, R_AX, s, false);
            st_greg(x, RT, R_DX, s, false);
            if (!x->dead) {
                a64_mov_imm(a, 9, (uint64_t)CC_SZP | ((uint64_t)(uint32_t)s << 32));
                a64_str(a, 8, 9, RC, OFF(cc_op));
                a64_str(a, 8, RR, RC, OFF(cc_dst));
                a64_str(a, 8, 11, RC, OFF(cc_aux));
                x->ccop_mem = (int64_t)((uint64_t)CC_SZP | ((uint64_t)(uint32_t)s << 32));
                x->flags_op = CC_SZP;
                x->flags_size = s;
            } else {
                x->flags_op = 0;
            }
            return 1;
        }
        if (r >= 4) { /* MUL/IMUL/DIV/IDIV: helper (DIV pode gerar #DE) */
            set_cur_rip(x, in);
            load_rm(x, in, RA, s);
            a64_mov(a, 1, 0, RC);
            a64_mov_imm(a, 1, (uint64_t)r);
            a64_mov(a, 1, 2, RA);
            a64_mov_imm(a, 3, (uint64_t)s);
            emit_call(x, (const void *)x86_jit_muldiv);
            if (r < 6)
                x->flags_op = 0;
            return 1;
        }
        load_rm(x, in, RA, s);
        if (r == 0) {
            a64_mov_imm(a, RB, (uint64_t)in->imm);
            emit_alu(x, 4, s);
        } else if (r == 2) { /* NOT: sem flags */
            a64_logic(a, 1, 1, 1, RR, XZR, RA);
            store_rm(x, in, RR, s);
        } else { /* NEG = 0 - v */
            a64_mov(a, 1, RB, RA);
            a64_mov(a, 1, RA, XZR);
            emit_alu(x, 5, s);
            store_rm(x, in, RR, s);
        }
        return 1;
    }
    case 0xfe: case 0xff: {
        int r = in->reg & 7;
        int s = op == 0xfe ? 1 : sz;
        if (r <= 1) { /* INC/DEC: CF preservado em cc_aux */
            load_rm(x, in, RA, s);
            emit_incdec(x, in, r, s);
            return 1;
        }
        if (op == 0xfe)
            return -1;
        if (r == 2 || r == 4) { /* CALL/JMP indireto */
            int s2 = x->code64 ? 8 : sz;
            load_rm(x, in, RA, s2);
            if (r == 2) {
                a64_mov_imm(a, RT, in->next);
                emit_push(x, in, RT, s2);
            }
            emit_exit_lookup(x, 0, RA);
            return 0;
        }
        /* PUSH r/m */
        int s2 = x->code64 ? (in->p66 && !(in->rex & 8) ? 2 : 8) : sz;
        load_rm(x, in, RT, s2);
        emit_push(x, in, RT, s2);
        return 1;
    }
    default:
        break;
    }
    if (op >= 0x140 && op <= 0x14f) { /* CMOVcc */
        load_rm(x, in, RA, sz);
        ld_greg(x, RB, in->reg, sz, rex);
        int cond = (x->j->off & 16384) ? COND_W0 : emit_cond(x, op & 15);
        if (cond == COND_W0 || cond == A_AL || cond == COND_NEVER) {
            if (x->j->off & 16384)
                cond_to_x0(x, op & 15);
            else if (cond != COND_W0)
                a64_mov_imm(a, 0, cond == A_AL ? 1 : 0);
            a64_cmp(a, 1, 0, XZR);
            cond = A_NE;
        }
        a64_csel(a, 1, RB, RA, RB, cond); /* condicao direto dos flags do host */
        st_greg(x, RB, in->reg, sz, rex); /* 32 bits: zera a parte alta mesmo sem mover */
        return 1;
    }
    if (op >= 0x180 && op <= 0x18f) {
        emit_jcc(x, in, op & 15, branch_target(x, in));
        return 0;
    }
    if (op >= 0x190 && op <= 0x19f) { /* SETcc */
        if (in->mem)
            emit_ea(x, in);
        cond_to_x0(x, op & 15);
        a64_mov(a, 1, RR, 0);
        store_rm(x, in, RR, 1);
        return 1;
    }
    if (op == 0x1b6 || op == 0x1b7 || op == 0x1be || op == 0x1bf) {
        int s = (op & 1) ? 2 : 1;
        load_rm(x, in, RA, s);
        if (op >= 0x1be)
            a64_sext(a, RA, RA, s);
        st_greg(x, RA, in->reg, sz, rex);
        return 1;
    }
    if (op >= 0x1c8 && op <= 0x1cf) { /* BSWAP */
        int r = (op & 7) | ((in->rex & 1) << 3);
        if (sz == 2)
            return -1;
        ld_greg(x, RA, r, sz, rex);
        a64_put(a, (sz == 8 ? 0xDAC00C00u : 0x5AC00800u) | ((uint32_t)RA << 5) | (uint32_t)RA);
        st_greg(x, RA, r, sz, rex);
        return 1;
    }
    if ((op >= 0x118 && op <= 0x11f) || op == 0x10d) /* NOP longo, PREFETCH, ENDBR */
        return 1;
    return -1;
}

/* ------------------------------------------------------------ cache */


static uint32_t mode_key(x86_cpu *c) { return (c->code64 ? 1u : 0) | (c->ssz == 8 ? 2u : 0) | (x86_user(c) ? 4u : 0); }

static bool mode_ok(x86_cpu *c)
{
    return (c->cr0 & CR0_PE) && !(c->eflags & EFL_VM) && (c->code64 || c->csz == 4) && c->ssz != 2;
}

static unsigned hash_of(uint64_t lin, uint32_t mode) { return (unsigned)((lin >> 2) ^ (lin >> 17) ^ mode) & ((1u << HASH_BITS) - 1); }

static uint64_t chunk_mask(unsigned lo, unsigned hi) /* trechos de 64 bytes em [lo, hi) */
{
    if (hi <= lo)
        return 0;
    unsigned a = lo >> 6, b = (hi - 1) >> 6;
    return (b >= 63 ? ~0ULL : (2ULL << b) - 1) & ~((1ULL << a) - 1);
}

static void mark_code_page(struct x86_jit *j, uint64_t phys, unsigned end_off)
{
    if (phys >= j->mem->code_limit)
        return;
    uint64_t pg = phys >> 12;
    j->chunks[pg] |= chunk_mask((unsigned)(phys & 0xfff), end_off);
    if (!((j->bits[pg >> 3] >> (pg & 7)) & 1)) {
        j->bits[pg >> 3] |= (uint8_t)(1u << (pg & 7));
        /* as entradas de TLB para escrita nessa pagina precisam ir pelo caminho lento */
        x86_tlb_protect_page(j->c, phys & ~0xfffULL);
    }
}

/* esvazia o cache global de saltos e invalida os caches por ponto de salto (geracao nova) */
static void jc_reset(struct x86_jit *j)
{
    memset(j->jc, 0xff, sizeof(j->jc)); /* chave ~0 nunca bate */
    if (j->jc_cnt)
        memset(j->jc_cnt, 0, j->npages * sizeof(uint16_t));
    j->jc[0].pad = ++j->site_gen;
    j->pending_site = NULL;
}

/* invalida so as entradas do cache de saltos da pagina pg (e os caches por ponto de salto) */
static void jc_drop_page(struct x86_jit *j, uint64_t pg)
{
    if (j->jc_cnt[pg]) {
        for (unsigned i = 0; i < JC_SIZE; i++)
            if (j->jc[i].phys >> 12 == pg) {
                j->jc[i].key = ~0ULL;
                j->jc[i].phys = ~0ULL;
            }
        j->jc_cnt[pg] = 0;
    }
    j->jc[0].pad = ++j->site_gen;
    j->pending_site = NULL;
}

/* O cache de saltos confere a traducao no TLB a cada consulta: trocas de CR3 e
 * INVLPG nao o invalidam (so a invalidacao de blocos, em code_hook/flush). */
void x86_jit_tlb_flushed(struct x86_jit *j) { (void)j; }

void x86_jit_flush(struct x86_jit *j)
{
    if (!j)
        return;
    jc_reset(j);
    memset(j->hash, 0, sizeof(j->hash));
    if (j->pages)
        memset(j->pages, 0, j->npages * sizeof(jit_block *));
    if (j->bits)
        memset(j->bits, 0, (j->npages + 7) / 8);
    if (j->chunks)
        memset(j->chunks, 0, j->npages * sizeof(uint64_t));
    j->nblocks = 0;
    j->pos = j->exit_stub + 32; /* o stub fica no inicio */
    j->c->jit_exit = NULL;
    j->n_flush++;
    x86_tlb_flush(j->c);
}

/* invalida os blocos de uma pagina fisica (escrita em codigo) */
static void code_hook(void *opaque, uint64_t pa, uint64_t len)
{
    struct x86_jit *j = opaque;
    uint64_t pg = pa >> 12;
    if (pg >= j->npages)
        return;
    /* escrita em dado vizinho de codigo: a pagina continua traduzida (e com escritas lentas) */
    unsigned lo = (unsigned)(pa & 0xfff), hi = lo + (unsigned)(len > 0x1000 - lo ? 0x1000 - lo : len);
    if (!(j->chunks[pg] & chunk_mask(lo, hi ? hi : lo + 1)))
        return;
    j->chunks[pg] = 0;
    for (jit_block *b = j->pages[pg]; b; b = b->pnext)
        b->valid = false;
    j->pages[pg] = NULL;
    j->bits[pg >> 3] &= (uint8_t)~(1u << (pg & 7));
    j->n_inval++;
    j->c->jit_smc = 1;
    jc_drop_page(j, pg);
    /* a pagina volta a aceitar escritas rapidas */
    x86_tlb_unprotect_page(j->c, pa & ~0xfffULL);
}

static jit_block *lookup(struct x86_jit *j, uint64_t lin, uint64_t phys, uint32_t mode, uint64_t csbase)
{
    jit_block **pp = &j->hash[hash_of(lin, mode)];
    while (*pp) {
        jit_block *b = *pp;
        if (!b->valid) { /* remove da lista os blocos invalidados */
            *pp = b->hnext;
            continue;
        }
        if (b->lin == lin && b->phys == phys && b->mode == mode && b->csbase == csbase)
            return b;
        pp = &b->hnext;
    }
    return NULL;
}

/* efeito de uma instrucao sobre os flags aritmeticos (analise de vivacidade) */
#define FX_READ 1
#define FX_WRITE 2
static int flags_effect(const jinsn *in)
{
    int op = in->op, r = in->reg & 7;
    if (in->icall) {
        int nk = native_icall_kind(in);
        if (sse_kind(in, NULL, NULL) != SSE_NONE || nk == 1)
            return 0;
        int s2 = simd2_kind(in);
        if (s2 == S2_COMIS)
            return FX_WRITE; /* reescreve todos os flags aritmeticos */
        if (s2 != S2_NONE)
            return 0;
        if (nk == 3 || nk == 4)
            return FX_WRITE; /* CMPXCHG/XADD reescrevem todos os flags */
        return FX_READ | FX_WRITE;
    }
    if (op < 0x40)
        return ((op >> 3) == 2 || (op >> 3) == 3) ? (FX_READ | FX_WRITE) : FX_WRITE;
    if (op >= 0x40 && op <= 0x4f)
        return FX_READ | FX_WRITE;
    switch (op) {
    case 0x80: case 0x81: case 0x83: return (r == 2 || r == 3) ? (FX_READ | FX_WRITE) : FX_WRITE;
    case 0x84: case 0x85: case 0xa8: case 0xa9: case 0x69: case 0x6b: case 0x1af: return FX_WRITE;
    case 0xf6: case 0xf7: return (r == 0 || r == 3 || r == 4 || r == 5) ? FX_WRITE : 0;
    case 0xfe: case 0xff: return r <= 1 ? (FX_READ | FX_WRITE) : 0;
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: /* SHL/SHR/SAR com contagem != 0 reescrevem tudo */
        return (r >= 4 && shift_count((jinsn *)in, (in->op & 1) ? op_size((jinsn *)in) : 1) != 0) ? FX_WRITE
                                                                                                  : FX_READ | FX_WRITE;
    case 0xd2: case 0xd3: return FX_READ | FX_WRITE;
    default: break;
    }
    if ((op >= 0x70 && op <= 0x7f) || (op >= 0x140 && op <= 0x14f) || (op >= 0x180 && op <= 0x19f))
        return FX_READ;
    return 0;
}

/*
 * Gera o corpo do bloco. dead[i] (opcional) diz se os flags da instrucao i sao
 * mortos. Preenche ins/stored e devolve o numero de instrucoes traduzidas.
 */
/* Instrucao que atravessa o fim da pagina: executada pelo interpretador dentro do bloco
 * (ele busca os bytes da outra pagina a cada vez, entao o bloco nao depende dela). So
 * instrucoes comuns, com prefixos e opcode nesta pagina: assim a classe da instrucao nao
 * muda se a outra pagina mudar (no maximo o tamanho e os operandos; o RIP conferido
 * depois da chamada cobre o tamanho). */
static bool xpage_decode(jctx *x, const uint8_t *page, unsigned off, uint64_t rip, jinsn *in)
{
    x86_cpu *c = x->c;
    unsigned avail = 0x1000 - off;
    uint8_t buf[16];
    if (avail >= 15 || (x->j->off & 8192))
        return false;
    memcpy(buf, page + off, avail);
    uint64_t li = x->code64 ? rip : (uint32_t)(x->csbase + rip);
    if (!x86_peek_code(c, (li & ~0xfffULL) + 0x1000, buf + avail, 15 - avail))
        return false;
    if (!decode(c, buf, 15, rip, in) || (unsigned)in->len <= avail)
        return false;
    /* cabecalho: prefixos, opcode e ModRM */
    unsigned i = 0;
    while (i < avail) {
        uint8_t b = buf[i];
        if (b == 0x66 || b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x26 || b == 0x2e || b == 0x36 ||
            b == 0x3e || b == 0x64 || b == 0x65 || (x->code64 && (b & 0xf0) == 0x40))
            i++;
        else
            break;
    }
    int op = in->op;
    i += op < 0x100 ? 1 : op < 0x200 ? 2 : 3;
    /* o ModRM pode ficar na outra pagina, exceto no grupo FF (CALL/JMP far) */
    if (op == 0xff)
        i++;
    if (i > avail)
        return false;
    if (op < 0x100) {
        switch (op) {
        case 0x07: case 0x17: case 0x1f: case 0x62: case 0x8e: case 0x9a: case 0x9d: case 0xc4: case 0xc5:
        case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: case 0xe4: case 0xe5: case 0xe6: case 0xe7:
        case 0xea: case 0xec: case 0xed: case 0xee: case 0xef: case 0xf1: case 0xf4: case 0xfa: case 0xfb:
        case 0x6c: case 0x6d: case 0x6e: case 0x6f:
            return false;
        case 0xff:
            return (in->reg & 7) != 3 && (in->reg & 7) != 5 && (in->reg & 7) != 7;
        default:
            return true;
        }
    }
    if (op >= 0x200)
        return true; /* 0F 38 / 0F 3A: SSSE3/SSE4 */
    int o = op & 0xff;
    return (o >= 0x80 && o <= 0x8f) || (o >= 0x90 && o <= 0x9f) || (o >= 0x40 && o <= 0x4f) || o == 0xaf ||
           o == 0xb6 || o == 0xb7 || o == 0xbe || o == 0xbf || o == 0xa3 || o == 0xab || o == 0xb3 || o == 0xbb ||
           o == 0xba || o == 0xbc || o == 0xbd || o == 0xb8 || o == 0xc0 || o == 0xc1 || o == 0xb0 || o == 0xb1 ||
           o == 0xa4 || o == 0xa5 || o == 0xac || o == 0xad || o == 0x1f || o == 0x0d || o == 0x18 ||
           (o >= 0xc8 && o <= 0xcf) || (o >= 0x10 && o <= 0x17) || (o >= 0x28 && o <= 0x2f) ||
           (o >= 0x50 && o <= 0x7f) || (o >= 0xc2 && o <= 0xc6) || o >= 0xd0;
}

char x86_jit_dis_path[256]; /* depuracao (CLI "jitdis ARQ") */

static unsigned emit_block(jctx *x, const uint8_t *page, unsigned limit, const bool *dead, jinsn *ins, bool *stored,
                           bool *ended_out)
{
    struct x86_jit *j = x->j;
    x86_cpu *c = x->c;
    a64 *a = &x->a;
    jit_block *b = x->b;

    /* prologo: orcamento de instrucoes (corrigido no fim). O bloco roda se ainda ha
     * orcamento, mesmo que ultrapasse um pouco o fim da fatia. */
    uint32_t *blt, *sub_at;
    if (j->off & 2048) { /* MVM_JIT_OFF=2048: orcamento so em memoria (como antes) */
        a64_ldr(a, 8, 9, RC, OFF(jit_budget));
        a64_addsub_imm(a, 1, 1, 1, XZR, 9, 0, 0);
        blt = a64_here(a);
        a64_bcond(a, A_LE, a64_here(a));
        sub_at = a64_here(a);
        a64_sub_imm(a, 9, 9, 0);
        a64_str(a, 8, 9, RC, OFF(jit_budget));
        a64_mov(a, 1, RBUD, 9);
    } else {
        a64_addsub_imm(a, 1, 1, 1, XZR, RBUD, 0, 0); /* cmp x29, #0 */
        blt = a64_here(a);
        a64_bcond(a, A_LE, a64_here(a));
        sub_at = a64_here(a);
        a64_sub_imm(a, RBUD, RBUD, 0);
    }

    uint64_t rip = b->rip;
    unsigned n = 0;
    bool ended = false;
    while (n < limit) {
        uint64_t li = x->code64 ? rip : (uint32_t)(x->csbase + rip);
        unsigned off = (unsigned)(li & 0xfff);
        if ((li & ~0xfffULL) != x->page_lin)
            break;
        jinsn in;
        if (unlikely(c->brk) && rip == c->brk) /* MVM_X86_BREAK: fica com o interpretador */
            break;
        bool xpage = false;
        if (!decode(c, page + off, (int)(0x1000 - off), rip, &in)) {
            if (!xpage_decode(x, page, off, rip, &in))
                break;
            xpage = true;
        }
        if (j->skip[in.op & 0x1ff])
            break;
        uint32_t *save = a->p;
        jctx snap = *x; /* estado de compilacao (flags, saidas, cache de registradores) */
        x->stored = false;
        x->dead = dead ? dead[n] : false;
        x->rip_pending = false;
        int r = 1;
        if (xpage)
            emit_icall(x, &in);
        else
            r = emit_insn(x, &in);
        x->rip_pending = false;
        if (r < 0) { /* nao suportada: desfaz o que foi emitido */
            *x = snap;
            a->p = save;
            break;
        }
        ins[n] = in;
        stored[n] = x->stored;
        n++;
        x->end_off = off + (unsigned)in.len;
        rip = in.next;
        if (r == 0) {
            ended = true;
            break;
        }
        if (x->stored) {
            /* uma escrita pelo caminho lento invalidou codigo traduzido (talvez este
             * bloco): sai depois da instrucao completa. A saida fica fora da linha. */
            a64_ldr(a, 4, 9, RC, OFF(jit_smc));
            uint32_t *jbad = a64_here(a);
            a64_cbnz(a, 0, 9, a64_here(a));
            if (cold_begin(x, jbad, NULL)) {
                emit_exit_next(x, rip, false);
                cold_end_exit(x);
            } else {
                *jbad ^= 1u << 24; /* CBNZ -> CBZ: pula a saida em linha */
                emit_exit_next(x, rip, false);
                a64_patch(jbad, a64_here(a));
            }
        }
    }
    if (n && !ended)
        emit_exit_next(x, rip, true);
    /* saida antecipada (orcamento esgotado): nada executado */
    a64_patch(blt, a64_here(a));
    emit_exit_why(x, EX_BUDGET);
    a64_mov_imm(a, 9, b->rip);
    a64_str(a, 8, 9, RC, OFF(rip));
    a64_str(a, 8, XZR, RC, OFF(jit_exit));
    a64_b(a, j->exit_stub);
    cold_flush(x);
    *sub_at = (*sub_at & ~(0xfffu << 10)) | (n << 10);
    *ended_out = ended;
    return n;
}

static jit_block *translate(struct x86_jit *j, x86_cpu *c, uint64_t lin, uint64_t phys, const uint8_t *page, uint32_t mode)
{
    if (j->nblocks >= MAX_BLOCKS || (size_t)(j->code_end - j->pos) < 65536)
        x86_jit_flush(j);
    jit_block *b = &j->blocks[j->nblocks++];
    memset(b, 0, sizeof(*b));
    uint64_t csbase = c->code64 ? 0 : c->seg[S_CS].base;
    b->lin = lin;
    b->rip = c->rip;
    b->csbase = csbase;
    b->phys = phys;
    b->mode = mode;
    b->valid = true;
    b->code = j->pos;

    static jinsn ins[BLOCK_MAX_INSNS];
    static bool stored[BLOCK_MAX_INSNS], dead[BLOCK_MAX_INSNS];
    jctx x;
    bool ended;
    unsigned n = 0;
    for (int pass = 0; pass < 2; pass++) {
        memset(&x, 0, sizeof(x));
        x.j = j;
        x.c = c;
        x.b = b;
        x.code64 = c->code64;
        x.csbase = csbase;
        x.page_lin = lin & ~0xfffULL;
        x.mode = mode;
        x.a.p = b->code;
        x.a.end = j->code_end;
        rc_reset(&x);
        x.rc_on = !(j->off & 32);
        x.ccop_mem = -1;
        memset(b->exit, 0, sizeof(b->exit));
        if (pass == 0) {
            n = emit_block(&x, page, j->max_insns, NULL, ins, stored, &ended);
            if (n == 0)
                break;
            /* vivacidade dos flags, de tras para frente (vivos na saida do bloco) */
            bool live = true;
            for (int i = (int)n - 1; i >= 0; i--) {
                bool after = live || stored[i]; /* apos uma escrita pode haver saida (codigo automodificavel) */
                int fx = flags_effect(&ins[i]);
                dead[i] = (fx & FX_WRITE) && !after && !(j->off & 16);
                if (fx & FX_READ)
                    live = true;
                else if (fx & FX_WRITE)
                    live = false;
                else
                    live = after;
            }
        } else {
            unsigned n2 = emit_block(&x, page, n, dead, ins, stored, &ended);
            if (n2 != n) { /* nao deveria acontecer: refaz sem a otimizacao */
                x.a.p = b->code;
                x.ncold = x.nm2c = x.nc2m = 0;
                rc_reset(&x);
                x.ccop_mem = -1;
                memset(b->exit, 0, sizeof(b->exit));
                x.nexit = 0;
                n = emit_block(&x, page, n, NULL, ins, stored, &ended);
            }
        }
    }
    if (n == 0) { /* primeira instrucao nao suportada: marca para o interpretador */
        if (j->stats && c->cpl == 3 && j->n_interp_log < 40) {
            j->n_interp_log++;
            unsigned o = (unsigned)(lin & 0xfff);
            LOGI("JIT: bloco interpretado em %llx (pagina+%03x): %02x %02x %02x %02x", (unsigned long long)lin, o, page[o],
                 o < 0xfff ? page[o + 1] : 0, o < 0xffe ? page[o + 2] : 0, o < 0xffd ? page[o + 3] : 0);
        }
        b->interp = true;
        b->code = NULL;
        return b;
    }
    a64 *a = &x.a;
    if (a->overflow) {
        x86_jit_flush(j);
        return NULL;
    }
    b->ninsn = n;
    j->pos = a->p;
    static FILE *dis;
    static int dis_init;
    if (!dis_init++ && getenv("MVM_JIT_DISASM"))
        dis = fopen(getenv("MVM_JIT_DISASM"), "w");
    if (unlikely(x86_jit_dis_path[0])) { /* CLI "jitdis ARQ": comeca a gravar com a VM rodando */
        if (dis)
            fclose(dis);
        dis = fopen(x86_jit_dis_path, "w");
        x86_jit_dis_path[0] = 0;
    }
    if (dis) { /* MVM_JIT_DISASM=arquivo: "# rip ninsn" + palavras do codigo (objdump -b binary) */
        fprintf(dis, "# %llx %u %p\n", (unsigned long long)b->rip, n, (void *)b->code);
        for (uint32_t *w = b->code; w < j->pos; w++)
            fprintf(dis, "%08x\n", *w);
        fflush(dis);
    }
    __builtin___clear_cache((char *)b->code, (char *)j->pos);
    mark_code_page(j, phys, x.end_off);
    if ((phys >> 12) < j->npages) {
        b->pnext = j->pages[phys >> 12];
        j->pages[phys >> 12] = b;
    }
    j->n_trans++;
    return b;
}

/* depuracao (CLI "jitoff N"): troca os bits de MVM_JIT_OFF com a VM rodando, para medir A/B */
volatile long x86_jit_new_off = -1;

int64_t x86_jit_run(x86_cpu *c, int64_t budget)
{
    struct x86_jit *j = c->jit;
    if (!j)
        return -1;
    if (unlikely(x86_jit_new_off >= 0)) {
        j->off = (unsigned)x86_jit_new_off;
        x86_jit_new_off = -1;
        x86_jit_flush(j);
        LOGI("JIT: MVM_JIT_OFF=0x%x", j->off);
    }
    if (unlikely(j->off & 0x40000000u)) /* depuracao: so o interpretador */
        return -1;
    if (!mode_ok(c)) {
        j->ret_nomode++;
        return -1;
    }
    uint64_t lin = c->code64 ? c->rip : (uint32_t)(c->seg[S_CS].base + c->rip);
    uint64_t phys;
    c->cur_rip = c->rip;
    const uint8_t *page = x86_code_host(c, lin & ~0xfffULL, &phys);
    if (!page) {
        j->ret_nopage++;
        return -1;
    }
    phys = (phys & ~0xfffULL) | (lin & 0xfff);
    uint32_t mode = mode_key(c);
    uint64_t csbase = c->code64 ? 0 : c->seg[S_CS].base;
    jit_exit_rec *e = c->jit_exit;
    uint64_t flushes = j->n_flush;
    jit_block *b = lookup(j, lin, phys, mode, csbase);
    if (!b) {
        b = translate(j, c, lin, phys, page, mode);
        if (!b)
            return -1;
        unsigned h = hash_of(lin, mode);
        b->hnext = j->hash[h];
        j->hash[h] = b;
    }
    if (b->interp || budget <= 0) {
        if (b->interp)
            j->ret_interp++;
        else
            j->ret_budget++;
        return -1;
    }
    if (j->stats && e) {
        j->nochain[0]++;
        if (j->n_flush != flushes) j->nochain[1]++;
        else if (!e->blk->valid) j->nochain[2]++;
        else if (e->target != c->rip) j->nochain[3]++;
        else if (e->blk->mode != mode) j->nochain[4]++;
        else if (e->blk->csbase != csbase) j->nochain[5]++;
        else if ((e->blk->phys >> 12) != (phys >> 12)) j->nochain[6]++;
        else j->nochain[7]++;
    }
    /* encadeia a saida anterior com este bloco (mesma pagina fisica, modo e CS) */
    if (e && j->n_flush == flushes && e->blk->valid && e->target == c->rip && e->blk->mode == mode &&
        e->blk->csbase == csbase && (e->blk->phys >> 12) == (phys >> 12)) {
        a64_patch(e->patch, b->code);
        __builtin___clear_cache((char *)e->patch, (char *)(e->patch + 1));
        j->n_chain++;
    }
    c->jit_budget = budget;
    c->jit_kick = 0;
    c->jit_exit = NULL;
    c->jit_smc = 0;
    if (j->pending_site && !csbase && j->pending_flush == j->n_flush) {
        /* o salto que acabou de falhar no cache do seu ponto passa a ir direto para ca */
        uint64_t *slot = j->pending_site;
        slot[1] = (uint64_t)(uintptr_t)b->code;
        slot[2] = phys & ~0xfffULL;
        slot[3] = j->site_gen;
        slot[0] = c->rip ^ ((uint64_t)mode << 59);
    }
    j->pending_site = NULL;
    if (!csbase) { /* registra no cache de saltos */
        uint64_t key = c->rip ^ ((uint64_t)mode << 59);
        unsigned h = (unsigned)(c->rip ^ (c->rip >> 12)) & (JC_SIZE - 1);
        uint64_t opg = j->jc[h].phys >> 12, npg = phys >> 12;
        if (j->jc[h].key != ~0ULL && opg < j->npages && j->jc_cnt[opg])
            j->jc_cnt[opg]--;
        if (npg < j->npages && j->jc_cnt[npg] < 0xffff)
            j->jc_cnt[npg]++;
        j->jc[h].key = key;
        j->jc[h].code = b->code;
        j->jc[h].phys = phys & ~0xfffULL;
        j->jc_used = true;
    }
    j->n_enter++;
    c->jit_pad = 0;
    c->jit_site = NULL;
    j->enter(c, b->code, &c->tlb[x86_user(c)][0], j->jc);
    j->pending_site = c->jit_site;
    j->pending_flush = j->n_flush;
    j->exit_why[c->jit_pad & 7]++;
    int64_t done = budget - (c->jit_budget + c->jit_kick);
    c->jit_budget = c->jit_kick = 0;
    if (j->stats)
        j->n_jit_insns += (uint64_t)done;
    if (c->jit_smc)
        c->jit_exit = NULL;
    return done;
}

/* stub de entrada/saida: salva os registradores preservados e salta para o bloco */
static void build_stubs(struct x86_jit *j)
{
    a64 A = {j->code, j->code_end, false}, *a = &A;
    j->enter = (void (*)(x86_cpu *, uint32_t *, void *, void *))(void *)a64_here(a);
    a64_stp_pre(a, 29, 30, XSP, -96);
    a64_mov(a, 1, 29, XSP);
    /* mov x29, sp (ADD x29, sp, #0) */
    a->p[-1] = 0x910003FDu;
    a64_stp(a, 19, 20, XSP, 16);
    a64_stp(a, 21, 22, XSP, 32);
    a64_stp(a, 23, 24, XSP, 48);
    a64_stp(a, 25, 26, XSP, 64);
    a64_stp(a, 27, 28, XSP, 80);
    a64_mov(a, 1, RC, 0);
    a64_mov(a, 1, RTLB, 2);
    a64_mov(a, 1, RJC, 3);
    a64_ldr(a, 8, RBUD, RC, OFF(jit_budget));
    a64_br(a, 1);
    j->exit_stub = a64_here(a);
    a64_str(a, 8, RBUD, RC, OFF(jit_budget));
    a64_ldp(a, 19, 20, XSP, 16);
    a64_ldp(a, 21, 22, XSP, 32);
    a64_ldp(a, 23, 24, XSP, 48);
    a64_ldp(a, 25, 26, XSP, 64);
    a64_ldp(a, 27, 28, XSP, 80);
    a64_ldp_post(a, 29, 30, XSP, 96);
    a64_ret(a);
    __builtin___clear_cache((char *)j->code, (char *)a->p);
    j->pos = j->exit_stub + 32;
}

struct x86_jit *x86_jit_new(x86_cpu *c)
{
    const char *e = getenv("MVM_JIT");
    if (e && *e == '0')
        return NULL;
    void *mem = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        LOGW("JIT: memoria executavel indisponivel; usando o interpretador");
        return NULL;
    }
    struct x86_jit *j = calloc(1, sizeof(*j));
    j->c = c;
    j->mem = c->mem;
    j->code = mem;
    j->code_end = (uint32_t *)((uint8_t *)mem + CODE_SIZE);
    jc_reset(j);
    j->blocks = calloc(MAX_BLOCKS, sizeof(jit_block));
    j->npages = (c->vm->ram_size + 0xfff) >> 12;
    j->pages = calloc(j->npages, sizeof(jit_block *));
    j->bits = calloc((j->npages + 7) / 8 + 1, 1);
    j->jc_cnt = calloc(j->npages, sizeof(uint16_t));
    j->chunks = calloc(j->npages, sizeof(uint64_t));
    c->mem->code_bits = j->bits;
    c->mem->code_limit = j->npages << 12;
    c->mem->code_hook = code_hook;
    c->mem->code_opaque = j;
    build_stubs(j);
    j->max_insns = BLOCK_MAX_INSNS;
    const char *st = getenv("MVM_JIT_STATS");
    j->stats = st && *st == '1';
    const char *mi = getenv("MVM_JIT_MAXINSN");
    if (mi && atoi(mi) > 0)
        j->max_insns = (unsigned)atoi(mi);
    if (getenv("MVM_JIT_OFF"))
        j->off = (unsigned)strtoul(getenv("MVM_JIT_OFF"), NULL, 0);
    if (getenv("MVM_JIT_S2OFF")) /* depuracao: bit k desliga o tipo S2_k do SSE nativo */
        x86_jit_s2_off = (long)strtoul(getenv("MVM_JIT_S2OFF"), NULL, 0);
    const char *sk = getenv("MVM_JIT_SKIP");
    for (const char *q = sk; q && *q;) {
        char *endp;
        unsigned long v = strtoul(q, &endp, 16);
        if (endp == q)
            break;
        if (v < 0x200)
            j->skip[v] = 1;
        q = *endp == ',' ? endp + 1 : endp;
    }
    LOGI("JIT x86 -> AArch64 ativo");
    return j;
}

void x86_jit_free(struct x86_jit *j)
{
    if (!j)
        return;
    if (j->mem->code_opaque == j) {
        j->mem->code_bits = NULL;
        j->mem->code_hook = NULL;
    }
    munmap(j->code, CODE_SIZE);
    free(j->blocks);
    free(j->pages);
    free(j->bits);
    free(j->jc_cnt);
    free(j->chunks);
    free(j);
}

/* estatistica: instrucao que o interpretador vai executar (MVM_JIT_STATS) */
void x86_jit_note_interp(x86_cpu *c)
{
    struct x86_jit *j = c->jit;
    if (!j || !j->stats)
        return;
    if (!mode_ok(c)) {
        j->n_interp_nojit++;
        return;
    }
    j->n_interp++;
    uint64_t lin = c->code64 ? c->rip : (uint32_t)(c->seg[S_CS].base + c->rip);
    uint64_t pa;
    const uint8_t *p = x86_code_host(c, lin & ~0xfffULL, &pa);
    if (!p)
        return;
    unsigned off = (unsigned)(lin & 0xfff), op = 0;
    for (int i = 0; i < 15 && off < 0x1000; i++, off++) {
        uint8_t b = p[off];
        if (b == 0x66 || b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x26 || b == 0x2e || b == 0x36 ||
            b == 0x3e || b == 0x64 || b == 0x65 || (c->code64 && (b & 0xf0) == 0x40))
            continue;
        op = b;
        if (b == 0x0f && off + 1 < 0x1000)
            op = 0x100 | p[off + 1];
        break;
    }
    j->interp_op[op & 0x1ff]++;
}

/* MVM_JIT_BLOCKS=arquivo: "inicio fim rip ninsn simbolo" de cada bloco valido (para o perfil) */
static void dump_blocks(struct x86_jit *j)
{
    const char *path = getenv("MVM_JIT_BLOCKS");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f)
        return;
    for (unsigned i = 0; i < j->nblocks; i++) {
        jit_block *b = &j->blocks[i];
        if (!b->code)
            continue;
        uint32_t *end = i + 1 < j->nblocks && j->blocks[i + 1].code ? j->blocks[i + 1].code : j->pos;
        char sym[128];
        x86_debug_symbolize(j->c, b->lin, sym, sizeof(sym));
        fprintf(f, "%llx %llx %llx %u %s\n", (unsigned long long)(uintptr_t)b->code, (unsigned long long)(uintptr_t)end,
                (unsigned long long)b->rip, b->ninsn, sym[0] ? sym : "-");
    }
    fclose(f);
}

void x86_jit_stats(struct x86_jit *j)
{
    if (j)
        dump_blocks(j);
    if (j && j->stats) {
        LOGI("JIT: %llu instrucoes traduzidas, %llu interpretadas em modo traduzivel, %llu em modo nao traduzivel",
             (unsigned long long)j->n_jit_insns, (unsigned long long)j->n_interp, (unsigned long long)j->n_interp_nojit);
        for (int k = 0; k < 25; k++) {
            unsigned best = 0;
            for (unsigned o = 0; o < 0x200; o++)
                if (j->interp_op[o] > j->interp_op[best])
                    best = o;
            if (!j->interp_op[best])
                break;
            LOGI("JIT:   opcode %s%02x: %llu", best >= 0x100 ? "0f " : "", best & 0xff,
                 (unsigned long long)j->interp_op[best]);
            j->interp_op[best] = 0;
        }
        for (int k = 0; k < 25; k++) {
            unsigned best = 0;
            for (unsigned o = 0; o < 0x400; o++)
                if (j->icall_op[o] > j->icall_op[best])
                    best = o;
            if (!j->icall_op[best])
                break;
            LOGI("JIT:   icall %s%02x: %llu", best >= 0x300 ? "0f 3a " : best >= 0x200 ? "0f 38 " : best >= 0x100 ? "0f " : "",
                 best & 0xff, (unsigned long long)j->icall_op[best]);
            j->icall_op[best] = 0;
        }
    }
    if (j)
        LOGI("JIT: %llu blocos traduzidos, %llu invalidacoes de pagina, %llu esvaziamentos, %llu encadeamentos, %llu entradas",
             (unsigned long long)j->n_trans, (unsigned long long)j->n_inval, (unsigned long long)j->n_flush,
             (unsigned long long)j->n_chain, (unsigned long long)j->n_enter);
    if (j && j->stats) {
        for (int k = 1; k < EX_N; k++)
            LOGI("JIT:   saidas por %s: %llu", ex_names[k], (unsigned long long)j->exit_why[k]);
        LOGI("JIT:   saidas com registro %llu: flush %llu, invalido %llu, outro rip %llu, modo %llu, cs %llu, outra pagina %llu, encadeadas %llu",
             (unsigned long long)j->nochain[0], (unsigned long long)j->nochain[1], (unsigned long long)j->nochain[2],
             (unsigned long long)j->nochain[3], (unsigned long long)j->nochain[4], (unsigned long long)j->nochain[5],
             (unsigned long long)j->nochain[6], (unsigned long long)j->nochain[7]);
        LOGI("JIT:   retornos ao interpretador: bloco interpretado %llu, orcamento %llu, modo %llu, sem pagina %llu",
             (unsigned long long)j->ret_interp, (unsigned long long)j->ret_budget, (unsigned long long)j->ret_nomode,
             (unsigned long long)j->ret_nopage);
    }
}

#else /* sem JIT neste host */

struct x86_jit *x86_jit_new(x86_cpu *c)
{
    (void)c;
    return NULL;
}
void x86_jit_free(struct x86_jit *j) { (void)j; }
void x86_jit_flush(struct x86_jit *j) { (void)j; }
int64_t x86_jit_run(x86_cpu *c, int64_t budget)
{
    (void)c;
    (void)budget;
    return -1;
}
void x86_jit_stats(struct x86_jit *j) { (void)j; }
void x86_jit_note_interp(x86_cpu *c) { (void)c; }
void x86_jit_tlb_flushed(struct x86_jit *j) { (void)j; }

#endif
