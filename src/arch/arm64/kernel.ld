ENTRY(_start)
SECTIONS
{
    . = 0x40080000;

    .text : {
        *(.text.boot)
        *(.text.vectors)
        *(.text*)
    }

    .rodata : { *(.rodata*) }

    .data : { *(.data*) }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
        . += 0x20000;
        stack_top = .;
    }

    /DISCARD/ : { *(.comment) *(.note*) }
}
