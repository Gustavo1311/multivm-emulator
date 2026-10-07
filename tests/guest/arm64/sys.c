/* Testes de sistema AArch64: excecoes, MMU, EL0, timer generico + GICv2. */
#include <stdint.h>

void test_putc(char c);
extern char vectors[];
void el0_entry(void);
uint64_t enter_el0(uint64_t pc, uint64_t sp);

#define RD(reg) ({ uint64_t _v; __asm__ volatile("mrs %0, " #reg : "=r"(_v)); _v; })
#define WR(reg, v) __asm__ volatile("msr " #reg ", %0" ::"r"((uint64_t)(v)))
#define ISB() __asm__ volatile("isb" ::: "memory")

static void puts_(const char *s)
{
    while (*s)
        test_putc(*s++);
}

static void hex(uint64_t v)
{
    for (int i = 60; i >= 0; i -= 4)
        test_putc("0123456789abcdef"[(v >> i) & 15]);
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

volatile uint64_t last_ec, last_far, sync_count, irq_count, el0_result;

/* chamado pelo vetor sincrono; regs[0..30] salvos na pilha */
void sync_handler(uint64_t *regs)
{
    uint64_t esr = RD(esr_el1), elr = RD(elr_el1);
    uint64_t ec = esr >> 26;
    last_ec = ec;
    sync_count++;
    switch (ec) {
    case 0x15: /* SVC */
        if ((esr & 0xffff) == 1) { /* retorno do EL0: volta para o rotulo em x20 */
            el0_result = regs[0];
            WR(spsr_el1, 0x3c5); /* EL1h, DAIF mascarado */
            WR(elr_el1, regs[20]);
            return;
        }
        regs[0] = regs[0] * 2 + 1;
        break;
    case 0x3c: /* BRK: pula */
        WR(elr_el1, elr + 4);
        break;
    case 0x24: case 0x25: /* data abort */
        last_far = RD(far_el1);
        WR(elr_el1, elr + 4);
        break;
    default:
        puts_("excecao inesperada EC=");
        hex(ec);
        puts_(" ELR=");
        hex(elr);
        test_putc('\n');
        for (;;)
            ;
    }
}

#define GICD 0x08000000UL
#define GICC 0x08010000UL
#define MMIO32(a) (*(volatile uint32_t *)(a))

void irq_handler(void)
{
    uint32_t iar = MMIO32(GICC + 0x0c);
    uint32_t id = iar & 0x3ff;
    if (id == 27) {
        WR(cntv_ctl_el0, 0);
        irq_count++;
    }
    MMIO32(GICC + 0x10) = iar;
}

static uint64_t l1[512] __attribute__((aligned(4096)));
static uint64_t l2[512] __attribute__((aligned(4096)));
static uint64_t l3[512] __attribute__((aligned(4096)));
static uint64_t target_page[512] __attribute__((aligned(4096)));
static uint64_t el0_stack[512] __attribute__((aligned(16)));

#define ALIAS 0x80000000UL /* diferenca entre o alias EL0 (0xC0000000) e a RAM */

void sys_tests(void)
{
    puts_("sys tests\n");
    /* padrao no framebuffer (se configurado com --fb 320x200 ou maior) */
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 256; x++)
            ((volatile uint32_t *)0x10000000UL)[y * 320 + x] = ((uint32_t)x << 16) | ((uint32_t)y << 8) | 0x40;
    WR(vbar_el1, (uint64_t)vectors);
    ISB();

    /* 1. SVC e BRK em EL1 */
    register uint64_t x0 __asm__("x0") = 20;
    __asm__ volatile("svc #7" : "+r"(x0) :: "memory");
    check("svc", x0 == 41 && last_ec == 0x15);
    __asm__ volatile("brk #3" ::: "memory");
    check("brk", last_ec == 0x3c);

    /* 2. MMU: 39 bits VA, granulo 4K */
    l1[0] = 0x00000000UL | (1 << 10) | (0 << 2) | 1;                  /* dispositivos */
    l1[1] = 0x40000000UL | (1 << 10) | (3 << 8) | (1 << 2) | 1;       /* RAM (EL1) */
    l1[2] = (uint64_t)l2 | 3;
    l1[3] = 0x40000000UL | (1 << 10) | (3 << 8) | (1 << 6) | (1 << 2) | 1; /* alias EL0 RW */
    l2[0] = (uint64_t)l3 | 3;
    l3[0] = (uint64_t)target_page | (1 << 10) | (3 << 8) | (1 << 2) | 3;
    target_page[0] = 0x1122334455667788ULL;
    WR(mair_el1, 0xff00);
    WR(tcr_el1, (25ULL << 0) | (1ULL << 23) | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (2ULL << 32));
    WR(ttbr0_el1, (uint64_t)l1);
    __asm__ volatile("dsb sy; tlbi vmalle1; dsb sy; isb" ::: "memory");
    WR(sctlr_el1, RD(sctlr_el1) | 1 | (1 << 2) | (1 << 12));
    ISB();
    volatile uint64_t *va = (volatile uint64_t *)0x80000000UL;
    check("mmu read", *va == 0x1122334455667788ULL);
    *va = 0xcafef00d;
    check("mmu write", target_page[0] == 0xcafef00d);
    last_far = 0;
    volatile uint64_t dummy = *(volatile uint64_t *)0x80001008UL;
    (void)dummy;
    check("data abort", last_far == 0x80001008UL && last_ec == 0x25);
    /* AT S1E1R */
    __asm__ volatile("at s1e1r, %0; isb" ::"r"(0x80000010UL));
    uint64_t par = RD(par_el1);
    check("at", !(par & 1) && (par & 0xfffffffff000ULL) == (uint64_t)target_page);

    /* 3. EL0 via alias em 0xC0000000 */
    uint64_t r = enter_el0((uint64_t)el0_entry + ALIAS, (uint64_t)&el0_stack[512] + ALIAS);
    check("el0", r == 12345 * 3);
    /* EL0 nao pode acessar a RAM do kernel (pagina so-EL1) */
    last_far = 0;

    /* 4. timer virtual + GIC */
    MMIO32(GICD + 0x000) = 1;
    MMIO32(GICC + 0x000) = 1;
    MMIO32(GICC + 0x004) = 0xff;
    MMIO32(GICD + 0x100) = 1u << 27;
    uint64_t freq = RD(cntfrq_el0);
    check("cntfrq", freq == 62500000);
    uint64_t t0 = RD(cntvct_el0);
    WR(cntv_tval_el0, freq / 100); /* 10 ms */
    WR(cntv_ctl_el0, 1);
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    for (int i = 0; i < 1000 && !irq_count; i++)
        __asm__ volatile("wfi");
    __asm__ volatile("msr daifset, #2" ::: "memory");
    uint64_t t1 = RD(cntvct_el0);
    check("timer irq", irq_count == 1);
    check("timer delay", t1 - t0 >= freq / 100);

    puts_(fails ? "SYS FAIL\n" : "SYS OK\n");
}
