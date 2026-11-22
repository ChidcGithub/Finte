#include "finte/finte.h"
#include "finte/boot.h"
#include "finte/initrd.h"
#include "shell.h"
#include "input.h"
#include "blk.h"
#include "timer.h"
#include "smp.h"
#include "hal_x86.h"

void faults_init(void);
void finte_kernel_post(uint64_t boot_arg, uint64_t magic);

static void early_putc(char c)
{
    __asm__ volatile("outb %0, %1"
                     :
                     : "a"((unsigned char)c), "Nd"((unsigned short)0x3F8));
}

extern char __kernel_end[];

static uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v & ~0xFFFull;
}

static void reserve_page_tables(uint64_t table, int level)
{
    if (!table || level < 1 || level > 4) {
        return;
    }
    (void)pmm_reserve(table, 4096);
    uint64_t *pt = (uint64_t *)(uintptr_t)table;
    for (uint32_t i = 0; i < 512; i++) {
        uint64_t e = pt[i];
        if ((e & 1u) == 0) {
            continue;
        }
        if (level > 1 && (e & 0x80u)) {
            continue; /* huge page */
        }
        if (level > 1) {
            reserve_page_tables(e & 0x000FFFFFFFFFF000ull, level - 1);
        }
    }
}

static void pmm_boot_reserve(const fboot_info_t *bi)
{
    if (!bi) {
        return;
    }
    /* The x86 kernel runs on a 1 GiB identity map. Keep allocations below it
     * and above the BIOS/boot-data area. */
    if (pmm_init(bi, 0x1000000ull, 0x40000000ull) != 0) {
        return;
    }
    uint64_t kbase = FINTE_KERNEL_BASE;
    uint64_t kend = (uint64_t)(uintptr_t)__kernel_end;
    if (kend > kbase) {
        (void)pmm_reserve(kbase, (size_t)(kend - kbase));
    }
    (void)pmm_reserve((uint64_t)(uintptr_t)bi,
                      bi->total_size ? bi->total_size : sizeof(*bi));
    if (bi->ramdisk_size) {
        (void)pmm_reserve(bi->ramdisk_addr, bi->ramdisk_size);
    }
    if ((bi->flags & FINTE_BOOT_FLAG_MEMMAP) && bi->memmap_addr) {
        (void)pmm_reserve(bi->memmap_addr,
                          (size_t)bi->memmap_count * bi->memmap_desc_size);
    }
    if ((bi->flags & FINTE_BOOT_FLAG_MODULES) && bi->modules_addr) {
        (void)pmm_reserve(bi->modules_addr,
                          (size_t)bi->module_count * bi->module_desc_size);
        const fboot_module_t *mods =
            (const fboot_module_t *)(uintptr_t)bi->modules_addr;
        for (uint32_t i = 0; i < bi->module_count; i++) {
            (void)pmm_reserve(mods[i].addr, (size_t)mods[i].size);
        }
    }
    if (bi->acpi_addr) {
        (void)pmm_reserve(bi->acpi_addr, 1u << 20);
    }
    if (bi->dtb_addr) {
        (void)pmm_reserve(bi->dtb_addr, 1u << 20);
    }
    if ((bi->flags & FINTE_BOOT_FLAG_FRAMEBUFFER) && bi->fb_addr) {
        (void)pmm_reserve(bi->fb_addr,
                          (size_t)bi->fb_pitch * bi->fb_height);
    }
    reserve_page_tables(read_cr3(), 4);
}

static void kputs(const char *s)
{
    hal_console_write(s, finte_strlen(s));
}

static void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        hal_console_write(buf, (size_t)n);
    }
}

void kmain(uint64_t boot_arg, uint64_t magic)
{
    early_putc('F');
    const fboot_info_t *bi = NULL;
    int boot_error = FINTE_BOOT_OK;
    if (magic == FINTE_BOOT_MAGIC) {
        fboot_info_t *candidate = (fboot_info_t *)(uintptr_t)boot_arg;
        boot_error = finte_boot_validate(candidate, FINTE_BOOT_ARCH_X86_64);
        if (boot_error == FINTE_BOOT_OK) {
            bi = candidate;
        }
    }
    early_putc('G');

    hal_boot_info(bi);
    blk_boot_info(bi);
    input_boot(bi);
    early_putc('H');
    pmm_boot_reserve(bi);
    early_putc('I');
    hal_init();
    early_putc('J');
    faults_init();
    early_putc('K');
    usermode_prepare();
    early_putc('L');
    timer_init();
    early_putc('M');
    kputs("Finte " FINTE_VERSION_STRING " | x86_64 bare metal\n");
    if (bi) {
        kprintf("boot: %s -> long mode -> 1 GiB identity map\n",
                bi->platform == FINTE_BOOT_PLATFORM_UEFI
                    ? "custom UEFI loader"
                    : "custom BIOS loader");
        kprintf("boot: protocol v%u arch=%u memmap=%u modules=%u fw=%u\n",
                (unsigned)bi->version, (unsigned)bi->arch,
                (unsigned)bi->memmap_count, (unsigned)bi->module_count,
                (unsigned)bi->fw_services);
        if (pmm_active()) {
            kprintf("pmm: usable=%llu free=%llu reserved=%llu\n",
                    (unsigned long long)pmm_usable_pages(),
                    (unsigned long long)pmm_free_page_count(),
                    (unsigned long long)pmm_reserved_pages());
        }
    } else {
        if (magic == FINTE_BOOT_MAGIC && boot_error != FINTE_BOOT_OK) {
            kprintf("boot: invalid boot info (%d)\n", boot_error);
        } else {
            kputs("boot: Multiboot fallback -> long mode\n");
        }
    }
    kputs("mode: multi-core capable, preemptive flows, finite budgets\n");

    finte_init();
    if (bi && bi->ramdisk_size) {
        if (finte_initrd_init((const void *)(uintptr_t)bi->ramdisk_addr,
                              bi->ramdisk_size) == 0) {
            kprintf("ramdisk: %u files, %u bytes\n",
                    (unsigned)finte_initrd_count(),
                    (unsigned)bi->ramdisk_size);
        }
    }

    finte_kernel_post(boot_arg, magic);

    int aps = smp_init();
    kprintf("smp: %d application processor(s) online, %u cpu(s) total\n",
            aps, (unsigned)smp_cpu_count());

    kprintf("core: nodes=%u flows=%u channels=%u caps/flow=%u\n",
            FINTE_MAX_NODES, FINTE_MAX_FLOWS, FINTE_MAX_CHANNELS, FINTE_MAX_CAPS);
    finte_shell_run(bi);
    hal_panic("shell returned");
}
