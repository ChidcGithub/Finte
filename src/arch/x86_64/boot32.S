/* Finte stage-1: Multiboot -> 32-bit protected mode -> long mode.
 *
 * QEMU's Multiboot loader only accepts ELF32, so this outer image is 32-bit.
 * It contains the 64-bit kernel as a flat binary at 2 MiB, identity maps the
 * first 1 GiB with 2 MiB pages, enables long mode, and jumps to 0x200000.
 */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000003
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.set KERNEL64_BASE, 0x4000000

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .bss, "aw", @nobits
.align 4096
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 4096

.align 16
boot_stack_bottom:
    .skip 16384
boot_stack_top:

.section .text.entry, "ax"
.code32
.globl _start
.type _start, @function
_start:
    cli

    /* Preserve the Multiboot hand-off for the 64-bit kernel. */
    mov %eax, %esi              /* magic */
    mov %ebx, %edi              /* multiboot info physical address */
    mov $boot_stack_top, %esp
    mov $0, %ebp

    /* PML4[0] -> PDPT */
    mov $pdpt, %eax
    or  $0x3, %eax
    mov %eax, pml4
    /* PDPT[0] -> PD */
    mov $pd, %eax
    or  $0x3, %eax
    mov %eax, pdpt

    /* PD: 512 * 2 MiB identity mappings. */
    xor %ecx, %ecx
    mov $pd, %ebx
1:
    mov %ecx, %eax
    shl $21, %eax
    or  $0x83, %eax             /* present | writable | 2 MiB page */
    mov %eax, (%ebx, %ecx, 8)   /* long-mode entries are 8 bytes */
    inc %ecx
    cmp $512, %ecx
    jne 1b

    lgdt gdt64_ptr

    /* PAE + long mode + paging. */
    mov %cr4, %eax
    or  $0x20, %eax
    mov %eax, %cr4

    mov $pml4, %eax
    mov %eax, %cr3

    mov $0xC0000080, %ecx
    rdmsr
    or  $0x100, %eax            /* EFER.LME */
    wrmsr

    mov %cr0, %eax
    or  $0x80000001, %eax
    mov %eax, %cr0

    /* The far jump both enters 64-bit mode and transfers to the real kernel.
     * Its entry code establishes the stack and data segments.
     */
    ljmp $0x08, $KERNEL64_BASE
.size _start, . - _start

.section .rodata
.align 16
gdt64:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF    /* 64-bit code */
    .quad 0x00CF92000000FFFF    /* data */
gdt64_end:

.align 8
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .kernel64, "a"
.incbin "build/kernel64.bin"
