/* Cooperative x86_64 context switch.
 *
 * kx_switch(&from->rsp, to->rsp) saves RFLAGS and the callee-saved register
 * set, then returns on the other stack. Saving RFLAGS lets a flow run with
 * interrupts enabled while the scheduler runs with them disabled. New stacks
 * are prepared by hal_ctx_create with a flags slot, six dummy callee-saved
 * slots, and kx_trampoline.
 */
.text
.code64
.globl kx_switch
.type kx_switch, @function
kx_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    pushfq
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    popfq
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.size kx_switch, . - kx_switch

.globl kx_trampoline
.type kx_trampoline, @function
kx_trampoline:
    and $-16, %rsp
    call finte_hal_bootstrap
    ud2
.size kx_trampoline, . - kx_trampoline

.section .note.GNU-stack, "", @progbits
