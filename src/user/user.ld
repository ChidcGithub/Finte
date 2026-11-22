OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5); /* read + execute */
    data PT_LOAD FLAGS(6); /* read + write */
}
SECTIONS
{
    . = 0x100000000;
    .text : { *(.text.start) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data : { *(.data*) } :data
    .bss : { *(.bss*) *(COMMON) } :data
}
