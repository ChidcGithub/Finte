OUTPUT_FORMAT(elf64-x86-64)
ENTRY(svc_add)
SECTIONS
{
    . = 0x120000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
