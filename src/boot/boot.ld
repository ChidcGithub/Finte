OUTPUT_FORMAT(binary)
ENTRY(_start)

SECTIONS
{
    . = 0x7C00;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    . = 0x7DFE;
    .signature : {
        SHORT(0xAA55)
    }
}
