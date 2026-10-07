# Inicializacao bare-metal i386 (o carregador ELF32 entra em modo protegido plano).
    .code32
    .section .text.start, "ax"
    .global _start
_start:
    mov $stack_top, %esp
    mov %cr0, %eax
    and $~4, %eax
    or $2, %eax
    mov %eax, %cr0
    mov %cr4, %eax
    or $0x600, %eax
    mov %eax, %cr4
    fninit
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb
    call test_main
    call sys_tests
    mov $0x501, %dx
    out %al, %dx
1:  hlt
    jmp 1b

    .global test_putc
test_putc:
    mov 4(%esp), %eax
    mov $0x3f8, %dx
    out %al, %dx
    ret

    .macro ISR_NOERR n
    .global isr\n
isr\n:
    push $0
    push $\n
    jmp isr_common
    .endm
    .macro ISR_ERR n
    .global isr\n
isr\n:
    push $\n
    jmp isr_common
    .endm

    ISR_NOERR 0
    ISR_NOERR 3
    ISR_NOERR 6
    ISR_ERR 13
    ISR_ERR 14
    ISR_NOERR 32
    ISR_NOERR 128

isr_common:
    pusha
    push %esp
    call x86_handler
    add $4, %esp
    popa
    add $8, %esp
    iret

# saida do modo usuario: int $0x81 restaura o contexto salvo por enter_user
    .global isr129
isr129:
    mov saved_eax, %eax
    mov saved_esp, %esp
    pop %ebp
    pop %edi
    pop %esi
    pop %ebx
    ret

# uint32_t enter_user(uint32_t eip, uint32_t esp)
    .global enter_user
enter_user:
    mov 4(%esp), %eax
    mov 8(%esp), %ecx
    push %ebx
    push %esi
    push %edi
    push %ebp
    mov %esp, saved_esp
    push $0x23                  # SS
    push %ecx                   # ESP
    push $0x202                 # EFLAGS
    push $0x1b                  # CS
    push %eax                   # EIP
    mov $0x23, %dx
    mov %dx, %ds
    mov %dx, %es
    iret

    .global sysenter_entry
sysenter_entry:
    incl sysenter_count
    sysexit                     # volta para EDX com ESP = ECX

    .global user_code
user_code:
    mov $1000, %eax
    imul $3, %eax, %eax         # 3000
    push %eax
    pop %ebx
    int $0x80                   # handler soma 7 a ebx
    mov %esp, %ecx
    lea 1f, %edx
    sysenter
1:  lea 45(%ebx), %eax          # 3052
    mov %eax, saved_eax
    int $0x81
    ud2

    .bss
    .balign 16
    .global sysenter_count
sysenter_count: .long 0
saved_esp: .long 0
saved_eax: .long 0
    .balign 16
    .space 65536
stack_top:
