/* Testes de sistema i386: IDT, excecoes, paginacao 2 niveis (PSE), TSS/anel 3, SYSENTER, PIT. */
#include <stdint.h>

void test_putc(char c);
uint32_t enter_user(uint32_t eip, uint32_t esp);
void sysenter_entry(void);
void user_code(void);
extern volatile uint32_t sysenter_count;
extern char isr0[], isr3[], isr6[], isr13[], isr14[], isr32[], isr128[], isr129[];

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

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p)); }
static inline void wrmsr(uint32_t m, uint64_t v)
{
    __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

typedef struct {
    uint32_t edi, esi, ebp, esp0, ebx, edx, ecx, eax;
    uint32_t vec, err, eip, cs, eflags, esp, ss;
} frame;

volatile uint32_t cnt[256], last_err, last_cr2, user_cs;

void x86_handler(frame *f)
{
    cnt[f->vec & 255]++;
    switch (f->vec) {
    case 0: f->eip += 2; break;
    case 6: f->eip += 2; break;
    case 13: last_err = f->err; f->eip += 2; break;
    case 14:
        last_err = f->err;
        __asm__ volatile("mov %%cr2, %0" : "=r"(last_cr2));
        f->eip += 2;
        break;
    case 32: outb(0x20, 0x20); break;
    case 128: user_cs = f->cs; f->ebx += 7; break;
    default: break;
    }
}

static uint64_t gdt[6] __attribute__((aligned(8)));
static struct { uint16_t lim; uint32_t base; } __attribute__((packed)) gdtr, idtr;
static uint64_t idt[256] __attribute__((aligned(8)));
static uint32_t tss[26] __attribute__((aligned(8)));
static uint8_t kstack[16384] __attribute__((aligned(16)));
static uint8_t ustack[8192] __attribute__((aligned(16)));
static uint32_t pd[1024] __attribute__((aligned(4096)));
static uint32_t pt[1024] __attribute__((aligned(4096)));
static uint32_t target[1024] __attribute__((aligned(4096)));

static uint64_t seg_desc(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    return (limit & 0xffff) | ((uint64_t)(base & 0xffffff) << 16) | ((uint64_t)access << 40) |
           ((uint64_t)((limit >> 16) & 0xf) << 48) | ((uint64_t)flags << 52) | ((uint64_t)(base >> 24) << 56);
}

static void set_gate(int vec, void *h, int dpl)
{
    uint32_t a = (uint32_t)h;
    idt[vec] = (a & 0xffff) | (0x08ULL << 16) | ((uint64_t)(0x8e | (dpl << 5)) << 40) | ((uint64_t)(a >> 16) << 48);
}

void sys_tests(void)
{
    puts_("sys tests\n");
    gdt[0] = 0;
    gdt[1] = seg_desc(0, 0xfffff, 0x9a, 0xc);
    gdt[2] = seg_desc(0, 0xfffff, 0x92, 0xc);
    gdt[3] = seg_desc(0, 0xfffff, 0xfa, 0xc);
    gdt[4] = seg_desc(0, 0xfffff, 0xf2, 0xc);
    tss[1] = (uint32_t)&kstack[sizeof(kstack)]; /* ESP0 */
    tss[2] = 0x10;                              /* SS0 */
    tss[25] = 104 << 16;                        /* base do mapa de E/S alem do limite */
    gdt[5] = seg_desc((uint32_t)tss, 103, 0x89, 0);
    gdtr.lim = sizeof(gdt) - 1;
    gdtr.base = (uint32_t)gdt;
    __asm__ volatile("lgdt %0" ::"m"(gdtr));
    __asm__ volatile("ljmp $0x08, $1f; 1: mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es; mov %%ax, %%ss" ::: "eax", "memory");
    __asm__ volatile("mov $0x28, %%ax; ltr %%ax" ::: "eax");

    set_gate(0, isr0, 0);
    set_gate(3, isr3, 3);
    set_gate(6, isr6, 0);
    set_gate(13, isr13, 0);
    set_gate(14, isr14, 0);
    set_gate(32, isr32, 0);
    set_gate(128, isr128, 3);
    set_gate(129, isr129, 3);
    idtr.lim = sizeof(idt) - 1;
    idtr.base = (uint32_t)idt;
    __asm__ volatile("lidt %0" ::"m"(idtr));

    __asm__ volatile("int3");
    check("int3", cnt[3] == 1);
    __asm__ volatile("xor %%edx, %%edx; mov $5, %%eax; xor %%ecx, %%ecx; .byte 0xf7, 0xf1" ::: "eax", "ecx", "edx");
    check("#DE", cnt[0] == 1);
    __asm__ volatile("ud2");
    check("#UD", cnt[6] == 1);
    __asm__ volatile("mov $0x1234, %%ax; .byte 0x8e, 0xd8" ::: "eax");
    check("#GP", cnt[13] == 1 && last_err == 0x1234);
    __asm__ volatile("mov $0x10, %%ax; mov %%ax, %%ds" ::: "eax");

    /* paginacao: 1 GiB 1:1 com paginas de 4 MiB (U=1), 0x40000000 com PT de 4K */
    for (int i = 0; i < 1024; i++) {
        pd[i] = i < 256 ? ((uint32_t)i << 22) | 0x87 : 0;
        pt[i] = 0;
    }
    pd[256] = (uint32_t)pt | 3;
    pt[0] = (uint32_t)target | 3;
    target[0] = 0x13572468;
    __asm__ volatile("mov %%cr4, %%eax; or $0x10, %%eax; mov %%eax, %%cr4" ::: "eax");
    __asm__ volatile("mov %0, %%cr3" ::"r"(pd) : "memory");
    __asm__ volatile("mov %%cr0, %%eax; or $0x80010000, %%eax; mov %%eax, %%cr0" ::: "eax", "memory");
    volatile uint32_t *va = (volatile uint32_t *)0x40000000u;
    check("paging read", *va == 0x13572468);
    *va = 99;
    check("paging write", target[0] == 99 && (pt[0] & 0x60) == 0x60);
    __asm__ volatile("mov $0x40001000, %%eax; .byte 0x8b, 0x18" ::: "eax", "ebx");
    check("#PF", cnt[14] == 1 && last_cr2 == 0x40001000u && last_err == 0);

    /* anel 3 + int 0x80 + SYSENTER/SYSEXIT */
    wrmsr(0x174, 0x08);
    wrmsr(0x175, (uint32_t)&kstack[8192]);
    wrmsr(0x176, (uint32_t)sysenter_entry);
    uint32_t r = enter_user((uint32_t)user_code, (uint32_t)&ustack[sizeof(ustack)]);
    __asm__ volatile("mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es" ::: "eax");
    check("ring3", r == 3052);
    check("int 0x80 from ring3", cnt[128] == 1 && (user_cs & 3) == 3);
    check("sysenter/sysexit", sysenter_count == 1);

    /* PIT */
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
    check("pit irq", cnt[32] >= 5);

    /* x87 */
    volatile double x = 2.0;
    double s;
    __asm__ volatile("fldl %1; fsqrt; fstpl %0" : "=m"(s) : "m"(x));
    check("x87 fsqrt", s > 1.41421356 && s < 1.41421357);

    puts_(fails ? "SYS FAIL\n" : "SYS OK\n");
}
