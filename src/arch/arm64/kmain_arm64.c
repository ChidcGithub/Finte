/* Minimal arm64 QEMU virt kernel: initialize the HAL and run the core suite. */
#include "finte/finte.h"
#include "finte/boot.h"
#include "finte/selftest.h"

static void aputs(const char *s)
{
    hal_console_write(s, finte_strlen(s));
}

void kmain_arm64(uint64_t boot_arg, uint64_t magic)
{
    const fboot_info_t *bi = NULL;
    int boot_error = FINTE_BOOT_OK;
    if (magic == FINTE_BOOT_MAGIC) {
        fboot_info_t *candidate = (fboot_info_t *)(uintptr_t)boot_arg;
        boot_error = finte_boot_validate(candidate, FINTE_BOOT_ARCH_ARM64);
        if (boot_error == FINTE_BOOT_OK) {
            bi = candidate;
        }
    }

    hal_init();
    aputs("Finte " FINTE_VERSION_STRING " | arm64 bare metal\n");
    aputs(bi ? "boot: UEFI -> EL1, PL011 console\n"
             : "boot: QEMU virt -> EL1, PL011 console\n");
    if (bi) {
        char boot[160];
        int n = finte_format(boot, sizeof(boot),
                             "boot: protocol v%u memmap=%u acpi=%u dtb=%u fb=%u\n",
                             (unsigned)bi->version,
                             (unsigned)bi->memmap_count,
                             (bi->flags & FINTE_BOOT_FLAG_ACPI) ? 1u : 0u,
                             (bi->flags & FINTE_BOOT_FLAG_DTB) ? 1u : 0u,
                             (bi->flags & FINTE_BOOT_FLAG_FRAMEBUFFER) ? 1u : 0u);
        if (n > 0) {
            hal_console_write(boot, (size_t)n);
        }
    } else if (magic == FINTE_BOOT_MAGIC) {
        char err[64];
        int n = finte_format(err, sizeof(err),
                             "boot: invalid boot info (%d)\n", boot_error);
        if (n > 0) {
            hal_console_write(err, (size_t)n);
        }
    }

    finte_init();
    fselftest_report_t report;
    int rc = finte_selftest_run(2, &report);

    char buf[192];
    int n = finte_format(buf, sizeof(buf),
                         "arm64 self-check: %s (%u sections, %u checks, "
                         "%u failures)\n",
                         rc == 0 ? "PASS" : "FAIL",
                         (unsigned)report.sections,
                         (unsigned)report.checks,
                         (unsigned)report.failures);
    hal_console_write(buf, (size_t)n);
    hal_exit(rc == 0 ? 0 : 1);
}
