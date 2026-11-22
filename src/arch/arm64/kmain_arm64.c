/* Minimal arm64 QEMU virt kernel: initialize the HAL and run the core suite. */
#include "finte/finte.h"
#include "finte/boot.h"
#include "finte/selftest.h"

extern char __bss_end[];
extern void arm64_mmu_init(const fboot_info_t *bi);
extern int arm64_mmu_enabled(void);
extern int arm64_mmu_va_bits(void);

static volatile uint32_t g_smp_hits[FINTE_MAX_CPUS];

static int arm64_translates(uint64_t va, uint64_t *pa)
{
    uint64_t par;
    __asm__ volatile("at s1e1r, %1; isb; mrs %0, par_el1"
                     : "=r"(par) : "r"(va) : "memory");
    if (par & 1ull) {
        return 0;
    }
    *pa = (par & 0x0000FFFFFFFFF000ull) | (va & 0xFFFull);
    return 1;
}

static void smp_probe_flow(fflow_t *f)
{
    (void)f;
    fcpu_t *cpu = hal_cpu_self();
    if (cpu && cpu->id < FINTE_MAX_CPUS) {
        __atomic_add_fetch(&g_smp_hits[cpu->id], 1, __ATOMIC_RELAXED);
    }
}

static const fprogram_t g_smp_program = { "arm64-smp", smp_probe_flow, 4096 };

static void aputs(const char *s)
{
    hal_console_write(s, finte_strlen(s));
}

static void pmm_boot_reserve(const fboot_info_t *bi)
{
    if (!bi) {
        return;
    }
    if (pmm_init(bi, 0x40000000ull, 0x100000000ull) != 0) {
        return;
    }
    uint64_t kbase = 0x40080000ull;
    uint64_t kend = (uint64_t)(uintptr_t)__bss_end + 0x20000ull;
    if (kend > kbase) {
        (void)pmm_reserve(kbase, (size_t)(kend - kbase));
    }
    (void)pmm_reserve((uint64_t)(uintptr_t)bi,
                      bi->total_size ? bi->total_size : sizeof(*bi));
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
    if (bi->ramdisk_size) {
        (void)pmm_reserve(bi->ramdisk_addr, bi->ramdisk_size);
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
    pmm_boot_reserve(bi);
    arm64_mmu_init(bi);
    aputs("Finte " FINTE_VERSION_STRING " | arm64 bare metal\n");
    if (arm64_mmu_enabled()) {
        char mmu[64];
        int mn = finte_format(mmu, sizeof(mmu),
                              "mmu: %d-bit identity map enabled\n",
                              arm64_mmu_va_bits());
        if (mn > 0) {
            hal_console_write(mmu, (size_t)mn);
        }
        uint64_t kpa = 0;
        uint64_t dev_pa = 0;
        if (arm64_translates(0x40080000ull, &kpa) &&
            arm64_translates(0x09000000ull, &dev_pa) &&
            kpa == 0x40080000ull && dev_pa == 0x09000000ull) {
            aputs("mmu: identity translations ok\n");
        } else {
            aputs("mmu: identity translations FAILED\n");
            hal_exit(1);
        }
    } else {
        aputs("mmu: identity map unavailable\n");
    }
    if (pmm_active()) {
        char pm[128];
        int pn = finte_format(pm, sizeof(pm),
                              "pmm: usable=%llu free=%llu reserved=%llu\n",
                              (unsigned long long)pmm_usable_pages(),
                              (unsigned long long)pmm_free_page_count(),
                              (unsigned long long)pmm_reserved_pages());
        if (pn > 0) {
            hal_console_write(pm, (size_t)pn);
        }
    }
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

    extern int gic_version(void);
    fcpu_t *cpu0 = hal_cpu_slot(0);
    uint64_t before = cpu0 ? cpu0->ticks : 0;
    hal_timer_start_cpu();
    hal_irq_enable();
    uint64_t t0 = hal_now_ns();
    while (hal_now_ns() - t0 < 100000000ull) {
        __asm__ volatile("yield");
    }
    hal_irq_disable();
    hal_timer_stop_cpu();
    uint64_t after = cpu0 ? cpu0->ticks : 0;
    n = finte_format(buf, sizeof(buf),
                     "arm64 timer: gic=v%d ticks=%llu\n",
                     gic_version(), (unsigned long long)(after - before));
    hal_console_write(buf, (size_t)n);

    extern int arm64_smp_init(void);
    int aps = arm64_smp_init();
    n = finte_format(buf, sizeof(buf),
                     "arm64 smp: %d application processor(s) online, %u cpu(s) total\n",
                     aps, (unsigned)hal_cpu_count());
    hal_console_write(buf, (size_t)n);

    if (aps > 0 && hal_cpu_count() > 1) {
        finte_memset((void *)g_smp_hits, 0, sizeof(g_smp_hits));
        uint32_t count = hal_cpu_count() * 2u;
        for (uint32_t i = 0; i < count; i++) {
            fhandle_t h = finte_flow_create("arm64-smp", &g_smp_program,
                                            4096, FINTE_PERSONA_NATIVE);
            if (h == FINTE_HANDLE_NONE) {
                break;
            }
            (void)finte_flow_start(h);
        }
        finte_run();
        n = finte_format(buf, sizeof(buf),
                         "arm64 smp: hits=%u/%u/%u/%u\n",
                         (unsigned)g_smp_hits[0],
                         (unsigned)g_smp_hits[1],
                         (unsigned)g_smp_hits[2],
                         (unsigned)g_smp_hits[3]);
        hal_console_write(buf, (size_t)n);
    }

    hal_exit(rc == 0 ? 0 : 1);
}
