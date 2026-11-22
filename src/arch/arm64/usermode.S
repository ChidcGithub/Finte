/* AArch64 EL0 entry. x0 = entry, x1 = user stack, x2 = kernel stack top. */
.text
.globl arm64_um_enter
.type arm64_um_enter, %function
arm64_um_enter:
    msr sp_el0, x1
    msr elr_el1, x0
    msr spsr_el1, xzr
    mov sp, x2
    isb
    eret
.size arm64_um_enter, . - arm64_um_enter

/* x0 = saved 288-byte exception frame, x1 = kernel stack top. */
.globl arm64_um_enter_frame
.type arm64_um_enter_frame, %function
arm64_um_enter_frame:
    sub x1, x1, #288
    mov x2, #36
1:
    ldr x3, [x0], #8
    str x3, [x1], #8
    subs x2, x2, #1
    b.ne 1b
    sub sp, x1, #288
    b arm64_restore_frame
.size arm64_um_enter_frame, . - arm64_um_enter_frame
