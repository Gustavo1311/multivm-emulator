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
    puts_("ATOM "); hex(S[0]);
    puts_("VIOL "); hex(viol);
    puts_("SEEN "); hex(seen);
    puts_("LOCK "); hex(S[5]);
    puts_("FUZZ END\n");
}
