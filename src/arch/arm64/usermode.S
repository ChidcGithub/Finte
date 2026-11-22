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
