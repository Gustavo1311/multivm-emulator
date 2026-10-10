/* Instrucoes que cruzam o fim de uma pagina de codigo (traduzidas em nativo pelo JIT com
 * conferencia dos bytes da pagina seguinte): CALL, JNZ e ADD com imediato num laco, e
 * depois o imediato reescrito na pagina seguinte (codigo automodificavel). */
#include <stdint.h>
void out_char(char c);
static void hex(uint64_t v)
{
    for (int i = 28; i >= 0; i -= 4) out_char("0123456789abcdef"[(v >> i) & 15]);
    out_char('\n');
}
static void puts_(const char *s) { while (*s) out_char(*s++); }
extern uint32_t xp_loop(uint32_t n);   /* soma com ADD imm32 que cruza a pagina */
extern uint8_t xp_imm[];               /* ultimo byte do imediato (na pagina seguinte) */
__asm__(
    ".text\n"
    ".p2align 12\n"
    "xp_base:\n"
    ".skip 4096 - 16, 0x90\n"
    "xp_loop:\n"                       /* pagina 0 + 4080 */
    "    xor %eax, %eax\n"
    "    mov %edi, %ecx\n"
    "1:\n"
    "    .skip 4093 - (. - xp_base), 0x90\n"
    "    call xp_inc\n"                 /* E8 + 4 bytes: 3 na pagina 0, 2 na pagina 1 */
    "    jmp 2f\n"
    "    .skip 8192 - 3 - (. - xp_base), 0x90\n"
    "2:  add $0x01000000, %eax\n"      /* 05 + imm32: 3 bytes na pagina 1, 2 na pagina 2 */
    "    .globl xp_imm\n"
    "    .set xp_imm, . - 1\n"
    "    dec %ecx\n"
    "    jnz 1b\n"
    "    ret\n"
    "xp_inc:\n"
    "    add $1, %eax\n"
    "    ret\n");
void fuzz_main(void)
{
    uint32_t a = xp_loop(100000);
    puts_("XP1 "); hex(a);           /* 100000 * (1 + 0x01000000) truncado */
    xp_imm[0] = 0x02;                /* ADD $0x02000000 a partir de agora */
    uint32_t b = xp_loop(1000);
    puts_("XP2 "); hex(b);
    puts_("FUZZ END\n");
}
