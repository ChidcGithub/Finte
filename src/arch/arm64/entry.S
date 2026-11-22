/* QEMU virt arm64 entry and exception vectors. */
.section .text.boot, "ax"
.globl _start
_start:
    mov x19, x0
    mov x20, x1

    mrs x0, mpidr_el1
    and x0, x0, #0xff
    cbnz x0, park

    ldr x0, =vector_table
    msr vbar_el1, x0
    isb

    ldr x0, =stack_top
    mov sp, x0

    ldr x1, =__bss_start
    ldr x2, =__bss_end
1:
    cmp x1, x2
    b.hs 2f
    str xzr, [x1], #8
    b 1b
2:
    mov x0, x19
    mov x1, x20
    bl kmain_arm64

park:
    wfi
    b park

.section .text.vectors, "ax"
.align 11
.globl vector_table
vector_table:
    .rept 16
    b arm64_fault_entry
    .balign 128
    .endr

arm64_fault_entry:
    bl arm64_fault
1:
    wfi
    b 1b
