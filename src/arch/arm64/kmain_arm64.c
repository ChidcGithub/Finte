/* Minimal arm64 QEMU virt kernel: initialize the HAL and run the core suite. */
#include "finte/finte.h"
#include "finte/selftest.h"

static void aputs(const char *s)
{
    hal_console_write(s, finte_strlen(s));
}

void kmain_arm64(void)
{
    hal_init();
    aputs("Finte " FINTE_VERSION_STRING " | arm64 bare metal\n");
    aputs("boot: QEMU virt -> EL1, PL011 console\n");

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
