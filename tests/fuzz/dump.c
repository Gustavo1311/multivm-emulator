/* Parte comum do fuzzer: imprime o estado salvo apos cada teste. */
#include <stdint.h>

void out_char(char c);
void run_tests(void);

uint64_t sv_regs[16];
uint64_t sv_flags;
uint8_t sv_xmm[256] __attribute__((aligned(16)));
extern uint8_t buf[512];

#ifdef __x86_64__
#define NREG 14
#define NXMM 16
#else
#define NREG 7
#define NXMM 8
#endif

static void hex(uint64_t v, int digits)
{
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        out_char("0123456789abcdef"[(v >> i) & 15]);
}

void dump(uint32_t idx, uint32_t mask)
{
    hex(idx, 5);
    out_char(' ');
    hex(sv_flags & mask, 3);
    for (int i = 0; i < NREG; i++) {
        out_char(' ');
#ifdef __x86_64__
        hex(sv_regs[i], 16);
#else
        hex((uint32_t)sv_regs[i], 8);
#endif
    }
#ifdef DUMP_BUF
    for (int i = 136; i < 148; i++) {
        out_char(i == 136 ? '[' : ' ');
        hex(buf[i], 2);
    }
    out_char(']');
#endif
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 512; i++)
        h = (h ^ buf[i]) * 1099511628211ULL;
    out_char(' ');
    hex(h, 16);
#ifdef DUMP_FULL
    for (int i = 0; i < NXMM * 16; i++) {
        if (i % 16 == 0) out_char(' ');
        hex(sv_xmm[i], 2);
    }
#else
    h = 1469598103934665603ULL;
    for (int i = 0; i < NXMM * 16; i++)
        h = (h ^ sv_xmm[i]) * 1099511628211ULL;
    out_char(' ');
    hex(h, 16);
#endif
    out_char('\n');
}

void fuzz_main(void)
{
    run_tests();
    const char *s = "FUZZ END\n";
    while (*s)
        out_char(*s++);
}
