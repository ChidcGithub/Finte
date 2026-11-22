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
    const fboot_info_t *bi = NULL;
    int boot_error = FINTE_BOOT_OK;
    if (magic == FINTE_BOOT_MAGIC) {
        fboot_info_t *candidate = (fboot_info_t *)(uintptr_t)boot_arg;
        boot_error = finte_boot_validate(candidate, FINTE_BOOT_ARCH_X86_64);
        if (boot_error == FINTE_BOOT_OK) {
            bi = candidate;
        }
    }

    hal_boot_info(bi);
    blk_boot_info(bi);
    input_boot(bi);
    hal_init();
    faults_init();
    usermode_prepare();
    timer_init();
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
