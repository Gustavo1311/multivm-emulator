// Inicializacao bare-metal AArch64 para os testes (maquina virt).
    .section .text.start, "ax"
    .global _start
_start:
    ldr x1, =stack_top
    mov sp, x1
    mov x1, #(3 << 20)          // CPACR_EL1.FPEN = 11: habilita FP/SIMD
    msr cpacr_el1, x1
    isb
    ldr x1, =__bss_start
    ldr x2, =__bss_end
1:  cmp x1, x2
    b.hs 2f
    strb wzr, [x1], #1
    b 1b
2:  bl test_main
    bl sys_tests
    ldr w0, =0x84000008         // PSCI SYSTEM_OFF
    hvc #0
3:  wfi
    b 3b

    .global test_putc
test_putc:
    ldr x1, =0x09000000
    strb w0, [x1]
    ret

    .bss
    .balign 16
    .space 65536
stack_top:
