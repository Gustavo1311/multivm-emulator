/* Teste de atomicidade com uma "outra CPU" (thread do host, MVM_STRESS_ATOMIC=0x700000:N):
 * convidado e host incrementam [0] com operacao atomica e [5] dentro de um spinlock [4]. */
#include <stdint.h>
void out_char(char c);
#define S ((volatile uint32_t *)0x700000)
static void hex(uint64_t v)
{
    for (int i = 28; i >= 0; i -= 4) out_char("0123456789abcdef"[(v >> i) & 15]);
    out_char('\n');
}
static void puts_(const char *s) { while (*s) out_char(*s++); }
void fuzz_main(void)
{
    for (int i = 0; i < 16; i++) S[i] = 0;
    S[2] = 1; /* comeca a outra CPU */
    for (long i = 0; i < GN; i++) {
        __asm__ volatile("lock incl %0" : "+m"(S[0]) : : "memory");
        uint32_t one;
        do {
            one = 1;
            __asm__ volatile("xchgl %0, %1" : "+r"(one), "+m"(S[4]) : : "memory");
            if (one) __asm__ volatile("pause");
        } while (one);
        S[5] = S[5] + 1;                       /* secao critica sem atomico */
        __asm__ volatile("" : : : "memory");
        S[4] = 0;                              /* libera com escrita simples (ordem do x86) */
    }
    while (!S[3]) __asm__ volatile("pause");
    /* fase 2: le a marca e depois o dado; pela ordem do x86, dado >= marca */
    uint32_t viol = 0, seen = 0;
    S[6] = 1;
    while (!S[10]) {
        uint32_t f = S[9];
        uint32_t d = S[8];
        if (d < f) viol++;
        if (f) seen++;
    }
    /* fase 3: varias operacoes travadas (64/32/16 bits) contra adicoes atomicas do host;
     * cada volta soma 4 em cada contador */
    volatile uint64_t *c64 = (volatile uint64_t *)0x700100;
    volatile uint32_t *c32 = (volatile uint32_t *)0x700108;
    volatile uint16_t *c16 = (volatile uint16_t *)0x70010c;
    *c64 = 0; *c32 = 0; *c16 = 0;
    /* a pagina dos contadores passa a ter codigo traduzido (escritas pelo caminho lento):
     * o LOCK tem de continuar atomico ali */
    volatile uint8_t *code = (volatile uint8_t *)0x700800;
    code[0] = 0xc3; /* ret */
    __asm__ volatile("call *%0" : : "r"(code) : "memory");
    S[11] = 1;
    for (long i = 0; i < GN; i++) {
        uint64_t a = 1;
        __asm__ volatile("lock xaddq %0, %1" : "+r"(a), "+m"(*c64) : : "memory");
        __asm__ volatile("lock incq %0" : "+m"(*c64) : : "memory");
        __asm__ volatile("lock addq $3, %0" : "+m"(*c64) : : "memory");
        __asm__ volatile("lock subq $1, %0" : "+m"(*c64) : : "memory");
        __asm__ volatile("lock decq %0" : "+m"(*c64) : : "memory");
        uint64_t o = *c64, n;
        do { n = o + 1; } while (!__sync_bool_compare_and_swap(c64, o, n) && ((o = *c64), 1));
        uint32_t b = 1;
        __asm__ volatile("lock xaddl %0, %1" : "+r"(b), "+m"(*c32) : : "memory");
        __asm__ volatile("lock incl %0" : "+m"(*c32) : : "memory");
        __asm__ volatile("lock addl $3, %0" : "+m"(*c32) : : "memory");
        __asm__ volatile("lock subl $1, %0" : "+m"(*c32) : : "memory");
        __asm__ volatile("lock decl %0" : "+m"(*c32) : : "memory");
        uint32_t o2 = *c32, n2;
        do { n2 = o2 + 1; } while (!__sync_bool_compare_and_swap(c32, o2, n2) && ((o2 = *c32), 1));
        uint16_t w = 1;
        __asm__ volatile("lock xaddw %0, %1" : "+r"(w), "+m"(*c16) : : "memory");
        __asm__ volatile("lock incw %0" : "+m"(*c16) : : "memory");
        __asm__ volatile("lock addw $3, %0" : "+m"(*c16) : : "memory");
        __asm__ volatile("lock subw $1, %0" : "+m"(*c16) : : "memory");
        __asm__ volatile("lock decw %0" : "+m"(*c16) : : "memory");
        uint16_t o3 = *c16, n3;
        do { n3 = (uint16_t)(o3 + 1); } while (!__sync_bool_compare_and_swap(c16, o3, n3) && ((o3 = *c16), 1));
    }
    while (!S[12]) __asm__ volatile("pause");
    puts_("C64 "); hex(*c64);
    puts_("C32 "); hex(*c32);
    puts_("C16 "); hex(*c16);
    puts_("ATOM "); hex(S[0]);
    puts_("VIOL "); hex(viol);
    puts_("SEEN "); hex(seen);
    puts_("LOCK "); hex(S[5]);
    puts_("FUZZ END\n");
}
