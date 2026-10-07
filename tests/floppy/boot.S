/*
 * Setor de boot do teste de disquete: usa a INT 13h do SeaBIOS (que fala com o
 * FDC por DMA) para ler setores com padrao conhecido, gravar um setor e reler.
 * Resultado na serial: "FLOPPY OK" ou "FLOPPY FAIL x". Sai pela porta 0x501.
 *
 * Padrao da imagem (gerada por mkimg.py): o setor LBA k (k >= 1) tem todos os
 * bytes iguais a (k * 7 + 3) & 0xff. Geometria 1,44 MiB: 80 x 2 x 18.
 */
    .code16
    .text
    .globl _start
_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $0x7c00, %sp
    sti
    mov %dl, drive

    mov $msg_boot, %si
    call puts

    /* reset do controlador */
    xor %ah, %ah
    mov drive, %dl
    int $0x13
    mov $'R', %bl
    jc fail

    /* 1) trilha 0, face 0, setores 2..18 (LBA 1..17) em 0x1000:0 */
    mov $0x1000, %ax
    mov %ax, %es
    xor %bx, %bx
    mov $0x0211, %ax        /* ler 17 setores */
    mov $0x0002, %cx        /* C0 S2 */
    xor %dh, %dh            /* H0 */
    mov drive, %dl
    int $0x13
    mov $'1', %bl
    jc fail
    mov $1, %bp             /* primeiro LBA */
    mov $17, %di            /* quantidade */
    call check
    mov $'2', %bl
    jnz fail

    /* 2) cilindro 1, face 1, setores 1..18 (LBA 54..71) */
    xor %bx, %bx
    mov $0x0212, %ax
    mov $0x0101, %cx        /* C1 S1 */
    mov $0x01, %dh          /* H1 */
    mov drive, %dl
    int $0x13
    mov $'3', %bl
    jc fail
    mov $54, %bp
    mov $18, %di
    call check
    mov $'4', %bl
    jnz fail

    /* 3) grava 0xA5 no ultimo setor (C79 H1 S18, LBA 2879) e rele */
    push %es
    mov $0x2000, %ax
    mov %ax, %es
    xor %di, %di
    mov $0xa5, %al
    mov $512, %cx
    rep stosb
    xor %bx, %bx
    mov $0x0301, %ax
    mov $0x4f12, %cx        /* C79 S18 */
    mov $0x01, %dh
    mov drive, %dl
    int $0x13
    mov $'5', %bl
    jc fail_pop
    mov $0x3000, %ax
    mov %ax, %es
    xor %bx, %bx
    mov $0x0201, %ax
    mov $0x4f12, %cx
    mov $0x01, %dh
    mov drive, %dl
    int $0x13
    mov $'6', %bl
    jc fail_pop
    xor %di, %di
    mov $0xa5, %al
    mov $512, %cx
    repe scasb
    mov $'7', %bl
    jnz fail_pop
    pop %es

    mov $msg_ok, %si
    call puts
    jmp done

fail_pop:
    pop %es
fail:
    mov $msg_fail, %si
    call puts
    mov %bl, %al
    call putc
    mov $'\n', %al
    call putc
done:
    mov $0x501, %dx
    xor %al, %al
    out %al, %dx
1:  hlt
    jmp 1b

/* confere %di setores a partir de %es:0, LBA inicial %bp; ZF=1 se tudo certo */
check:
    xor %si, %si
2:  mov %bp, %ax
    mov $7, %cl
    mul %cl                 /* ax = lba * 7 (lba < 256) */
    add $3, %al
    cmp %es:(%si), %al
    jne 3f
    cmp %es:511(%si), %al
    jne 3f
    add $512, %si
    inc %bp
    dec %di
    jnz 2b
    xor %ax, %ax            /* ZF=1 */
3:  ret

putc:
    push %dx
    mov $0x3f8, %dx
    out %al, %dx
    mov $0x402, %dx
    out %al, %dx
    pop %dx
    ret

puts:
    lodsb
    test %al, %al
    jz 4f
    call putc
    jmp puts
4:  ret

drive:    .byte 0
msg_boot: .asciz "floppy boot\n"
msg_ok:   .asciz "FLOPPY OK\n"
msg_fail: .asciz "FLOPPY FAIL "

    .org 510
    .byte 0x55, 0xaa
