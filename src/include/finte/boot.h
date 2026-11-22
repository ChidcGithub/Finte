#ifndef FINTE_BOOT_H
#define FINTE_BOOT_H

#include <stdint.h>

/* Finte boot protocol v1.
 *
 * The structure is intentionally fixed-width and self-describing. Producers
 * fill it before entering the kernel; the kernel validates every pointer and
 * range before use. All pointers are physical addresses in the boot address
 * space. The producer retains ownership of the pointed-to data until the
 * kernel has copied or reserved it.
 */
#define FINTE_BOOT_MAGIC 0xF17E0001u
#define FINTE_MULTIBOOT_MAGIC 0x2BADB002u
#define FINTE_KERNEL_BASE 0x04000000ull
#define FINTE_BOOT_VERSION 1u

#define FINTE_BOOT_ARCH_X86_64 1u
#define FINTE_BOOT_ARCH_ARM64  2u

#define FINTE_BOOT_PLATFORM_BIOS 1u
#define FINTE_BOOT_PLATFORM_UEFI 2u

#define FINTE_BOOT_CONSOLE_NONE  0u
#define FINTE_BOOT_CONSOLE_VGA   1u
#define FINTE_BOOT_CONSOLE_UEFI  2u
#define FINTE_BOOT_CONSOLE_PL011 3u

#define FINTE_BOOT_FLAG_MEMMAP    (1u << 0)
#define FINTE_BOOT_FLAG_ACPI      (1u << 1)
#define FINTE_BOOT_FLAG_DTB       (1u << 2)
#define FINTE_BOOT_FLAG_FRAMEBUFFER (1u << 3)
#define FINTE_BOOT_FLAG_MODULES   (1u << 4)
#define FINTE_BOOT_FLAG_FW_SERVICES (1u << 5)

#define FINTE_BOOT_MMAP_USABLE       1u
#define FINTE_BOOT_MMAP_RESERVED     2u
#define FINTE_BOOT_MMAP_ACPI_RECLAIM 3u
#define FINTE_BOOT_MMAP_ACPI_NVS     4u
#define FINTE_BOOT_MMAP_BAD          5u
#define FINTE_BOOT_MMAP_KERNEL       6u
#define FINTE_BOOT_MMAP_MODULE       7u

#define FINTE_BOOT_MMAP_MAX 64u
#define FINTE_BOOT_MODULE_MAX 4u

#define FINTE_BOOT_OK              0
#define FINTE_BOOT_ERR_NULL       -1
#define FINTE_BOOT_ERR_MAGIC      -2
#define FINTE_BOOT_ERR_VERSION    -3
#define FINTE_BOOT_ERR_SIZE       -4
#define FINTE_BOOT_ERR_ARCH       -5
#define FINTE_BOOT_ERR_PLATFORM   -6
#define FINTE_BOOT_ERR_CONSOLE    -7
#define FINTE_BOOT_ERR_KERNEL     -8
#define FINTE_BOOT_ERR_RANGE      -9
#define FINTE_BOOT_ERR_MEMMAP     -10
#define FINTE_BOOT_ERR_MODULE     -11
#define FINTE_BOOT_ERR_FRAMEBUFFER -12
#define FINTE_BOOT_ERR_FIRMWARE   -13

typedef struct fboot_blk {
    uint64_t block_io;       /* EFI_BLOCK_IO_PROTOCOL *, only if fw_services */
    uint64_t last_block;
    uint32_t media_id;
    uint32_t block_size;
    uint32_t flags;
    uint32_t reserved;
    char name[8];
} fboot_blk_t;

#define FINTE_BOOT_BLK_MAX 8

typedef struct fboot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;           /* FINTE_BOOT_MMAP_* */
    uint32_t flags;          /* producer-specific attributes, 0 if unused */
} fboot_mmap_entry_t;

typedef struct fboot_module {
    uint64_t addr;
    uint64_t size;
    uint32_t type;           /* 1 = ramdisk, 2 = initrd, 3 = payload */
    uint32_t flags;
    char name[16];
} fboot_module_t;

#define FINTE_BOOT_MODULE_RAMDISK 1u
#define FINTE_BOOT_MODULE_INITRD  2u
#define FINTE_BOOT_MODULE_PAYLOAD 3u

/* Keep this layout in sync with src/boot/stage2.S offsets. */
typedef struct fboot_info {
    /* Original v0-compatible prefix. */
    uint32_t magic;
    uint32_t boot_drive;
    uint32_t kernel_size;
    uint32_t ramdisk_addr;
    uint32_t ramdisk_size;
    uint32_t version;
    uint32_t console_type;
    uint32_t reserved;

    uint64_t efi_system_table;  /* only if fw_services */
    uint64_t efi_con_in;        /* only if fw_services */
    uint64_t efi_con_out;       /* only if fw_services */
    uint64_t reserved2;

    uint64_t fb_addr;
    uint32_t fb_pitch;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
    uint32_t fb_format;
    uint32_t fb_pad;

    uint64_t fw_idt_base;       /* only if fw_services */
    uint32_t fw_idt_limit;
    uint32_t fw_idt_pad;

    uint32_t blk_count;         /* only if fw_services */
    uint32_t blk_reserved;
    uint32_t input_usb;
    uint32_t input_pad;
    fboot_blk_t blk[FINTE_BOOT_BLK_MAX];

    /* v1 additions. */
    uint32_t header_size;       /* bytes of this structure understood */
    uint32_t total_size;        /* bytes including optional trailing data */
    uint32_t arch;              /* FINTE_BOOT_ARCH_* */
    uint32_t platform;          /* FINTE_BOOT_PLATFORM_* */
    uint32_t flags;             /* FINTE_BOOT_FLAG_* */
    uint32_t memmap_count;
    uint32_t memmap_desc_size;
    uint32_t module_count;
    uint32_t module_desc_size;
    uint32_t fw_services;       /* 1 = firmware services remain callable */
    uint32_t reserved3;
    uint64_t kernel_base;
    uint64_t memmap_addr;       /* fboot_mmap_entry_t[] */
    uint64_t modules_addr;      /* fboot_module_t[] */
    uint64_t acpi_addr;
    uint64_t dtb_addr;
    uint64_t cmdline_addr;
    uint64_t cmdline_size;
    uint64_t entropy_addr;
    uint64_t entropy_size;
} fboot_info_t;

#define FINTE_BOOT_INFO_BYTES 568u

int finte_boot_validate(const fboot_info_t *bi, uint32_t expected_arch);
int finte_boot_mmap_usable(const fboot_info_t *bi, uint64_t addr,
                           uint64_t size);

#endif /* FINTE_BOOT_H */
