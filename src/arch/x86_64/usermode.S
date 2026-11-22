/* User-mode entry, GDT reload and int 0x80 syscall gate. */
.text
.code64

.globl um_load_gdt
.type um_load_gdt, @function
um_load_gdt:
    lgdt (%rdi)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    ret
.size um_load_gdt, . - um_load_gdt

.globl um_enter
.type um_enter, @function
um_enter:
    /* rdi = user RIP, rsi = user stack top, rdx = RFLAGS */
    pushq $0x1B              /* user SS */
    pushq %rsi               /* user RSP */
    pushq %rdx               /* RFLAGS */
    pushq $0x23              /* user CS */
    pushq %rdi               /* user RIP */
    iretq
.size um_enter, . - um_enter

.globl isr128
.type isr128, @function
isr128:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi
    call usermode_syscall
    jmp um_restore
.size isr128, . - isr128

.globl um_restore
.type um_restore, @function
um_restore:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    iretq
.size um_restore, . - um_restore

.globl um_enter_frame
.type um_enter_frame, @function
um_enter_frame:
    /* rdi = saved 160-byte frame, rsi = kernel stack top */
    sub $160, %rsi
    mov %rdi, %r8
    mov %rsi, %r9
    mov %r9, %rdi
    mov %r8, %rsi
    mov $20, %rcx
    rep movsq
    mov %r9, %rsp
    jmp um_restore
.size um_enter_frame, . - um_enter_frame

.section .note.GNU-stack, "", @progbits
