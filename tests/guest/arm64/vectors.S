// Tabela de vetores e ajudantes de EL0 para os testes de sistema.
    .macro SAVE
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    .endm
    .macro RESTORE
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
    .endm

    .text
    .balign 2048
    .global vectors
vectors:
    .balign 128             // EL1t sync
    b .
    .balign 128
    b .
    .balign 128
    b .
    .balign 128
    b .
    .balign 128             // EL1h sync
    b sync_entry
    .balign 128             // EL1h irq
    b irq_entry
    .balign 128
    b .
    .balign 128
    b .
    .balign 128             // EL0 sync
    b sync_entry
    .balign 128             // EL0 irq
    b irq_entry
    .balign 128
    b .
    .balign 128
    b .

sync_entry:
    SAVE
    mov x0, sp
    bl sync_handler
    RESTORE
    eret

irq_entry:
    SAVE
    bl irq_handler
    RESTORE
    eret

// uint64_t enter_el0(uint64_t pc, uint64_t sp): executa em EL0 ate "svc #1".
    .global enter_el0
enter_el0:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    mov x19, x30
    adr x20, 1f
    msr elr_el1, x0
    msr sp_el0, x1
    mov x2, #0              // SPSR: EL0t, interrupcoes habilitadas
    msr spsr_el1, x2
    eret
1:  ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    ret

// Codigo de EL0 (independente de posicao)
    .global el0_entry
el0_entry:
    mov x0, #12345
    add x1, x0, x0
    add x0, x0, x1          // 37035
    sub sp, sp, #16
    str x0, [sp]
    ldr x0, [sp]
    add sp, sp, #16
    svc #1
    b .
