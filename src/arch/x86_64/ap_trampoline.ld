OUTPUT_FORMAT(elf64-x86-64)
ENTRY(ap_trampoline_start)
SECTIONS
{
    . = 0x8000;
    .text : { *(.text*) }
    /DISCARD/ : { *(.note*) *(.comment*) }
}
