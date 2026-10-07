@ Inicializacao bare-metal ARMv7 (maquina virt) e vetores de excecao.
    .syntax unified
    .arch armv7-a
    .arch_extension virt
    .fpu vfpv3
    .arm

    .section .text.start, "ax"
    .global _start
    .type _start, %function
_start:
    cpsid if
    @ pilhas por modo
    cps #0x12               @ IRQ
    ldr sp, =irq_stack_top
    cps #0x17               @ ABT
    ldr sp, =abt_stack_top
    cps #0x1b               @ UND
    ldr sp, =und_stack_top
    cps #0x13               @ SVC
    ldr sp, =stack_top
    @ VFP: acesso total a cp10/cp11 e FPEXC.EN
    mrc p15, 0, r0, c1, c0, 2
    orr r0, r0, #(0xf << 20)
    mcr p15, 0, r0, c1, c0, 2
    isb
    mov r0, #0x40000000
    vmsr fpexc, r0
    @ zera BSS
    ldr r1, =__bss_start
    ldr r2, =__bss_end
    mov r3, #0
1:  cmp r1, r2
    strblo r3, [r1], #1
    blo 1b
    @ vetores
    ldr r0, =vectors
    mcr p15, 0, r0, c12, c0, 0
    bl test_main
    bl sys_tests
    ldr r0, =0x84000008     @ PSCI SYSTEM_OFF
    hvc #0
2:  wfi
    b 2b

    .global test_putc
    .type test_putc, %function
test_putc:
    ldr r1, =0x09000000
    strb r0, [r1]
    bx lr

    .balign 32
vectors:
    b .                     @ reset
    b und_entry
    b svc_entry
    b pabt_entry
    b dabt_entry
    b .
    b irq_entry
    b .

und_entry:
    push {r0-r12, lr}
    mov r0, sp
    bl und_handler
    pop {r0-r12, lr}
    movs pc, lr             @ LR ja aponta para a proxima instrucao

svc_entry:
    push {r0-r12, lr}
    movw r1, #0xabcd
    cmp r7, r1
    beq svc_from_user
    mov r0, sp
    bl svc_handler
    pop {r0-r12, lr}
    movs pc, lr
svc_from_user:
    @ volta ao contexto salvo por enter_usr (r0 = resultado do usuario)
    ldr r1, [sp]            @ r0 salvo = resultado
    ldr r2, =saved_ctx
    ldr sp, [r2]
    str r1, [sp]            @ substitui r0 salvo
    pop {r0-r12, pc}

pabt_entry:
    b .

dabt_entry:
    push {r0-r12, lr}
    bl dabt_handler
    pop {r0-r12, lr}
    subs pc, lr, #4         @ pula a instrucao que falhou

irq_entry:
    sub lr, lr, #4
    push {r0-r3, r12, lr}
    bl irq_handler
    ldmfd sp!, {r0-r3, r12, pc}^

@ uint32_t enter_usr(uint32_t pc, uint32_t sp)
    .global enter_usr
    .type enter_usr, %function
enter_usr:
    push {r0-r12, lr}
    ldr r2, =saved_ctx
    str sp, [r2]
    cps #0x1f               @ SYS: mesmos registradores do USR
    mov sp, r1
    cps #0x13
    mov r3, #0x10           @ SPSR = USR, ARM, IRQs habilitadas
    msr spsr_cxsf, r3
    mov lr, r0
    movs pc, lr

    .global usr_code
    .type usr_code, %function
usr_code:
    mov r0, #100
    add r0, r0, r0, lsl #2  @ 500
    push {r0}
    pop {r1}
    add r0, r1, #23         @ 523
    movw r7, #0xabcd
    svc #1
    b .

    .bss
    .balign 8
saved_ctx: .space 4
    .balign 8
    .space 4096
irq_stack_top:
    .space 4096
abt_stack_top:
    .space 4096
und_stack_top:
    .space 65536
stack_top:
