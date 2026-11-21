/* First 64-bit instruction of the Finte kernel. */
.section .text.entry, "ax"
.code64
.globl _start64
.type _start64, @function
_start64:
    cli
    cld

    /* BIOS stage2 already installed flat data selectors. UEFI has its own
     * valid long-mode selectors; segment bases are ignored in long mode, so
     * do not disturb the firmware's GDT. */

    /* Keep the boot hand-off across the bss clear. */
    mov %rdi, %r12
    mov %rsi, %r13

    mov $stack_top, %rsp

    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    shr $3, %rcx
    xor %eax, %eax
    rep stosq

    mov %r12, %rdi
    mov %r13, %rsi
    and $-16, %rsp
    call kmain

2:  cli
    hlt
    jmp 2b
.size _start64, . - _start64

.section .bss.stack, "aw", @nobits
.align 16
stack_bottom:
    .skip 32768
stack_top:
