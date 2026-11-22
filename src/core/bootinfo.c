#include "finte/boot.h"

_Static_assert(sizeof(fboot_info_t) == FINTE_BOOT_INFO_BYTES,
               "boot info layout changed; update FINTE_BOOT_INFO_BYTES and stage2");

static int range_ok(uint64_t addr, uint64_t size)
{
    return size == 0 || (addr != 0 && addr + size >= addr);
}

int finte_boot_validate(const fboot_info_t *bi, uint32_t expected_arch)
{
    if (!bi) {
        return FINTE_BOOT_ERR_NULL;
    }
    if (bi->magic != FINTE_BOOT_MAGIC) {
        return FINTE_BOOT_ERR_MAGIC;
    }
    if (bi->version != FINTE_BOOT_VERSION) {
        return FINTE_BOOT_ERR_VERSION;
    }
    if (bi->header_size < FINTE_BOOT_INFO_BYTES ||
        bi->total_size < bi->header_size) {
        return FINTE_BOOT_ERR_SIZE;
    }
    if (expected_arch && bi->arch != expected_arch) {
        return FINTE_BOOT_ERR_ARCH;
    }
    if (bi->platform != FINTE_BOOT_PLATFORM_BIOS &&
        bi->platform != FINTE_BOOT_PLATFORM_UEFI) {
        return FINTE_BOOT_ERR_PLATFORM;
    }
    if (bi->console_type > FINTE_BOOT_CONSOLE_PL011 &&
        bi->console_type != FINTE_BOOT_CONSOLE_NONE) {
        return FINTE_BOOT_ERR_CONSOLE;
    }
    if (!bi->kernel_base || bi->kernel_size == 0) {
        return FINTE_BOOT_ERR_KERNEL;
    }
    if (!range_ok(bi->ramdisk_addr, bi->ramdisk_size)) {
        return FINTE_BOOT_ERR_RANGE;
    }
    if (!range_ok(bi->cmdline_addr, bi->cmdline_size) ||
        !range_ok(bi->entropy_addr, bi->entropy_size)) {
        return FINTE_BOOT_ERR_RANGE;
    }
    if (bi->flags & FINTE_BOOT_FLAG_MEMMAP) {
        if (!bi->memmap_addr || bi->memmap_count == 0 ||
            bi->memmap_count > FINTE_BOOT_MMAP_MAX ||
            bi->memmap_desc_size != sizeof(fboot_mmap_entry_t)) {
            return FINTE_BOOT_ERR_MEMMAP;
        }
        const fboot_mmap_entry_t *map =
            (const fboot_mmap_entry_t *)(uintptr_t)bi->memmap_addr;
        for (uint32_t i = 0; i < bi->memmap_count; i++) {
            if (map[i].length == 0 || map[i].base + map[i].length < map[i].base ||
                map[i].type < FINTE_BOOT_MMAP_USABLE ||
                map[i].type > FINTE_BOOT_MMAP_MODULE) {
                return FINTE_BOOT_ERR_MEMMAP;
            }
        }
    }
    if (bi->flags & FINTE_BOOT_FLAG_MODULES) {
        if (!bi->modules_addr || bi->module_count == 0 ||
            bi->module_count > FINTE_BOOT_MODULE_MAX ||
            bi->module_desc_size != sizeof(fboot_module_t)) {
            return FINTE_BOOT_ERR_MODULE;
        }
        const fboot_module_t *mods =
            (const fboot_module_t *)(uintptr_t)bi->modules_addr;
        for (uint32_t i = 0; i < bi->module_count; i++) {
            if (!range_ok(mods[i].addr, mods[i].size)) {
                return FINTE_BOOT_ERR_MODULE;
            }
        }
    }
    if (bi->flags & FINTE_BOOT_FLAG_FRAMEBUFFER) {
        if (!bi->fb_addr || bi->fb_width == 0 || bi->fb_height == 0 ||
            bi->fb_bpp < 8 || bi->fb_pitch <
                (uint64_t)bi->fb_width * (bi->fb_bpp / 8u)) {
            return FINTE_BOOT_ERR_FRAMEBUFFER;
        }
    }
    if (bi->flags & FINTE_BOOT_FLAG_FW_SERVICES) {
        if (!bi->fw_services || !bi->efi_system_table ||
            !bi->efi_con_in || !bi->efi_con_out) {
            return FINTE_BOOT_ERR_FIRMWARE;
        }
    } else if (bi->fw_services) {
        return FINTE_BOOT_ERR_FIRMWARE;
    }
    return FINTE_BOOT_OK;
}

int finte_boot_mmap_usable(const fboot_info_t *bi, uint64_t addr, uint64_t size)
{
    if (!bi || (bi->flags & FINTE_BOOT_FLAG_MEMMAP) == 0) {
        return 1;
    }
    if (size == 0) {
        return 1;
    }
    if (addr + size < addr) {
        return 0;
    }
    const fboot_mmap_entry_t *map =
        (const fboot_mmap_entry_t *)(uintptr_t)bi->memmap_addr;
    for (uint32_t i = 0; i < bi->memmap_count; i++) {
        if (map[i].type != FINTE_BOOT_MMAP_USABLE) {
            continue;
        }
        if (addr >= map[i].base && addr + size <= map[i].base + map[i].length) {
            return 1;
        }
    }
    return 0;
}
