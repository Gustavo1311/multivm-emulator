/*
 * Emissor de instrucoes AArch64 (A64) para o JIT. So as formas usadas pelo
 * tradutor; cada funcao grava uma instrucao de 32 bits no buffer.
 * Registradores: 0-30, 31 = XZR/SP conforme a instrucao.
 */
#ifndef MVM_A64_ASM_H
#define MVM_A64_ASM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t *p;    /* posicao atual */
    uint32_t *end;  /* limite do buffer */
    bool overflow;
} a64;

enum { A_EQ = 0, A_NE, A_HS, A_LO, A_MI, A_PL, A_VS, A_VC, A_HI, A_LS, A_GE, A_LT, A_GT, A_LE, A_AL };
#define XZR 31
#define XSP 31

static inline void a64_put(a64 *a, uint32_t ins)
{
    if (a->p >= a->end) {
        a->overflow = true;
        return;
    }
    *a->p++ = ins;
}

static inline uint32_t *a64_here(a64 *a) { return a->p; }

/* ---- imediatos ---- */
static inline void a64_movz(a64 *a, int sf, int rd, uint16_t imm, int hw)
{
    a64_put(a, (sf ? 0xD2800000u : 0x52800000u) | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}
static inline void a64_movk(a64 *a, int sf, int rd, uint16_t imm, int hw)
{
    a64_put(a, (sf ? 0xF2800000u : 0x72800000u) | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}
static inline void a64_movn(a64 *a, int sf, int rd, uint16_t imm, int hw)
{
    a64_put(a, (sf ? 0x92800000u : 0x12800000u) | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)rd);
}
/* carrega uma constante de 64 bits com o menor numero de instrucoes */
static inline void a64_mov_imm(a64 *a, int rd, uint64_t v)
{
    if (v == 0) {
        a64_movz(a, 1, rd, 0, 0);
        return;
    }
    if (~v < 0x10000) {
        a64_movn(a, 1, rd, (uint16_t)~v, 0);
        return;
    }
    if (v <= 0xffffffffULL && (uint32_t)~v < 0x10000) { /* 32 bits com a parte alta cheia */
        a64_movn(a, 0, rd, (uint16_t)~(uint32_t)v, 0);
        return;
    }
    bool first = true;
    for (int hw = 0; hw < 4; hw++) {
        uint16_t part = (uint16_t)(v >> (16 * hw));
        if (!part)
            continue;
        if (first)
            a64_movz(a, 1, rd, part, hw);
        else
            a64_movk(a, 1, rd, part, hw);
        first = false;
    }
}

/* ---- load/store com deslocamento imediato sem sinal (escalado) ---- */
static inline void a64_ldst_uimm(a64 *a, uint32_t base, int size, int rt, int rn, uint32_t off)
{
    uint32_t scale = size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
    a64_put(a, base | ((off >> scale) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
/* LDR (sem sinal, estende com zero) */
static inline void a64_ldr(a64 *a, int size, int rt, int rn, uint32_t off)
{
    static const uint32_t op[9] = {0, 0x39400000u, 0x79400000u, 0, 0xB9400000u, 0, 0, 0, 0xF9400000u};
    a64_ldst_uimm(a, op[size], size, rt, rn, off);
}
static inline void a64_str(a64 *a, int size, int rt, int rn, uint32_t off)
{
    static const uint32_t op[9] = {0, 0x39000000u, 0x79000000u, 0, 0xB9000000u, 0, 0, 0, 0xF9000000u};
    a64_ldst_uimm(a, op[size], size, rt, rn, off);
}
/* LDR/STR [Xn, Xm] */
static inline void a64_ldr_reg(a64 *a, int size, int rt, int rn, int rm)
{
    static const uint32_t op[9] = {0, 0x38606800u, 0x78606800u, 0, 0xB8606800u, 0, 0, 0, 0xF8606800u};
    a64_put(a, op[size] | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static inline void a64_str_reg(a64 *a, int size, int rt, int rn, int rm)
{
    static const uint32_t op[9] = {0, 0x38206800u, 0x78206800u, 0, 0xB8206800u, 0, 0, 0, 0xF8206800u};
    a64_put(a, op[size] | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
/* STP/LDP de 64 bits com pre/pos-indexacao */
static inline void a64_stp_pre(a64 *a, int rt, int rt2, int rn, int off)
{
    a64_put(a, 0xA9800000u | (((uint32_t)(off / 8) & 0x7f) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static inline void a64_ldp_post(a64 *a, int rt, int rt2, int rn, int off)
{
    a64_put(a, 0xA8C00000u | (((uint32_t)(off / 8) & 0x7f) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static inline void a64_stp(a64 *a, int rt, int rt2, int rn, int off)
{
    a64_put(a, 0xA9000000u | (((uint32_t)(off / 8) & 0x7f) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}
static inline void a64_ldp(a64 *a, int rt, int rt2, int rn, int off)
{
    a64_put(a, 0xA9400000u | (((uint32_t)(off / 8) & 0x7f) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

/* ---- aritmetica ---- */
/* ADD/SUB/ADDS/SUBS com imediato de 12 bits (sh = deslocado 12) */
static inline void a64_addsub_imm(a64 *a, int sf, int sub, int setf, int rd, int rn, uint32_t imm, int sh)
{
    uint32_t op = (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (setf ? 0x20000000u : 0) | 0x11000000u;
    a64_put(a, op | ((uint32_t)sh << 22) | ((imm & 0xfff) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_add_imm(a64 *a, int rd, int rn, uint32_t imm) { a64_addsub_imm(a, 1, 0, 0, rd, rn, imm, 0); }
static inline void a64_sub_imm(a64 *a, int rd, int rn, uint32_t imm) { a64_addsub_imm(a, 1, 1, 0, rd, rn, imm, 0); }
/* ADD/SUB (registrador deslocado, LSL) */
static inline void a64_addsub_reg(a64 *a, int sf, int sub, int setf, int rd, int rn, int rm, int lsl)
{
    uint32_t op = (sf ? 0x80000000u : 0) | (sub ? 0x40000000u : 0) | (setf ? 0x20000000u : 0) | 0x0B000000u;
    a64_put(a, op | ((uint32_t)rm << 16) | ((uint32_t)lsl << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_add(a64 *a, int rd, int rn, int rm) { a64_addsub_reg(a, 1, 0, 0, rd, rn, rm, 0); }
static inline void a64_sub(a64 *a, int rd, int rn, int rm) { a64_addsub_reg(a, 1, 1, 0, rd, rn, rm, 0); }
static inline void a64_cmp(a64 *a, int sf, int rn, int rm) { a64_addsub_reg(a, sf, 1, 1, XZR, rn, rm, 0); }
/* logicas (registrador): opc 0=AND 1=ORR 2=EOR 3=ANDS; n = inverte rm (BIC/ORN/EON) */
static inline void a64_logic(a64 *a, int sf, int opc, int n, int rd, int rn, int rm)
{
    a64_put(a, (sf ? 0x80000000u : 0) | ((uint32_t)opc << 29) | 0x0A000000u | ((uint32_t)n << 21) | ((uint32_t)rm << 16) |
                   ((uint32_t)rn << 5) | (uint32_t)rd);
}
/* logica com o segundo operando deslocado (sh: 0=LSL 1=LSR 2=ASR) */
static inline void a64_logic_sh(a64 *a, int sf, int opc, int rd, int rn, int rm, int sh, int amount)
{
    a64_put(a, (sf ? 0x80000000u : 0) | ((uint32_t)opc << 29) | 0x0A000000u | ((uint32_t)sh << 22) | ((uint32_t)rm << 16) |
                   ((uint32_t)amount << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_mov(a64 *a, int sf, int rd, int rm) { a64_logic(a, sf, 1, 0, rd, XZR, rm); }
static inline void a64_tst(a64 *a, int sf, int rn, int rm) { a64_logic(a, sf, 3, 0, XZR, rn, rm); }
/* AND imediato: so mascaras usadas aqui (N/immr/imms ja codificados) */
static inline void a64_and_bitmask(a64 *a, int rd, int rn, uint32_t n_immr_imms)
{
    a64_put(a, 0x92000000u | (n_immr_imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
/* logica com imediato codificado (opc: 0=AND 1=ORR 2=EOR 3=ANDS) */
static inline void a64_logic_imm(a64 *a, int sf, int opc, int rd, int rn, uint32_t n_immr_imms)
{
    a64_put(a, (sf ? 0x80000000u : 0) | ((uint32_t)opc << 29) | 0x12000000u | (n_immr_imms << 10) | ((uint32_t)rn << 5) |
                   (uint32_t)rd);
}
/* codifica v como imediato logico do AArch64 (N:immr:imms); false se nao tiver a forma
 * de um bloco de uns repetido e rotacionado. sf = 0: so os 32 bits baixos. */
static inline bool a64_encode_bitmask(uint64_t v, int sf, uint32_t *enc)
{
    if (!sf)
        v = (v & 0xffffffffULL) | (v << 32);
    if (v == 0 || v == ~0ULL)
        return false;
    unsigned size = 64;
    while (size > 2) { /* menor periodo que repete o padrao */
        unsigned h = size / 2;
        uint64_t m = (1ULL << h) - 1;
        if ((v & m) != ((v >> h) & m))
            break;
        size = h;
    }
    uint64_t mask = size == 64 ? ~0ULL : (1ULL << size) - 1;
    uint64_t e = v & mask;
    /* rotaciona para a direita ate o bloco de uns ficar no fundo (forma 0..01..1) */
    unsigned rot = 0;
    uint64_t r = e;
    for (; rot < size; rot++) {
        r = rot ? ((e >> rot) | (e << (size - rot))) & mask : e;
        if (!(r & (r + 1)))
            break;
    }
    if (rot == size)
        return false;
    unsigned ones = (unsigned)__builtin_popcountll(r);
    unsigned immr = (size - rot) % size;
    unsigned imms = ((~(size * 2 - 1)) & 0x3f) | (ones - 1);
    unsigned n = size == 64 ? 1 : 0;
    if (!sf && n)
        return false;
    *enc = (n << 12) | (immr << 6) | (imms & 0x3f);
    return true;
}
#define A64_MASK_PAGE_HI ((1u << 12) | (52u << 6) | 51u) /* ~0xfff */
#define A64_MASK_PAGE_LO ((1u << 12) | (0u << 6) | 11u)  /* 0xfff */

/* bitfields */
static inline void a64_ubfm(a64 *a, int sf, int rd, int rn, int immr, int imms)
{
    a64_put(a, (sf ? 0xD3400000u : 0x53000000u) | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_sbfm(a64 *a, int sf, int rd, int rn, int immr, int imms)
{
    a64_put(a, (sf ? 0x93400000u : 0x13000000u) | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_bfm(a64 *a, int rd, int rn, int immr, int imms)
{
    a64_put(a, 0xB3400000u | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_lsl_imm(a64 *a, int rd, int rn, int sh) { a64_ubfm(a, 1, rd, rn, (64 - sh) & 63, 63 - sh); }
static inline void a64_lsr_imm(a64 *a, int rd, int rn, int sh) { a64_ubfm(a, 1, rd, rn, sh, 63); }
static inline void a64_ubfx(a64 *a, int rd, int rn, int lsb, int width) { a64_ubfm(a, 1, rd, rn, lsb, lsb + width - 1); }
/* insere 'width' bits de rn (a partir do 0) em rd na posicao lsb */
static inline void a64_bfi(a64 *a, int rd, int rn, int lsb, int width) { a64_bfm(a, rd, rn, (64 - lsb) & 63, width - 1); }
/* extensao com sinal de 'size' bytes para 64 bits */
static inline void a64_sext(a64 *a, int rd, int rn, int size) { a64_sbfm(a, 1, rd, rn, 0, size * 8 - 1); }
/* extensao com zero de 'size' bytes (size 8 = copia) */
static inline void a64_zext(a64 *a, int rd, int rn, int size)
{
    if (size == 8)
        a64_mov(a, 1, rd, rn);
    else if (size == 4)
        a64_mov(a, 0, rd, rn);
    else
        a64_ubfm(a, 1, rd, rn, 0, size * 8 - 1);
}

/* deslocamento por registrador: type 0 = LSL, 1 = LSR, 2 = ASR, 3 = ROR */
static inline void a64_shiftv(a64 *a, int sf, int rd, int rn, int rm, int type)
{
    a64_put(a, 0x1AC02000u | ((uint32_t)sf << 31) | ((uint32_t)rm << 16) | ((uint32_t)type << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_asr_imm(a64 *a, int rd, int rn, int sh) { a64_sbfm(a, 1, rd, rn, sh, 63); }
/* rotacao a direita por constante (EXTR rd, rn, rn, #sh) */
static inline void a64_ror_imm(a64 *a, int sf, int rd, int rn, int sh)
{
    a64_put(a, (sf ? 0x93C00000u : 0x13800000u) | ((uint32_t)rn << 16) | ((uint32_t)sh << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* NEON (registradores Q/V) */
static inline void a64_ldr_q_reg(a64 *a, int qt, int rn, int rm) { a64_put(a, 0x3CE06800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)qt); }
static inline void a64_str_q_reg(a64 *a, int qt, int rn, int rm) { a64_put(a, 0x3CA06800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)qt); }
static inline void a64_ldr_q(a64 *a, int qt, int rn) { a64_put(a, 0x3DC00000u | ((uint32_t)rn << 5) | (uint32_t)qt); }
static inline void a64_str_q(a64 *a, int qt, int rn) { a64_put(a, 0x3D800000u | ((uint32_t)rn << 5) | (uint32_t)qt); }
/* operacao vetorial de 3 registradores: base = codificacao com Vd=Vn=Vm=0 */
static inline void a64_v3(a64 *a, uint32_t base, int vd, int vn, int vm) { a64_put(a, base | ((uint32_t)vm << 16) | ((uint32_t)vn << 5) | (uint32_t)vd); }
#define A64_V_EOR 0x6E201C00u
#define A64_V_AND 0x4E201C00u
#define A64_V_ORR 0x4EA01C00u
#define A64_V_BIC 0x4E601C00u /* vn & ~vm */
#define A64_V_ADD(sz) (0x4E208400u | ((uint32_t)(sz) << 22)) /* sz 0=16b 1=8h 2=4s 3=2d */
#define A64_V_SUB(sz) (0x6E208400u | ((uint32_t)(sz) << 22))
#define A64_V_CMEQ(sz) (0x6E208C00u | ((uint32_t)(sz) << 22))

/* contagem de zeros a esquerda / inversao de bits (64 bits) */
static inline void a64_clz(a64 *a, int rd, int rn) { a64_put(a, 0xDAC01000u | ((uint32_t)rn << 5) | (uint32_t)rd); }
static inline void a64_rbit(a64 *a, int rd, int rn) { a64_put(a, 0xDAC00000u | ((uint32_t)rn << 5) | (uint32_t)rd); }

/* ADR rd, alvo (+-1 MB) */
static inline void a64_adr(a64 *a, int rd, const uint32_t *target)
{
    int64_t d = (const uint8_t *)target - (const uint8_t *)a->p;
    uint32_t imm = (uint32_t)d & 0x1fffff;
    a64_put(a, 0x10000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | (uint32_t)rd);
}

/* multiplicacao */
static inline void a64_madd(a64 *a, int sf, int rd, int rn, int rm, int ra)
{
    a64_put(a, (sf ? 0x9B000000u : 0x1B000000u) | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* parte alta / produto longo */
static inline void a64_mulh(a64 *a, bool sgn, int rd, int rn, int rm)
{
    a64_put(a, (sgn ? 0x9B407C00u : 0x9BC07C00u) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_mull(a64 *a, bool sgn, int rd, int rn, int rm) /* Xd = Wn * Wm (64 bits) */
{
    a64_put(a, (sgn ? 0x9B207C00u : 0x9BA07C00u) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* selecao condicional */
static inline void a64_csel(a64 *a, int sf, int rd, int rn, int rm, int cond)
{
    a64_put(a, (sf ? 0x9A800000u : 0x1A800000u) | ((uint32_t)rm << 16) | ((uint32_t)cond << 12) | ((uint32_t)rn << 5) | (uint32_t)rd);
}
static inline void a64_cset(a64 *a, int rd, int cond)
{
    a64_put(a, 0x9A9F07E0u | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)rd);
}

/* ---- desvios ---- */
static inline void a64_b(a64 *a, uint32_t *target)
{
    a64_put(a, 0x14000000u | ((uint32_t)(target - a->p) & 0x3ffffff));
}
static inline void a64_bcond(a64 *a, int cond, uint32_t *target)
{
    a64_put(a, 0x54000000u | (((uint32_t)(target - a->p) & 0x7ffff) << 5) | (uint32_t)cond);
}
static inline void a64_cbz(a64 *a, int sf, int rt, uint32_t *target)
{
    a64_put(a, (sf ? 0xB4000000u : 0x34000000u) | (((uint32_t)(target - a->p) & 0x7ffff) << 5) | (uint32_t)rt);
}
static inline void a64_cbnz(a64 *a, int sf, int rt, uint32_t *target)
{
    a64_put(a, (sf ? 0xB5000000u : 0x35000000u) | (((uint32_t)(target - a->p) & 0x7ffff) << 5) | (uint32_t)rt);
}
static inline void a64_br(a64 *a, int rn) { a64_put(a, 0xD61F0000u | ((uint32_t)rn << 5)); }
static inline void a64_blr(a64 *a, int rn) { a64_put(a, 0xD63F0000u | ((uint32_t)rn << 5)); }
static inline void a64_ret(a64 *a) { a64_put(a, 0xD65F03C0u); }

/* corrige um desvio ja emitido (B, B.cond, CBZ/CBNZ) para apontar para 'target' */
static inline void a64_patch(uint32_t *at, uint32_t *target)
{
    int32_t d = (int32_t)(target - at);
    uint32_t ins = *at;
    if ((ins & 0xFC000000u) == 0x14000000u)
        *at = 0x14000000u | ((uint32_t)d & 0x3ffffff);
    else
        *at = (ins & ~(0x7ffffu << 5)) | (((uint32_t)d & 0x7ffff) << 5);
}

#endif
