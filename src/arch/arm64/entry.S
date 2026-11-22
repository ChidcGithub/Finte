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
    ic iallu
    dsb nsh
    isb
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
    b arm64_sync_entry
    .balign 128
    b arm64_irq_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_irq_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_irq_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_irq_entry
    .balign 128
    b arm64_sync_entry
    .balign 128
    b arm64_sync_entry

/* 288-byte exception frame:
 * x0..x30, ELR, SPSR, ESR, FAR. */
.macro SAVE_FRAME
    sub sp, sp, #288
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x9, elr_el1
    str x9, [sp, #248]
    mrs x9, spsr_el1
    str x9, [sp, #256]
    mrs x9, esr_el1
    str x9, [sp, #264]
    mrs x9, far_el1
    str x9, [sp, #272]
    mrs x9, sp_el0
    str x9, [sp, #280]
.endm

.macro RESTORE_FRAME
    ldr x9, [sp, #248]
    msr elr_el1, x9
    ldr x9, [sp, #256]
    msr spsr_el1, x9
    ldr x9, [sp, #280]
    msr sp_el0, x9
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #288
    eret
.endm

arm64_irq_entry:
    SAVE_FRAME
    mov x0, sp
    bl arm64_irq_handler
    RESTORE_FRAME

arm64_sync_entry:
    SAVE_FRAME
    mov x0, sp
    bl arm64_sync_handler
    RESTORE_FRAME
