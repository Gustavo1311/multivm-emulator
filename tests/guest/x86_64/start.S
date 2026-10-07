# Inicializacao bare-metal x86-64 (o carregador ELF64 ja entra em modo longo).
    .code64
    .section .text.start, "ax"
    .global _start
_start:
    lea stack_top(%rip), %rsp
    mov %cr0, %rax
    and $~4, %rax               # EM = 0
    or $2, %rax                 # MP = 1
    mov %rax, %cr0
    mov %cr4, %rax
    or $0x600, %rax             # OSFXSR | OSXMMEXCPT
    mov %rax, %cr4
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
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
    mov %edi, %eax
    mov $0x3f8, %dx
    out %al, %dx
    ret

# --- stubs de interrupcao: empilham (erro), vetor e desviam para o comum ---
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
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi
    call x64_handler
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    add $16, %rsp
    iretq

# uint64_t enter_user(uint64_t rip, uint64_t rsp): executa em anel 3 ate SYSCALL
    .global enter_user
enter_user:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, saved_rsp(%rip)
    mov %rdi, %rcx              # RIP do usuario
    mov %rsi, %rsp              # pilha do usuario (trocada antes do sysret)
    mov $0x202, %r11            # RFLAGS com IF
    sysretq

    .global syscall_entry
syscall_entry:
    mov saved_rsp(%rip), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret                         # rax = valor do usuario

# codigo de usuario (independente de posicao)
    .global user_code
user_code:
    mov $1000, %rax
    imul $3, %rax, %rax         # 3000
    push %rax
    pop %rbx
    int $0x80                   # handler soma 7 a rbx
    lea 45(%rbx), %rax          # 3052
    syscall
    ud2

    .bss
    .balign 16
saved_rsp: .quad 0
    .balign 16
    .space 65536
stack_top:
