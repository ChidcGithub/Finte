OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)
SECTIONS
{
    . = 0x100000000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
