# Plataformas do fuzzer: usuario Linux (referencia no qemu-user) e bare-metal (MultiVM).
#if defined(__x86_64__)
    .text
#ifdef LINUX
    .globl _start
_start:
    and $-16, %rsp
    call fuzz_main
    mov $60, %eax
    xor %edi, %edi
    syscall
    .globl out_char
out_char:
    push %rdi
    mov $1, %eax
    mov $1, %edi
    mov %rsp, %rsi
    mov $1, %edx
    syscall
    pop %rdi
    ret
#else
    .section .text.start, "ax"
    .globl _start
_start:
    lea stk_top(%rip), %rsp
    mov %cr0, %rax
    and $~4, %rax
    or $2, %rax
    mov %rax, %cr0
    mov %cr4, %rax
    or $0x600, %rax
    mov %rax, %cr4
    call fuzz_main
    mov $0x501, %dx
    out %al, %dx
1:  hlt
    jmp 1b
    .text
    .globl out_char
out_char:
    mov %edi, %eax
    mov $0x3f8, %dx
    out %al, %dx
    ret
    .bss
    .balign 16
    .space 65536
stk_top:
#endif
#else
    .text
#ifdef LINUX
    .globl _start
_start:
    and $-16, %esp
    call fuzz_main
    mov $1, %eax
    xor %ebx, %ebx
    int $0x80
    .globl out_char
out_char:
    push %ebx
    mov $4, %eax
    mov $1, %ebx
    lea 8(%esp), %ecx
    mov $1, %edx
    int $0x80
    pop %ebx
    ret
#else
    .section .text.start, "ax"
    .globl _start
_start:
    mov $stk_top, %esp
    mov %cr0, %eax
    and $~4, %eax
    or $2, %eax
    mov %eax, %cr0
    mov %cr4, %eax
    or $0x600, %eax
    mov %eax, %cr4
    call fuzz_main
    mov $0x501, %dx
    out %al, %dx
1:  hlt
    jmp 1b
    .text
    .globl out_char
out_char:
    mov 4(%esp), %eax
    mov $0x3f8, %dx
    out %al, %dx
    ret
    .bss
    .balign 16
    .space 65536
stk_top:
#endif
#endif
