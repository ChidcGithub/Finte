OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(svc_add)
SECTIONS
{
    . = 0x120000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
