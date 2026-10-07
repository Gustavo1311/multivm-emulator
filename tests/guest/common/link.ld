ENTRY(_start)
SECTIONS
{
    . = LOAD_ADDR;
    .text : { *(.text.start) *(.text .text.*) }
    . = ALIGN(16);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(16);
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) *(.ARM.exidx*) }
}
