/* Testes de sistema ARMv7: SVC/UND, MMU (descritores curtos), modo usuario, timer + GIC. */
#include <stdint.h>

void test_putc(char c);
uint32_t enter_usr(uint32_t pc, uint32_t sp);
void usr_code(void);

#define MCR(cp, op1, crn, crm, op2, v) \
    __asm__ volatile("mcr " #cp ", " #op1 ", %0, " #crn ", " #crm ", " #op2 ::"r"((uint32_t)(v)) : "memory")
#define MRC(cp, op1, crn, crm, op2) \
    ({ uint32_t _v; __asm__ volatile("mrc " #cp ", " #op1 ", %0, " #crn ", " #crm ", " #op2 : "=r"(_v)); _v; })

static void puts_(const char *s)
{
    while (*s)
        test_putc(*s++);
}

static int fails;
static void check(const char *name, int ok)
{
    puts_(ok ? "  ok   " : "  FAIL ");
    puts_(name);
    test_putc('\n');
    if (!ok)
        fails++;
}

volatile uint32_t und_count, dfar, dfsr, irq_count, svc_arg;

void und_handler(uint32_t *regs) { und_count++; (void)regs; }
void svc_handler(uint32_t *regs) { svc_arg = regs[0]; regs[0] = regs[0] * 3; }
void dabt_handler(void)
{
    dfar = MRC(p15, 0, c6, c0, 0);
    dfsr = MRC(p15, 0, c5, c0, 0);
}

#define GICD 0x08000000u
#define GICC 0x08010000u
#define MMIO32(a) (*(volatile uint32_t *)(a))

void irq_handler(void)
{
    uint32_t iar = MMIO32(GICC + 0x0c);
    if ((iar & 0x3ff) == 27) {
        MCR(p15, 0, c14, c3, 1, 0);
        irq_count++;
    }
    MMIO32(GICC + 0x10) = iar;
}

static uint32_t l1[4096] __attribute__((aligned(16384)));
static uint32_t l2[256] __attribute__((aligned(1024)));
static uint32_t target_page[1024] __attribute__((aligned(4096)));
static uint32_t usr_stack[256] __attribute__((aligned(8)));

static uint64_t cntvct(void)
{
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void sys_tests(void)
{
    puts_("sys tests\n");

    /* 1. SVC e instrucao indefinida */
    register uint32_t r0 __asm__("r0") = 14;
    __asm__ volatile("svc #5" : "+r"(r0) :: "memory", "lr");
    check("svc", r0 == 42 && svc_arg == 14);
    __asm__ volatile("udf #0" ::: "memory");
    check("undef", und_count == 1);

    /* 2. MMU: secoes 1:1, pagina 4K em 0x80000000 */
    for (uint32_t i = 0; i < 4096; i++)
        l1[i] = 0;
    for (uint32_t i = 0; i < 0x400; i++) /* 0x00000000-0x3fffffff: dispositivos (so PL1) */
        l1[i] = (i << 20) | (1 << 10) | 2;
    for (uint32_t i = 0x400; i < 0x800; i++) /* RAM: so PL1, cacheavel */
        l1[i] = (i << 20) | (1 << 10) | (1 << 3) | (1 << 2) | 2;
    for (uint32_t i = 0xc00; i < 0xc10; i++) /* alias de usuario em 0xC0000000 (16 MiB) */
        l1[i] = ((i - 0x800) << 20) | (3 << 10) | (1 << 3) | (1 << 2) | 2;
    l1[0x800] = (uint32_t)l2 | 1;
    for (int i = 0; i < 256; i++)
        l2[i] = 0;
    l2[0] = (uint32_t)target_page | (3 << 4) | 2 | (1 << 3) | (1 << 2);
    target_page[0] = 0x11223344;
    MCR(p15, 0, c2, c0, 2, 0);           /* TTBCR */
    MCR(p15, 0, c2, c0, 0, (uint32_t)l1); /* TTBR0 */
    MCR(p15, 0, c3, c0, 0, 0x55555555);  /* DACR: todos cliente */
    MCR(p15, 0, c8, c7, 0, 0);           /* TLBIALL */
    __asm__ volatile("dsb; isb");
    uint32_t sctlr = MRC(p15, 0, c1, c0, 0);
    MCR(p15, 0, c1, c0, 0, sctlr | 1 | (1 << 2) | (1 << 12));
    __asm__ volatile("isb");
    volatile uint32_t *va = (volatile uint32_t *)0x80000000u;
    check("mmu read", *va == 0x11223344);
    *va = 0xcafef00d;
    check("mmu write", target_page[0] == 0xcafef00d);
    volatile uint32_t dummy = *(volatile uint32_t *)0x80001004u;
    (void)dummy;
    check("data abort", dfar == 0x80001004u && (dfsr & 0xf) == 7);
    MCR(p15, 0, c7, c8, 0, 0x80000010u); /* ATS1CPR */
    uint32_t par = MRC(p15, 0, c7, c4, 0);
    check("ats1cpr", !(par & 1) && (par & 0xfffff000u) == (uint32_t)target_page);

    /* 3. modo usuario via alias */
    uint32_t off = 0xc0000000u - 0x40000000u;
    uint32_t r = enter_usr((uint32_t)usr_code + off, (uint32_t)&usr_stack[256] + off);
    check("usr mode", r == 523);
    /* usuario nao acessa pagina so-PL1: verificado pela traducao */
    MCR(p15, 0, c7, c8, 2, 0x40000000u); /* ATS1CUR */
    par = MRC(p15, 0, c7, c4, 0);
    check("usr perm", (par & 1) != 0);

    /* 4. VFP */
    volatile double a = 1.5, b = 2.25;
    volatile float fa = 3.0f;
    double s = a * b + (double)fa;
    check("vfp", s == 6.375);

    /* 5. timer virtual + GIC */
    MMIO32(GICD) = 1;
    MMIO32(GICC) = 1;
    MMIO32(GICC + 4) = 0xff;
    MMIO32(GICD + 0x100) = 1u << 27;
    uint32_t freq = MRC(p15, 0, c14, c0, 0);
    check("cntfrq", freq == 62500000);
    uint64_t t0 = cntvct();
    MCR(p15, 0, c14, c3, 0, freq / 100);
    MCR(p15, 0, c14, c3, 1, 1);
    __asm__ volatile("cpsie i");
    for (int i = 0; i < 1000 && !irq_count; i++)
        __asm__ volatile("wfi");
    __asm__ volatile("cpsid i");
    uint64_t t1 = cntvct();
    check("timer irq", irq_count == 1);
    check("timer delay", t1 - t0 >= freq / 100);

    puts_(fails ? "SYS FAIL\n" : "SYS OK\n");
}
