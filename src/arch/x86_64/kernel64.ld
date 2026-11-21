/* The real 64-bit kernel, linked at 2 MiB and converted to a flat binary. */
ENTRY(_start64)

SECTIONS
{
    . = 64M;

    .text : ALIGN(16)
    {
        KEEP(*(.text.entry))
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
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    .bss.stack : ALIGN(16)
    {
        *(.bss.stack)
    }

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.note*)
        *(.comment*)
        *(.eh_frame*)
    }
}
