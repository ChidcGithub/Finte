#include "finte/uefi.h"
#include "finte/boot.h"

#define KERNEL_BASE FINTE_KERNEL_BASE
#define KERNEL_RESERVE 0x2400000ull /* code + 32 MiB heap + stack */
#define RAMDISK_MAX 0x40000ull
#define LOW_LIMIT 0x3F000000ull

static const EFI_GUID loaded_image_guid = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID simple_fs_guid = {
    0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID gop_guid = {
    0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}
};
static const EFI_GUID block_io_guid = {
    0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID simple_input_guid = {
    0x387477C1, 0x69C7, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID device_path_guid = {
    0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID acpi20_guid = {
    0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}
};
static const EFI_GUID acpi10_guid = {
    0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}
};

static uint8_t uefi_memory_map[64 * 1024] __attribute__((aligned(8)));
static fboot_mmap_entry_t boot_mmap[FINTE_BOOT_MMAP_MAX];
static fboot_module_t boot_modules[FINTE_BOOT_MODULE_MAX];
static UINTN g_map_key;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    return a->Data1 == b->Data1 && a->Data2 == b->Data2 &&
           a->Data3 == b->Data3 &&
           a->Data4[0] == b->Data4[0] && a->Data4[1] == b->Data4[1] &&
           a->Data4[2] == b->Data4[2] && a->Data4[3] == b->Data4[3] &&
           a->Data4[4] == b->Data4[4] && a->Data4[5] == b->Data4[5] &&
           a->Data4[6] == b->Data4[6] && a->Data4[7] == b->Data4[7];
}

static void *find_config_table(EFI_SYSTEM_TABLE *st, const EFI_GUID *guid)
{
    if (!st || !st->ConfigurationTable) {
        return NULL;
    }
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&st->ConfigurationTable[i].VendorGuid, guid)) {
            return st->ConfigurationTable[i].VendorTable;
        }
    }
    return NULL;
}

static uint32_t map_uefi_memory_type(uint32_t type)
{
    switch (type) {
    case 7:  /* conventional */
    case 1:  /* loader code */
    case 2:  /* loader data */
    case 3:  /* boot services code */
    case 4:  /* boot services data */
        return FINTE_BOOT_MMAP_USABLE;
    case 9:  /* ACPI reclaim */
        return FINTE_BOOT_MMAP_ACPI_RECLAIM;
    case 10: /* ACPI NVS */
        return FINTE_BOOT_MMAP_ACPI_NVS;
    default:
        return FINTE_BOOT_MMAP_RESERVED;
    }
}

extern void uefi_jump_to_kernel(uint64_t boot_info, uint64_t magic,
                                uint64_t entry);
extern void uefi_get_idt(void *out);

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void)
{
    outb(0x3F8 + 1, 0);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 1);
    outb(0x3F8 + 1, 0);
    outb(0x3F8 + 3, 3);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void serial_putc(char c)
{
    while ((inb(0x3F8 + 5) & 0x20) == 0) {
    }
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        serial_putc(*s++);
    }
}

static void serial_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[17];
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[v & 0xF];
        v >>= 4;
    }
    buf[16] = 0;
    serial_puts("0x");
    serial_puts(buf);
}

static void conout(EFI_SYSTEM_TABLE *st, const char *s)
{
    CHAR16 buf[128];
    int n = 0;
    while (*s && n < 126) {
        if (*s == '\n') {
            buf[n++] = '\r';
        }
        buf[n++] = (CHAR16)(uint8_t)*s++;
    }
    buf[n] = 0;
    if (st && st->ConOut && st->ConOut->OutputString) {
        st->ConOut->OutputString(st->ConOut, buf);
    }
}

static void fail(EFI_SYSTEM_TABLE *st, const char *msg)
{
    serial_puts("UEFI FAIL: ");
    serial_puts(msg);
    serial_puts("\r\n");
    conout(st, "Finte UEFI loader failed: ");
    conout(st, msg);
    conout(st, "\r\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static EFI_STATUS open_file(EFI_SYSTEM_TABLE *st, EFI_HANDLE image,
                            EFI_FILE_PROTOCOL **out)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(image,
        (EFI_GUID *)&loaded_image_guid, (void **)&li);
    if (!efi_ok(s) || !li) {
        return 1;
    }
    s = st->BootServices->HandleProtocol(li->DeviceHandle,
        (EFI_GUID *)&simple_fs_guid, (void **)&fs);
    if (!efi_ok(s) || !fs) {
        return 1;
    }
    s = fs->OpenVolume(fs, &root);
    if (!efi_ok(s) || !root) {
        return 1;
    }
    *out = root;
    return 0;
}

static int handle_is_usb_keyboard(EFI_SYSTEM_TABLE *st, EFI_HANDLE handle)
{
    void *dp = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        handle, (EFI_GUID *)&device_path_guid, &dp);
    if (!efi_ok(s) || !dp) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)dp;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t type = p[0];
        uint8_t subtype = p[1];
        uint16_t len = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
        if (len < 4) {
            break;
        }
        if (type == 3 && subtype == 5) { /* Messaging / USB */
            return 1;
        }
        if (type == 0x7F) {
            break;
        }
        p += len;
    }
    return 0;
}

static void fill_boot_info(fboot_info_t *bi, EFI_SYSTEM_TABLE *st,
                           uint64_t kernel_size, uint64_t ramdisk_addr,
                           uint64_t ramdisk_size,
                           EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    memset(bi, 0, sizeof(*bi));
    bi->magic = FINTE_BOOT_MAGIC;
    bi->boot_drive = 0;
    bi->kernel_size = (uint32_t)kernel_size;
    bi->ramdisk_addr = (uint32_t)ramdisk_addr;
    bi->ramdisk_size = (uint32_t)ramdisk_size;
    bi->version = FINTE_BOOT_VERSION;
    bi->console_type = FINTE_BOOT_CONSOLE_UEFI;
    bi->reserved = 0;
    bi->header_size = FINTE_BOOT_INFO_BYTES;
    bi->total_size = FINTE_BOOT_INFO_BYTES;
    bi->arch = FINTE_BOOT_ARCH_X86_64;
    bi->platform = FINTE_BOOT_PLATFORM_UEFI;
    bi->flags = FINTE_BOOT_FLAG_FW_SERVICES;
    bi->fw_services = 1;
    bi->kernel_base = KERNEL_BASE;
    bi->memmap_count = 0;
    bi->memmap_desc_size = sizeof(fboot_mmap_entry_t);
    bi->module_count = 0;
    bi->module_desc_size = sizeof(fboot_module_t);
    bi->efi_system_table = (uint64_t)(uintptr_t)st;
    bi->efi_con_in = (uint64_t)(uintptr_t)st->ConIn;
    bi->efi_con_out = (uint64_t)(uintptr_t)st->ConOut;
    bi->reserved2 = 0;
    bi->blk_count = 0;
    bi->input_usb = 0;

    if (ramdisk_addr && ramdisk_size) {
        memset(&boot_modules[0], 0, sizeof(boot_modules[0]));
        boot_modules[0].addr = ramdisk_addr;
        boot_modules[0].size = ramdisk_size;
        boot_modules[0].type = FINTE_BOOT_MODULE_RAMDISK;
        boot_modules[0].name[0] = 'r';
        boot_modules[0].name[1] = 'a';
        boot_modules[0].name[2] = 'm';
        boot_modules[0].name[3] = 'd';
        boot_modules[0].name[4] = 'i';
        boot_modules[0].name[5] = 's';
        boot_modules[0].name[6] = 'k';
        bi->modules_addr = (uint64_t)(uintptr_t)boot_modules;
        bi->module_count = 1;
        bi->flags |= FINTE_BOOT_FLAG_MODULES;
    }

    EFI_HANDLE *in_handles = NULL;
    UINTN in_count = 0;
    EFI_STATUS ins = st->BootServices->LocateHandleBuffer(
        2, (EFI_GUID *)&simple_input_guid, NULL, &in_count, &in_handles);
    if (efi_ok(ins) && in_handles) {
        for (UINTN i = 0; i < in_count; i++) {
            if (handle_is_usb_keyboard(st, in_handles[i])) {
                bi->input_usb = 1;
                break;
            }
        }
        if (st->BootServices->FreePool) {
            ((void (*)(void *))st->BootServices->FreePool)(in_handles);
        }
    }

    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } idtr;
    uefi_get_idt(&idtr);
    bi->fw_idt_limit = idtr.limit;
    bi->fw_idt_base = idtr.base;

    EFI_HANDLE *handles = NULL;
    UINTN count = 0;
    EFI_STATUS bs = st->BootServices->LocateHandleBuffer(
        2, (EFI_GUID *)&block_io_guid, NULL, &count, &handles);
    if (efi_ok(bs) && handles) {
        for (UINTN i = 0; i < count && bi->blk_count < FINTE_BOOT_BLK_MAX; i++) {
            EFI_BLOCK_IO_PROTOCOL *bio = NULL;
            EFI_STATUS hs = st->BootServices->HandleProtocol(
                handles[i], (EFI_GUID *)&block_io_guid, (void **)&bio);
            if (!efi_ok(hs) || !bio || !bio->Media || !bio->ReadBlocks) {
                continue;
            }
            EFI_BLOCK_IO_MEDIA *m = bio->Media;
            if (!m->MediaPresent || m->LogicalPartition ||
                m->BlockSize != 512 || m->LastBlock == 0) {
                continue;
            }
            fboot_blk_t *b = &bi->blk[bi->blk_count];
            b->block_io = (uint64_t)(uintptr_t)bio;
            b->last_block = m->LastBlock + 1;
            b->media_id = m->MediaId;
            b->block_size = m->BlockSize;
            b->flags = m->ReadOnly ? 1u : 0u;
            b->reserved = 0;
            b->name[0] = 'f';
            b->name[1] = 'w';
            b->name[2] = (char)('0' + bi->blk_count);
            b->name[3] = 0;
            bi->blk_count++;
        }
        if (st->BootServices->FreePool) {
            ((void (*)(void *))st->BootServices->FreePool)(handles);
        }
    }
    serial_puts("UEFI: block devices ");
    serial_hex(bi->blk_count);
    serial_puts("\r\n");

    UINTN map_size = sizeof(uefi_memory_map);
    UINTN map_key = 0;
    UINTN desc_size = 0;
    uint32_t desc_version = 0;
    EFI_STATUS ms = st->BootServices->GetMemoryMap(
        &map_size, (EFI_MEMORY_DESCRIPTOR *)uefi_memory_map, &map_key,
        &desc_size, &desc_version);
    g_map_key = map_key;
    serial_puts("UEFI: memory map ");
    serial_hex((uint64_t)ms);
    serial_puts(" size ");
    serial_hex(map_size);
    serial_puts("\r\n");
    if (efi_ok(ms) && desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR)) {
        uint32_t n = 0;
        for (UINTN off = 0;
             off + desc_size <= map_size && n < FINTE_BOOT_MMAP_MAX;
             off += desc_size) {
            EFI_MEMORY_DESCRIPTOR *d =
                (EFI_MEMORY_DESCRIPTOR *)(uefi_memory_map + off);
            if (d->NumberOfPages == 0) {
                continue;
            }
            boot_mmap[n].base = d->PhysicalStart;
            boot_mmap[n].length = d->NumberOfPages * 4096ull;
            boot_mmap[n].type = map_uefi_memory_type(d->Type);
            boot_mmap[n].flags = 0;
            n++;
        }
        bi->memmap_addr = (uint64_t)(uintptr_t)boot_mmap;
        bi->memmap_count = n;
        bi->memmap_desc_size = sizeof(fboot_mmap_entry_t);
        if (n) {
            bi->flags |= FINTE_BOOT_FLAG_MEMMAP;
        }
    }

    void *acpi = find_config_table(st, &acpi20_guid);
    if (!acpi) {
        acpi = find_config_table(st, &acpi10_guid);
    }
    if (acpi) {
        bi->acpi_addr = (uint64_t)(uintptr_t)acpi;
        bi->flags |= FINTE_BOOT_FLAG_ACPI;
    }

    if (gop && gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
        if ((info->PixelFormat == 0 || info->PixelFormat == 1) &&
            gop->Mode->FrameBufferBase && info->PixelsPerScanLine) {
            bi->fb_addr = gop->Mode->FrameBufferBase;
            bi->fb_pitch = info->PixelsPerScanLine * 4;
            bi->fb_width = info->HorizontalResolution;
            bi->fb_height = info->VerticalResolution;
            bi->fb_bpp = 32;
            bi->fb_format = info->PixelFormat;
            bi->flags |= FINTE_BOOT_FLAG_FRAMEBUFFER;
        }
    }
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    serial_init();
    serial_puts("Finte UEFI loader start\r\n");
    if (!st || !st->BootServices) {
        fail(st, "no boot services");
    }
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    conout(st, "\r\nFinte UEFI loader\r\n");
    serial_puts("UEFI: loading kernel\r\n");

    EFI_FILE_PROTOCOL *root = NULL;
    if (open_file(st, image, &root) != 0) {
        fail(st, "cannot open ESP");
    }

    EFI_PHYSICAL_ADDRESS kaddr = KERNEL_BASE;
    EFI_STATUS s = st->BootServices->AllocatePages(
        EFI_ALLOCATE_ADDRESS, EFI_LOADER_CODE,
        (UINTN)(KERNEL_RESERVE / 4096), &kaddr);
    serial_puts("UEFI: alloc kernel ");
    serial_hex((uint64_t)s);
    serial_puts(" addr ");
    serial_hex(kaddr);
    serial_puts("\r\n");
    if (!efi_ok(s) || kaddr != KERNEL_BASE) {
        fail(st, "kernel allocation at 2 MiB failed");
    }

    CHAR16 kpath[] = {'\\','E','F','I','\\','F','I','N','T','E','\\',
                      'K','E','R','N','E','L','.','B','I','N',0};
    EFI_FILE_PROTOCOL *kf = NULL;
    s = root->Open(root, &kf, kpath, EFI_FILE_MODE_READ, 0);
    if (!efi_ok(s) || !kf) {
        fail(st, "kernel.bin not found");
    }
    UINTN ksize = (UINTN)KERNEL_RESERVE;
    s = kf->Read(kf, &ksize, (void *)(uintptr_t)KERNEL_BASE);
    kf->Close(kf);
    serial_puts("UEFI: kernel read ");
    serial_hex((uint64_t)s);
    serial_puts(" size ");
    serial_hex(ksize);
    serial_puts("\r\n");
    if (!efi_ok(s) || ksize < 4096) {
        fail(st, "kernel read failed");
    }

    EFI_PHYSICAL_ADDRESS raddr = LOW_LIMIT;
    uint64_t rpages = RAMDISK_MAX / 4096;
    s = st->BootServices->AllocatePages(EFI_ALLOCATE_MAX_ADDRESS,
        EFI_LOADER_DATA, (UINTN)rpages, &raddr);
    if (!efi_ok(s)) {
        fail(st, "ramdisk allocation failed");
    }
    uint64_t rsize = 0;
    CHAR16 rpath[] = {'\\','E','F','I','\\','F','I','N','T','E','\\',
                      'R','A','M','D','I','S','K','.','B','I','N',0};
    EFI_FILE_PROTOCOL *rf = NULL;
    s = root->Open(root, &rf, rpath, EFI_FILE_MODE_READ, 0);
    if (efi_ok(s) && rf) {
        UINTN n = (UINTN)RAMDISK_MAX;
        s = rf->Read(rf, &n, (void *)(uintptr_t)raddr);
        rf->Close(rf);
        if (efi_ok(s)) {
            rsize = n;
        }
    }
    serial_puts("UEFI: ramdisk ");
    serial_hex(raddr);
    serial_puts(" size ");
    serial_hex(rsize);
    serial_puts("\r\n");

    EFI_PHYSICAL_ADDRESS biaddr = LOW_LIMIT - 0x100000ull;
    s = st->BootServices->AllocatePages(EFI_ALLOCATE_MAX_ADDRESS,
        EFI_LOADER_DATA, 1, &biaddr);
    if (!efi_ok(s)) {
        fail(st, "boot info allocation failed");
    }
    fboot_info_t *bi = (fboot_info_t *)(uintptr_t)biaddr;

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    s = st->BootServices->LocateProtocol((EFI_GUID *)&gop_guid, NULL,
                                          (void **)&gop);
    serial_puts("UEFI: gop ");
    serial_hex((uint64_t)s);
    serial_puts("\r\n");
    if (efi_ok(s) && gop && gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
        serial_puts("UEFI: gop mode ");
        serial_hex(info->HorizontalResolution);
        serial_puts("x");
        serial_hex(info->VerticalResolution);
        serial_puts(" fmt ");
        serial_hex(info->PixelFormat);
        serial_puts(" fb ");
        serial_hex(gop->Mode->FrameBufferBase);
        serial_puts("\r\n");
    }

    conout(st, "Booting Finte kernel...\r\n");
    for (int attempt = 0; attempt < 4; attempt++) {
        fill_boot_info(bi, st, ksize, raddr, rsize, gop);
        s = st->BootServices->ExitBootServices(image, g_map_key);
        if (efi_ok(s)) {
            break;
        }
    }
    if (!efi_ok(s)) {
        fail(st, "ExitBootServices failed");
    }

    /* Boot Services are gone; only serial I/O and the framebuffer remain. */
    bi->fw_services = 0;
    bi->flags &= ~FINTE_BOOT_FLAG_FW_SERVICES;
    bi->efi_system_table = 0;
    bi->efi_con_in = 0;
    bi->efi_con_out = 0;
    bi->fw_idt_base = 0;
    bi->fw_idt_limit = 0;
    bi->blk_count = 0;
    bi->input_usb = 0;

    serial_puts("UEFI: jumping to kernel\r\n");
    uefi_jump_to_kernel((uint64_t)(uintptr_t)bi, FINTE_BOOT_MAGIC,
                        KERNEL_BASE);
    fail(st, "kernel returned");
    return 0;
}
