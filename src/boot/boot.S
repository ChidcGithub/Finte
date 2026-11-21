/* Finte stage 1: 512-byte MBR boot sector.
 *
 * Loads 32 sectors (16 KiB) of stage 2 to 0x0000:0x8000 using CHS reads, then
 * jumps there with DL preserved. CHS is used instead of EDD LBA because it is
 * the most compatible path across BIOS USB emulation, old firmware, and QEMU.
 */
.code16
.intel_syntax noprefix

.set STAGE2_SECTORS, 32
.set STAGE2_LOAD, 0x8000

.section .text
.globl _start
_start:
    cli
    mov al, 'B'
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl
    mov al, '1'
    mov al, dl

    mov si, msg_loading
    call bios_puts

    /* Disk reset + geometry. */
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov dl, [boot_drive]
    xor ah, ah
    int 0x13
    jc disk_error

    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    mov al, cl
    and al, 0x3F
    jz disk_error
    mov [spt], al
    mov al, dh
    inc al
    jz disk_error
    mov [heads], al
    mov al, 'g'

    /* Read stage2 one sector at a time: LBA 1..32 = C0,H0,S2... */
    mov byte ptr [cyl], 0
    mov byte ptr [head], 0
    mov byte ptr [sector], 2
    mov word ptr [buf_off], STAGE2_LOAD
    mov word ptr [left], STAGE2_SECTORS

read_loop:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ah, 0x02
    mov al, 1
    mov ch, [cyl]
    mov cl, [sector]
    mov dh, [head]
    mov dl, [boot_drive]
    mov bx, [buf_off]
    int 0x13
    jc disk_error

    add word ptr [buf_off], 512
    dec word ptr [left]
    jz read_done

    inc byte ptr [sector]
    mov al, [sector]
    cmp al, [spt]
    jbe read_loop
    mov byte ptr [sector], 1
    inc byte ptr [head]
    mov al, [head]
    cmp al, [heads]
    jb read_loop
    mov byte ptr [head], 0
    inc byte ptr [cyl]
    jmp read_loop

read_done:
    mov al, 'R'
    mov si, msg_stage2
    call bios_puts
    mov al, 'J'
    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_LOAD

disk_error:
    mov al, 'E'
    mov al, ah
    mov si, msg_error
    call bios_puts
1:
    cli
    hlt
    jmp 1b

bios_puts:
    push ax
    xor ax, ax
    mov ds, ax
    pop ax
    lodsb
    test al, al
    jz 2f
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp bios_puts
2:
    ret

.section .data
.align 2
spt:
    .byte 0
heads:
    .byte 0
cyl:
    .byte 0
head:
    .byte 0
sector:
    .byte 0
boot_drive:
    .byte 0
buf_off:
    .word 0
left:
    .word 0

msg_loading:
    .asciz "Finte boot\r\n"
msg_stage2:
    .asciz "Finte loader\r\n"
msg_error:
    .asciz "Finte disk error\r\n"
