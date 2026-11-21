/* 32-bit Multiboot wrapper. The real 64-bit kernel is embedded as a flat
 * binary and linked at 2 MiB.
 */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text : ALIGN(16)
    {
        KEEP(*(.multiboot))
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16)
    {
        *(.rodata*)
    }

    .data : ALIGN(16)
    {
        *(.data*)
    }

    .bss : ALIGN(16)
    {
        *(.bss*)
        *(COMMON)
    }

    . = 64M;

    .kernel64 : ALIGN(16)
    {
        KEEP(*(.kernel64))
    }

    /DISCARD/ :
    {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}
