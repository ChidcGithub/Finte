/* Microsoft x64 ABI -> Finte kernel entry registers.
 * rcx = boot_info, rdx = magic, r8 = entry.
 * The kernel entry expects rdi = boot_info, rsi = magic.
 */
.text
.globl uefi_jump_to_kernel
uefi_jump_to_kernel:
    mov %rcx, %rdi
    mov %rdx, %rsi
    jmp *%r8

.globl uefi_get_idt
uefi_get_idt:
    sidt (%rcx)
    ret
