/* Testes de sistema x86-64: IDT, excecoes, paginacao, anel 3, SYSCALL, TSS, PIT/PIC. */
#include <stdint.h>

void test_putc(char c);
uint64_t enter_user(uint64_t rip, uint64_t rsp);
void syscall_entry(void);
void user_code(void);
extern char isr0[], isr3[], isr6[], isr13[], isr14[], isr32[], isr128[];

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

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p)); }
static inline uint64_t rdmsr(uint32_t m)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t m, uint64_t v)
{
    __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vec, err, rip, cs, rflags, rsp, ss;
} frame;

volatile uint64_t cnt[256], last_err, last_cr2, user_int_cs;
/* pagina "copy-on-write": o #PF a torna gravavel e reinicia a instrucao */
static volatile uint64_t *cow_pte;
static volatile uint64_t cow_addr;

void x64_handler(frame *f)
{
    cnt[f->vec & 255]++;
    switch (f->vec) {
    case 0: f->rip += 3; break;   /* div %rcx */
    case 6: f->rip += 2; break;   /* ud2 */
    case 13: last_err = f->err; f->rip += 3; break;
    case 14:
        last_err = f->err;
        __asm__ volatile("mov %%cr2, %0" : "=r"(last_cr2));
        if (cow_pte && (last_cr2 & ~0xfffULL) == cow_addr) {
            *cow_pte |= 2; /* torna gravavel e repete a instrucao */
            __asm__ volatile("invlpg (%0)" ::"r"(cow_addr) : "memory");
            break;
        }
        f->rip += 3;              /* mov (%rax), %rbx */
        break;
    case 32: outb(0x20, 0x20); break;
    case 128: user_int_cs = f->cs; f->rbx += 7; break;
    default: break;
    }
}

static uint64_t gdt[8] __attribute__((aligned(16)));
static struct { uint16_t lim; uint64_t base; } __attribute__((packed)) gdtr, idtr;
static uint64_t idt[512] __attribute__((aligned(16)));
static uint8_t tss[104] __attribute__((aligned(16)));
static uint8_t kstack[16384] __attribute__((aligned(16)));
static uint8_t ustack[8192] __attribute__((aligned(16)));
static uint64_t pml4[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd0[512] __attribute__((aligned(4096)));
static uint64_t pd1[512] __attribute__((aligned(4096)));
static uint64_t pt1[512] __attribute__((aligned(4096)));
static uint64_t target[512] __attribute__((aligned(4096)));
static uint64_t cowpage[512] __attribute__((aligned(4096)));

static volatile uint64_t *cow_arm(uint64_t v)
{
    cowpage[0] = v;
    *cow_pte = (uint64_t)cowpage | 1; /* somente leitura */
    __asm__ volatile("invlpg (%0)" ::"r"(cow_addr) : "memory");
    return (volatile uint64_t *)cow_addr;
}

static void set_gate(int vec, void *h, int dpl)
{
    uint64_t a = (uint64_t)h;
    idt[vec * 2] = (a & 0xffff) | (0x08ULL << 16) | ((uint64_t)(0x8e | (dpl << 5)) << 40) | ((a >> 16) << 48);
    idt[vec * 2 + 1] = a >> 32;
}

void sys_tests(void)
{
    puts_("sys tests\n");
    /* padrao no framebuffer linear (0xFD000000) antes de trocar as tabelas de pagina */
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 256; x++)
            ((volatile uint32_t *)0xfd000000UL)[y * 320 + x] = ((uint32_t)x << 16) | ((uint32_t)y << 8) | 0x40;

    /* GDT: 0x08 codigo64 k, 0x10 dados k, 0x18 dados u, 0x20 codigo64 u, 0x28 TSS */
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffULL;
    gdt[2] = 0x00cf92000000ffffULL;
    gdt[3] = 0x00cff2000000ffffULL;
    gdt[4] = 0x00affa000000ffffULL;
    uint64_t tb = (uint64_t)tss;
    *(uint64_t *)(tss + 4) = (uint64_t)&kstack[sizeof(kstack)]; /* RSP0 */
    *(uint16_t *)(tss + 0x66) = 104;
    gdt[5] = 103 | ((tb & 0xffffff) << 16) | (0x89ULL << 40) | (((tb >> 24) & 0xff) << 56);
    gdt[6] = tb >> 32;
    gdtr.lim = sizeof(gdt) - 1;
    gdtr.base = (uint64_t)gdt;
    __asm__ volatile("lgdt %0" ::"m"(gdtr));
    __asm__ volatile("pushq $0x08; lea 1f(%%rip), %%rax; pushq %%rax; lretq; 1:\n"
                     "mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es; mov %%ax, %%ss" ::: "rax", "memory");
    __asm__ volatile("mov $0x28, %%ax; ltr %%ax" ::: "rax");

    set_gate(0, isr0, 0);
    set_gate(3, isr3, 3);
    set_gate(6, isr6, 0);
    set_gate(13, isr13, 0);
    set_gate(14, isr14, 0);
    set_gate(32, isr32, 0);
    set_gate(128, isr128, 3);
    idtr.lim = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt;
    __asm__ volatile("lidt %0" ::"m"(idtr));

    /* 1. excecoes */
    __asm__ volatile("int3");
    check("int3", cnt[3] == 1);
    __asm__ volatile("xor %%edx, %%edx; mov $5, %%eax; xor %%ecx, %%ecx; .byte 0x48, 0xf7, 0xf1" ::: "rax", "rcx", "rdx");
    check("#DE", cnt[0] == 1);
    __asm__ volatile("ud2");
    check("#UD", cnt[6] == 1);
    __asm__ volatile("mov $0x8000000000000000, %%rax; .byte 0x48, 0x8b, 0x18" ::: "rax", "rbx"); /* nao canonico */
    check("#GP", cnt[13] == 1);

    /* 2. paginacao propria: 1 GiB 1:1 (paginas de 2 MiB, U=1) + 0x40000000 com paginas de 4K */
    for (int i = 0; i < 512; i++) {
        pd0[i] = ((uint64_t)i << 21) | 0x87;
        pd1[i] = 0;
        pt1[i] = 0;
    }
    pml4[0] = (uint64_t)pdpt | 7;
    pdpt[0] = (uint64_t)pd0 | 7;
    pdpt[1] = (uint64_t)pd1 | 7;
    pd1[0] = (uint64_t)pt1 | 3;
    pt1[0] = (uint64_t)target | 3;
    target[0] = 0x0123456789abcdefULL;
    __asm__ volatile("mov %0, %%cr3" ::"r"((uint64_t)pml4) : "memory");
    volatile uint64_t *va = (volatile uint64_t *)0x40000000ULL;
    check("paging read", *va == 0x0123456789abcdefULL);
    *va = 42;
    check("paging write", target[0] == 42);
    check("dirty bit", (pt1[0] & 0x60) == 0x60);
    __asm__ volatile("mov $0x40001000, %%rax; .byte 0x48, 0x8b, 0x18" ::: "rax", "rbx");
    check("#PF", cnt[14] == 1 && last_cr2 == 0x40001000ULL && last_err == 0);
    /* escrita em pagina somente leitura */
    pt1[2] = (uint64_t)target | 1;
    __asm__ volatile("invlpg (%0)" ::"r"(0x40002000ULL) : "memory");
    __asm__ volatile("mov $0x40002000, %%rax; .byte 0x48, 0x89, 0x18" ::: "rax", "rbx");
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    check("#PF (WP)", (cr0 & 0x10000) && cnt[14] == 2 && last_err == 3);

    /* reinicio apos #PF de escrita (como no COW do Linux): a instrucao nao pode
     * alterar registradores/flags que ela mesma le antes de a escrita falhar */
    cow_addr = 0x40003000ULL;
    cow_pte = &pt1[3];
    {
        volatile uint64_t *m = cow_arm(10);
        uint64_t rb = 5;
        __asm__ volatile("lock xaddq %0, (%1)" : "+r"(rb) : "r"(m) : "memory", "cc");
        check("xadd reiniciado", cowpage[0] == 15 && rb == 10);

        m = cow_arm(7);
        uint64_t ra = 3, rc = 9;
        __asm__ volatile("lock cmpxchgq %2, (%1)" : "+a"(ra) : "r"(m), "r"(rc) : "memory", "cc");
        check("cmpxchg (falha) reiniciado", cowpage[0] == 7 && ra == 7);

        m = cow_arm(1);
        __asm__ volatile("stc; adcq $0, (%0)" ::"r"(m) : "memory", "cc");
        check("adc reiniciado", cowpage[0] == 2);

        m = cow_arm(0);
        uint64_t sp0, sp1;
        __asm__ volatile("mov %%rsp, %0; pushq $0x55; popq (%2); mov %%rsp, %1" : "=&r"(sp0), "=&r"(sp1) : "r"(m) : "memory");
        check("pop m64 reiniciado", cowpage[0] == 0x55 && sp0 == sp1);

        /* #PF dentro de um bloco do JIT: o RIP da falta deve ser o da instrucao que
         * acessou a memoria (senao o inc anterior roda duas vezes) */
        uint64_t n = 0;
        uint8_t cf = 1;
        m = cow_arm(1);
        __asm__ volatile("incq %0; lock btsq $3, (%2); setc %1" : "+r"(n), "=r"(cf) : "r"(m) : "memory", "cc");
        check("bts m64 reiniciado", n == 1 && cf == 0 && cowpage[0] == 9);
        m = cow_arm(9);
        __asm__ volatile("incq %0; lock btrq $0, (%2); setc %1" : "+r"(n), "=r"(cf) : "r"(m) : "memory", "cc");
        check("btr m64 reiniciado", n == 2 && cf == 1 && cowpage[0] == 8);
        m = cow_arm(8);
        __asm__ volatile("incq %0; btcl $31, (%1)" : "+r"(n) : "r"(m) : "memory", "cc");
        check("btc m32 reiniciado", n == 3 && cowpage[0] == 0x80000008ULL);
        m = cow_arm(0x8000000000000001ULL);
        __asm__ volatile("incq %0; rolq $4, (%1); addq $1, %0" : "+r"(n) : "r"(m) : "memory", "cc");
        check("rol m64 reiniciado", n == 5 && cowpage[0] == 0x18);
        m = cow_arm(3);
        __asm__ volatile("incq %0; shlq $2, (%1)" : "+r"(n) : "r"(m) : "memory", "cc");
        check("shl m64 reiniciado", n == 6 && cowpage[0] == 12);
        m = cow_arm(5);
        uint64_t mx = 7, md = 0;
        __asm__ volatile("incq %0; mulq (%3); addq $0, (%3)" : "+r"(n), "+a"(mx), "+d"(md) : "r"(m) : "memory", "cc");
        check("mul m64 + add reiniciado", n == 7 && mx == 35 && cowpage[0] == 5);
        m = cow_arm(0);
        __asm__ volatile("incq %0; pcmpeqd %%xmm0, %%xmm0; movdqu %%xmm0, (%1)" : "+r"(n) : "r"(m) : "memory", "xmm0");
        check("movdqu m128 reiniciado", n == 8 && cowpage[0] == ~0ULL && cowpage[1] == ~0ULL);
        m = cow_arm(4);
        uint64_t acc = 4, nv = 6;
        __asm__ volatile("incq %0; lock cmpxchgq %2, (%3)" : "+r"(n), "+a"(acc) : "r"(nv), "r"(m) : "memory", "cc");
        check("cmpxchg m64 (inc antes) reiniciado", n == 9 && acc == 4 && cowpage[0] == 6);
        m = cow_arm(10);
        uint64_t xv = 5;
        __asm__ volatile("incq %0; lock xaddq %1, (%2)" : "+r"(n), "+r"(xv) : "r"(m) : "memory", "cc");
        check("xadd m64 (inc antes) reiniciado", n == 10 && xv == 10 && cowpage[0] == 15);
    }
    cow_pte = 0;

    /* 3. anel 3: SYSRET -> usuario -> INT 0x80 (TSS) -> SYSCALL */
    wrmsr(0xc0000081, (0x10ULL << 48) | (0x08ULL << 32)); /* STAR */
    wrmsr(0xc0000082, (uint64_t)syscall_entry);         /* LSTAR */
    wrmsr(0xc0000084, 0x200);                            /* SFMASK: IF */
    wrmsr(0xc0000080, rdmsr(0xc0000080) | 1);            /* SCE */
    uint64_t r = enter_user((uint64_t)user_code, (uint64_t)&ustack[sizeof(ustack)]);
    check("ring3 + syscall", r == 3052);
    check("int 0x80 from ring3", cnt[128] == 1 && (user_int_cs & 3) == 3);

    /* 4. CPUID / RDTSC */
    uint32_t a, b, c2, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c2), "=d"(d) : "a"(0x80000001), "c"(0));
    check("cpuid lm", (d >> 29) & 1);
    uint64_t t0 = __builtin_ia32_rdtsc();

    /* 5. PIT (IRQ0) via PIC reprogramado para 0x20 */
    outb(0x20, 0x11); outb(0xa0, 0x11);
    outb(0x21, 0x20); outb(0xa1, 0x28);
    outb(0x21, 0x04); outb(0xa1, 0x02);
    outb(0x21, 0x01); outb(0xa1, 0x01);
    outb(0x21, 0xfe); outb(0xa1, 0xff);
    outb(0x43, 0x34);
    outb(0x40, 11932 & 0xff);
    outb(0x40, 11932 >> 8);
    __asm__ volatile("sti");
    for (int i = 0; i < 1000 && cnt[32] < 5; i++)
        __asm__ volatile("hlt");
    __asm__ volatile("cli");
    uint64_t t1 = __builtin_ia32_rdtsc();
    check("pit irq", cnt[32] >= 5);
    check("tsc", t1 - t0 >= 40000000ULL); /* >= 40 ms a 1 GHz */

    /* 6. codigo automodificavel: escrita em dado na mesma pagina do codigo nao pode
     * deixar codigo velho, e escrita no proprio bloco em execucao vale na hora */
    {
        static uint8_t smc[4096] __attribute__((aligned(4096)));
        volatile uint8_t *p = smc;
        typedef int (*fn)(void);
        fn f0 = (fn)(void *)smc, f1 = (fn)(void *)(smc + 0x100);
        p[0] = 0xb8; p[1] = 1; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0xc3; /* mov eax, 1; ret */
        int r = 0;
        for (int i = 0; i < 100; i++)
            r += f0();
        check("smc: codigo novo", r == 100);
        for (int i = 0; i < 100; i++) {
            p[2048 + (i & 63)] = (uint8_t)i; /* dado em outro trecho da pagina */
            p[40] = (uint8_t)i;              /* dado logo depois do codigo */
            r += f0();
        }
        check("smc: dado vizinho", r == 200);
        p[1] = 2; /* muda o imediato */
        check("smc: imediato alterado", f0() == 2);
        /* mov byte [rip+1], 5 ; mov eax, 1 ; ret -> o mov grava o imediato da instrucao seguinte */
        static const uint8_t self[] = {0xc6, 0x05, 1, 0, 0, 0, 5, 0xb8, 1, 0, 0, 0, 0xc3};
        int ok = 1;
        for (int i = 0; i < 50; i++) {
            for (unsigned k = 0; k < sizeof(self); k++)
                p[0x100 + k] = self[k];
            ok &= f1() == 5;
        }
        check("smc: proprio bloco", ok);
    }

    puts_(fails ? "SYS FAIL\n" : "SYS OK\n");
    (void)hex;
}
