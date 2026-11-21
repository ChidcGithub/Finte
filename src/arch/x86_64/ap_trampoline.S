/* AP startup trampoline, copied to physical 0x8000 and entered by SIPI.
 *
 * Real mode -> protected mode -> long mode. The AP uses a temporary GDT in
 * this blob, then loads the BSP's GDT/IDT before calling C. Fixed physical
 * offsets let the BSP patch CR3, stack table, entry point and descriptors.
 */
.section .text
.code16
.globl ap_trampoline_start
ap_trampoline_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x9F00, %sp
    lgdt ap_temp_gdtr
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmp $0x08, $ap_pm

.code32
ap_pm:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4
    mov 0x8F00, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    or $0x80000001, %eax
    mov %eax, %cr0
    ljmp $0x18, $ap_lm

.code64
ap_lm:
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    lgdt ap_saved_gdtr(%rip)
    lidt ap_saved_idtr(%rip)
    mov 0x8F18, %rax
    pushq %rax
    leaq ap_cs_reload(%rip), %rax
    pushq %rax
    lretq
ap_cs_reload:
    mov 0x8F40, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov 0x8FA0, %ecx
    mov $1, %edx
    lock xaddl %edx, 0x8FA0
    cmpl $8, %edx
    jae ap_halt
    mov 0x8F60(,%rdx,8), %rsp
    mov %edx, %esi
    mov 0x8F48, %rdi
    mov 0x8F10, %rax
    call *%rax
ap_halt:
    cli
    hlt
    jmp ap_halt

/* Temporary GDT used only until the BSP's GDT is loaded. */
.org 0xC00
.align 16
ap_temp_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF  /* 32-bit code */
    .quad 0x00CF92000000FFFF  /* 32-bit data */
    .quad 0x00AF9A000000FFFF  /* 64-bit code */
    .quad 0x00CF92000000FFFF  /* 64-bit data */
ap_temp_gdt_end:

.org 0xE00
ap_temp_gdtr:
    .word ap_temp_gdt_end - ap_temp_gdt - 1
    .long ap_temp_gdt

/* Shared boot block. Addresses are fixed relative to the 0x8000 copy. */
.org 0xF00
ap_boot_cr3:
    .quad 0
ap_boot_stack:
    .quad 0
ap_boot_entry:
    .quad 0
ap_boot_cs:
    .quad 0
ap_saved_gdtr:
    .skip 10
.org 0xF30
ap_saved_idtr:
    .skip 10
.org 0xF40
ap_boot_ds:
    .quad 0
.org 0xF48
ap_boot_ptr:
    .quad 0
.org 0xF50
ap_boot_lapic:
    .quad 0
.org 0xF60
ap_stack_table:
    .skip 64
.org 0xFA0
ap_cpu_counter:
    .long 0

.org 0x1000
