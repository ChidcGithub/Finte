/* Finte stage 2: CHS disk loader + protected/long mode transition.
 *
 * Loaded at 0x8000 by the MBR. Reads the image header at LBA 33, loads the
 * flat kernel to 0x10000 and the ramdisk to 0x60000 via BIOS CHS reads, copies
 * the kernel to 2 MiB, identity maps 1 GiB, and enters long mode.
 */
.code16
.intel_syntax noprefix

.equ HEADER_ADDR, 0x6000
.equ BOOTINFO_ADDR, 0x5000
.equ MODULE_ADDR, 0x5700
.equ MEMMAP_ADDR, 0x5800
.equ BOOTINFO_BYTES, 568
.equ BOOT_HEADER_SIZE, 448
.equ BOOT_TOTAL_SIZE, 452
.equ BOOT_ARCH, 456
.equ BOOT_PLATFORM, 460
.equ BOOT_FLAGS, 464
.equ BOOT_MMAP_COUNT, 468
.equ BOOT_MMAP_DESC, 472
.equ BOOT_MODULE_COUNT, 476
.equ BOOT_MODULE_DESC, 480
.equ BOOT_MODULES_ADDR, 512
.equ BOOT_MMAP_ADDR, 504
.equ BOOT_KERNEL_BASE, 496
.equ BOOT_FLAG_MEMMAP, 1
.equ BOOT_FLAG_MODULES, 16
.equ KERNEL_LOAD, 0x10000
.equ RAMDISK_LOAD, 0x60000
.equ KERNEL_DEST, 0x4000000

.section .text
.globl _start
_start:
    cli
    cld
    mov al, '2'
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    mov [boot_drive], dl

    call serial_init
    mov al, 'S'
    mov si, msg_stage2
    call serial_puts
    mov al, '2'
    call serial_putc
    call bios_puts

    /* Fast A20. */
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    mov al, 'A'
    call serial_putc

    /* Read CHS geometry. */
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    jc geometry_error
    mov al, cl
    and al, 0x3F
    jz geometry_error
    mov [spt], al
    mov al, dh
    inc al
    jz geometry_error
    mov [heads], al
    mov al, 'G'
    call serial_putc

    /* Read the image header (one sector) to 0x6000. */
    mov dword ptr [load_lba], 33
    mov word ptr [load_remaining], 1
    mov word ptr [load_seg], 0x0600
    call load_region

    mov al, 'H'
    call serial_putc
    cmp dword ptr [HEADER_ADDR + 0], 0x46494E48
    jne header_error
    mov eax, [HEADER_ADDR + 0x18]       /* kernel_size */
    test eax, eax
    jz header_error
    cmp eax, 0x40000
    ja header_error
    mov eax, [HEADER_ADDR + 0x1C]       /* ramdisk_size */
    cmp eax, 0x40000
    ja header_error

    /* Kernel to 0x10000. */
    mov eax, [HEADER_ADDR + 0x08]
    mov [load_lba], eax
    mov ax, [HEADER_ADDR + 0x0C]
    mov [load_remaining], ax
    mov word ptr [load_seg], 0x1000
    call load_region
    mov al, 'K'
    call serial_putc

    /* Ramdisk to 0x60000, if present. */
    mov ax, [HEADER_ADDR + 0x14]
    test ax, ax
    jz no_ramdisk
    mov eax, [HEADER_ADDR + 0x10]
    mov [load_lba], eax
    mov ax, [HEADER_ADDR + 0x14]
    mov [load_remaining], ax
    mov word ptr [load_seg], (RAMDISK_LOAD >> 4)
    call load_region

no_ramdisk:
    mov al, 'R'
    call serial_putc

    /* Boot information consumed by the kernel at 0x5000. */
    xor ax, ax
    mov es, ax
    mov edi, BOOTINFO_ADDR
    xor eax, eax
    mov ecx, 512
    rep stosd

    mov dword ptr [BOOTINFO_ADDR + 0x00], 0xF17E0001
    movzx eax, byte ptr [boot_drive]
    mov [BOOTINFO_ADDR + 0x04], eax
    mov eax, [HEADER_ADDR + 0x18]
    mov [BOOTINFO_ADDR + 0x08], eax
    mov dword ptr [BOOTINFO_ADDR + 0x0C], RAMDISK_LOAD
    mov eax, [HEADER_ADDR + 0x1C]
    mov [BOOTINFO_ADDR + 0x10], eax
    mov dword ptr [BOOTINFO_ADDR + 0x14], 1
    mov dword ptr [BOOTINFO_ADDR + 0x18], 1  /* VGA text console */

    /* v1 protocol metadata. */
    mov dword ptr [BOOTINFO_ADDR + BOOT_HEADER_SIZE], BOOTINFO_BYTES
    mov dword ptr [BOOTINFO_ADDR + BOOT_TOTAL_SIZE], BOOTINFO_BYTES
    mov dword ptr [BOOTINFO_ADDR + BOOT_ARCH], 1       /* x86_64 */
    mov dword ptr [BOOTINFO_ADDR + BOOT_PLATFORM], 1   /* BIOS */
    mov dword ptr [BOOTINFO_ADDR + BOOT_MMAP_DESC], 24
    mov dword ptr [BOOTINFO_ADDR + BOOT_KERNEL_BASE], KERNEL_DEST
    mov dword ptr [BOOTINFO_ADDR + BOOT_KERNEL_BASE + 4], 0

    /* One ramdisk module descriptor at 0x5700. */
    mov dword ptr [BOOTINFO_ADDR + BOOT_MODULE_COUNT], 1
    mov dword ptr [BOOTINFO_ADDR + BOOT_MODULE_DESC], 40
    mov dword ptr [BOOTINFO_ADDR + BOOT_MODULES_ADDR], MODULE_ADDR
    mov dword ptr [BOOTINFO_ADDR + BOOT_MODULES_ADDR + 4], 0
    mov dword ptr [MODULE_ADDR + 0], RAMDISK_LOAD
    mov dword ptr [MODULE_ADDR + 4], 0
    mov eax, [BOOTINFO_ADDR + 0x10]
    mov [MODULE_ADDR + 8], eax
    mov dword ptr [MODULE_ADDR + 12], 0
    mov dword ptr [MODULE_ADDR + 16], 1
    mov dword ptr [MODULE_ADDR + 20], 0
    mov dword ptr [MODULE_ADDR + 24], 0x646D6172  /* "ramd" */
    mov dword ptr [MODULE_ADDR + 28], 0x006B7369  /* "isk\0" */

    call e820_probe
    or dword ptr [BOOTINFO_ADDR + BOOT_FLAGS], BOOT_FLAG_MODULES

    mov ax, 0x0003                      /* BIOS 80x25 text mode */
    int 0x10
    mov si, msg_booting
    call serial_puts
    call bios_puts
    mov al, 'P'
    call serial_putc

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    .att_syntax
    ljmp $0x18, $pm_entry
    .intel_syntax noprefix

/* Probe the BIOS E820 map into MEMMAP_ADDR. The 24-byte entry layout is
 * base, length, type, attributes; it matches fboot_mmap_entry_t. */
e820_probe:
    xor ax, ax
    mov es, ax
    mov di, MEMMAP_ADDR
    xor eax, eax
    mov ecx, 384
    rep stosd
    mov dword ptr [BOOTINFO_ADDR + BOOT_MMAP_COUNT], eax
    mov dword ptr [BOOTINFO_ADDR + BOOT_FLAGS], BOOT_FLAG_MEMMAP
    mov dword ptr [BOOTINFO_ADDR + BOOT_MMAP_ADDR], MEMMAP_ADDR
    mov dword ptr [BOOTINFO_ADDR + BOOT_MMAP_ADDR + 4], 0
    mov di, MEMMAP_ADDR
    xor ebx, ebx
1:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    jc 3f
    cmp eax, 0x534D4150
    jne 4f
    /* Skip zero-length and type-0 entries. */
    cmp dword ptr [di + 8], 0
    jne 2f
    cmp dword ptr [di + 12], 0
    je 5f
2:
    cmp dword ptr [di + 16], 0
    je 5f
    inc dword ptr [BOOTINFO_ADDR + BOOT_MMAP_COUNT]
    add di, 24
    cmp dword ptr [BOOTINFO_ADDR + BOOT_MMAP_COUNT], 64
    jae 3f
5:
    test ebx, ebx
    jnz 1b
3:
    ret
4:
    mov dword ptr [BOOTINFO_ADDR + BOOT_MMAP_COUNT], 0
    mov dword ptr [BOOTINFO_ADDR + BOOT_FLAGS], 0
    ret

/* load_region: consume load_remaining sectors from load_lba into load_seg.
 * One sector per BIOS call, converted to CHS with the BIOS geometry. */
load_region:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    mov ax, [load_remaining]
    test ax, ax
    jz 9f

    mov eax, [load_lba]
    xor edx, edx
    movzx ebx, byte ptr [spt]
    div ebx
    inc dl
    mov [cur_sector], dl
    xor edx, edx
    movzx ebx, byte ptr [heads]
    div ebx
    mov [cur_head], dl
    mov [cur_cyl], ax

    xor ax, ax
    mov ds, ax
    mov ax, [load_seg]
    mov es, ax
    mov ax, [cur_cyl]
    mov ch, al
    mov al, ah
    and al, 0x03
    shl al, 6
    mov cl, [cur_sector]
    or cl, al
    mov dh, [cur_head]
    mov dl, [boot_drive]
    mov ah, 0x02
    mov al, 1
    xor bx, bx
    int 0x13
    jc disk_error

    inc dword ptr [load_lba]
    add word ptr [load_seg], 0x20
    dec word ptr [load_remaining]
    jmp load_region
9:
    ret

disk_error:
    mov si, msg_disk_error
    jmp fail
geometry_error:
    mov si, msg_geometry_error
    jmp fail
header_error:
    mov si, msg_header_error
fail:
    mov al, 'E'
    push si
    call serial_puts
    pop si
    call bios_puts
1:
    cli
    hlt
    jmp 1b

/* BIOS teletype output. */
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

/* COM1 output, for headless QEMU and serial hardware. */
serial_init:
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0
    mov al, 0x01
    out dx, al
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x3F8 + 2
    mov al, 0xC7
    out dx, al
    mov dx, 0x3F8 + 4
    mov al, 0x0B
    out dx, al
    ret

serial_putc:
    push ax
    mov dx, 0x3F8 + 5
1:
    in al, dx
    test al, 0x20
    jz 1b
    pop ax
    mov dx, 0x3F8
    out dx, al
    ret

serial_puts:
    lodsb
    test al, al
    jz 2f
    call serial_putc
    jmp serial_puts
2:
    ret

/* 32-bit setup, kernel relocation, paging, long mode. */
.macro serput ch
    mov dx, 0x3F8 + 5
1:
    in al, dx
    test al, 0x20
    jz 1b
    mov dx, 0x3F8
    mov al, \ch
    out dx, al
.endm

.code32
pm_entry:
    serput 'M'
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7000

    mov esi, KERNEL_LOAD
    mov edi, KERNEL_DEST
    mov ecx, [BOOTINFO_ADDR + 0x08]
    cld
    rep movsb
    serput 'C'

    /* Clear PML4, PDPT and PD at 0x90000..0x92FFF. The ramdisk occupies
     * 0x60000..0x85xxx, so keep the tables above it and below the EBDA. */
    mov edi, 0x90000
    xor eax, eax
    mov ecx, 3072
    rep stosd

    mov dword ptr [0x90000], 0x91003
    mov dword ptr [0x91000], 0x92003
    xor ecx, ecx
    mov ebx, 0x92000
1:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [ebx + ecx * 8], eax
    mov dword ptr [ebx + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne 1b

    mov eax, cr4
    or eax, 0x20
    mov cr4, eax
    mov eax, 0x90000
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x100
    wrmsr

    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax

    serput 'L'
    mov edi, BOOTINFO_ADDR
    mov esi, 0xF17E0001
    .att_syntax
    ljmp $0x08, $0x4000000
    .intel_syntax noprefix

.section .data
.align 8
boot_drive:
    .byte 0
spt:
    .byte 0
heads:
    .byte 0
cur_sector:
    .byte 0
cur_head:
    .byte 0

.align 8
load_lba:
    .long 0
load_remaining:
    .word 0
load_seg:
    .word 0
cur_cyl:
    .word 0

.align 16
gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF    /* 0x08: 64-bit code */
    .quad 0x00CF92000000FFFF    /* 0x10: data */
    .quad 0x00CF9A000000FFFF    /* 0x18: 32-bit code */
gdt_end:
gdt_desc:
    .word gdt_end - gdt - 1
    .long gdt

msg_stage2:
    .asciz "Finte bootloader: stage2 CHS loader\r\n"
msg_booting:
    .asciz "Finte bootloader: entering long mode...\r\n"
msg_disk_error:
    .asciz "Finte bootloader: CHS read error\r\n"
msg_geometry_error:
    .asciz "Finte bootloader: CHS geometry error\r\n"
msg_header_error:
    .asciz "Finte bootloader: bad image header\r\n"
