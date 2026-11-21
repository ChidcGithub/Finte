/* Minimal AArch64 UEFI loader: load Finte's arm64 ELF kernel and jump.
 *
 * The kernel is linked for QEMU's virt machine at 0x40080000 and expects to
 * run with the firmware's identity map still active.  The loader therefore
 * allocates that exact physical range, copies the PT_LOAD segments, exits boot
 * services and branches to the ELF entry point.
 */
#include "finte/uefi.h"

#define KERNEL_BASE 0x40080000ull
#define KERNEL_FILE_MAX (2u * 1024u * 1024u)
#define MEMORY_MAP_MAX (64u * 1024u)
#define ELF_PT_LOAD 1u
#define ELF_MACHINE_AARCH64 183u

static const EFI_GUID loaded_image_guid = {
    0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};
static const EFI_GUID simple_fs_guid = {
    0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}
};

typedef struct {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_header_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_phdr_t;

static uint8_t kernel_file[KERNEL_FILE_MAX] __attribute__((aligned(4096)));
static uint8_t memory_map[MEMORY_MAP_MAX] __attribute__((aligned(8)));

static void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)value;
    }
    return dst;
}

static void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

static void conout(EFI_SYSTEM_TABLE *st, const char *s)
{
    CHAR16 buf[160];
    int n = 0;
    while (*s && n < 158) {
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
    conout(st, "Finte arm64 UEFI loader failed: ");
    conout(st, msg);
    conout(st, "\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static int open_kernel(EFI_SYSTEM_TABLE *st, EFI_HANDLE image,
                       EFI_FILE_PROTOCOL **out)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        image, (EFI_GUID *)&loaded_image_guid, (void **)&li);
    if (!efi_ok(s) || !li) {
        return 1;
    }
    s = st->BootServices->HandleProtocol(
        li->DeviceHandle, (EFI_GUID *)&simple_fs_guid, (void **)&fs);
    if (!efi_ok(s) || !fs) {
        return 1;
    }
    s = fs->OpenVolume(fs, &root);
    if (!efi_ok(s) || !root) {
        return 1;
    }
    CHAR16 path[] = {'\\','E','F','I','\\','F','I','N','T','E','\\',
                     'K','E','R','N','E','L','.','E','L','F',0};
    EFI_FILE_PROTOCOL *file = NULL;
    s = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (!efi_ok(s) || !file) {
        return 1;
    }
    UINTN size = sizeof(kernel_file);
    s = file->Read(file, &size, kernel_file);
    file->Close(file);
    if (!efi_ok(s) || size < sizeof(elf64_header_t)) {
        return 1;
    }
    *out = root;
    return (int)size;
}

static void sync_instruction_cache(uint64_t start, uint64_t end)
{
    uint64_t p;
    for (p = start & ~63ull; p < end; p += 64) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" : : : "memory");
    for (p = start & ~63ull; p < end; p += 64) {
        __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    conout(st, "\nFinte arm64 UEFI loader\n");
    if (!st || !st->BootServices) {
        fail(st, "no boot services");
    }
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);

    EFI_FILE_PROTOCOL *root = NULL;
    int file_size = open_kernel(st, image, &root);
    if (file_size <= 0) {
        fail(st, "cannot read \\EFI\\FINTE\\KERNEL.ELF");
    }
    (void)root;

    const elf64_header_t *eh = (const elf64_header_t *)kernel_file;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != 2 || eh->ident[5] != 1 ||
        eh->machine != ELF_MACHINE_AARCH64) {
        fail(st, "kernel is not a little-endian ELF64 AArch64 image");
    }
    if (eh->phentsize != sizeof(elf64_phdr_t) ||
        eh->phoff + (uint64_t)eh->phnum * eh->phentsize > (uint64_t)file_size) {
        fail(st, "bad ELF program headers");
    }

    uint64_t lo = ~0ull;
    uint64_t hi = 0;
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(kernel_file + eh->phoff);
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) {
            continue;
        }
        if (ph[i].filesz > ph[i].memsz ||
            ph[i].offset + ph[i].filesz > (uint64_t)file_size) {
            fail(st, "bad ELF segment");
        }
        uint64_t start = ph[i].paddr;
        uint64_t end = ph[i].paddr + ph[i].memsz;
        if (start < lo) {
            lo = start;
        }
        if (end > hi) {
            hi = end;
        }
    }
    if (lo > hi || lo < KERNEL_BASE) {
        fail(st, "kernel link address is unsupported");
    }

    uint64_t alloc_base = lo & ~0xFFFull;
    uint64_t alloc_end = (hi + 0xFFFull) & ~0xFFFull;
    EFI_PHYSICAL_ADDRESS addr = alloc_base;
    UINTN pages = (UINTN)((alloc_end - alloc_base) / 4096);
    EFI_STATUS s = st->BootServices->AllocatePages(
        EFI_ALLOCATE_ADDRESS, EFI_LOADER_CODE, pages, &addr);
    if (!efi_ok(s) || addr != alloc_base) {
        fail(st, "cannot reserve the kernel's physical range");
    }
    memset((void *)(uintptr_t)alloc_base, 0, (size_t)(alloc_end - alloc_base));
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != ELF_PT_LOAD || ph[i].memsz == 0) {
            continue;
        }
        memcpy((void *)(uintptr_t)ph[i].paddr, kernel_file + ph[i].offset,
               (size_t)ph[i].filesz);
    }
    sync_instruction_cache(alloc_base, alloc_end);

    UINTN map_size = sizeof(memory_map);
    UINTN map_key = 0;
    UINTN desc_size = 0;
    uint32_t desc_version = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        map_size = sizeof(memory_map);
        s = st->BootServices->GetMemoryMap(
            &map_size, (EFI_MEMORY_DESCRIPTOR *)memory_map, &map_key,
            &desc_size, &desc_version);
        if (!efi_ok(s)) {
            fail(st, "GetMemoryMap failed");
        }
        s = st->BootServices->ExitBootServices(image, map_key);
        if (efi_ok(s)) {
            break;
        }
    }
    if (!efi_ok(s)) {
        fail(st, "ExitBootServices failed");
    }

    typedef void (*entry_fn)(uint64_t, uint64_t, uint64_t);
    entry_fn entry = (entry_fn)(uintptr_t)eh->entry;
    __asm__ volatile("msr daifset, #3" : : : "memory");
    entry(0, 0, 0);
    for (;;) {
        __asm__ volatile("wfi");
    }
}
