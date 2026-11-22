/* arm64 cooperative context switch.
 *
 * arm64_switch(&from->sp, to->sp) saves x19-x30 and DAIF, then returns on the
 * other stack. New frames are prepared by hal_ctx_create with x30 pointing at
 * arm64_trampoline. */
.text
.globl arm64_switch
.type arm64_switch, %function
arm64_switch:
    stp x19, x20, [sp, #-112]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mrs x9, daif
    str x9, [sp, #96]
    mrs x9, sp_el0
    str x9, [sp, #104]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldr x9, [sp, #96]
    msr daif, x9
    ldr x9, [sp, #104]
    msr sp_el0, x9
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #112
    ret
.size arm64_switch, . - arm64_switch

.globl arm64_trampoline
.type arm64_trampoline, %function
arm64_trampoline:
    mov x9, sp
    and x9, x9, #0xfffffffffffffff0
    mov sp, x9
    bl finte_hal_bootstrap
    brk #0
.size arm64_trampoline, . - arm64_trampoline
