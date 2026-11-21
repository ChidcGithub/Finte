#ifndef FINTE_BOOT_H
#define FINTE_BOOT_H

#include <stdint.h>

#define FINTE_BOOT_MAGIC 0xF17E0001u
#define FINTE_MULTIBOOT_MAGIC 0x2BADB002u
#define FINTE_KERNEL_BASE 0x04000000ull

typedef struct fboot_blk {
    uint64_t block_io;       /* EFI_BLOCK_IO_PROTOCOL * */
    uint64_t last_block;
    uint32_t media_id;
    uint32_t block_size;
    uint32_t flags;
    uint32_t reserved;
    char name[8];
} fboot_blk_t;

#define FINTE_BOOT_BLK_MAX 8

/* Filled by our stage2 loader at physical 0x5000 and passed in RDI.
 * Identity mapping makes the physical address directly readable. */
typedef struct fboot_info {
    uint32_t magic;
    uint32_t boot_drive;
    uint32_t kernel_size;
    uint32_t ramdisk_addr;
    uint32_t ramdisk_size;
    uint32_t version;
    uint32_t console_type;      /* 1 = VGA text, 2 = UEFI ConOut */
    uint32_t reserved;
    uint64_t efi_system_table;  /* UEFI only */
    uint64_t efi_con_in;
    uint64_t efi_con_out;
    uint64_t reserved2;
    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
    uint32_t fb_format;
    uint32_t fb_pad;
    uint64_t fw_idt_base;
    uint32_t fw_idt_limit;
    uint32_t fw_idt_pad;
    uint32_t blk_count;
    uint32_t blk_reserved;
    uint32_t input_usb;
    uint32_t input_pad;
    fboot_blk_t blk[FINTE_BOOT_BLK_MAX];
} fboot_info_t;

#endif /* FINTE_BOOT_H */
