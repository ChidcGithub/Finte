/* Kernel power-on self-test.
 *
 * Runs before the demo image is built. It checks the CPU mode, descriptor
 * tables, kernel mapping, clock and heap, then runs the shared core suite.
 * Any failure is fatal: the kernel never starts user work with a broken core.
 */
#include "finte/finte.h"
#include "finte/boot.h"
#include "finte/selftest.h"

extern const char _start64[];
uint64_t faults_idt_base(void);
uint32_t faults_idt_limit(void);

typedef struct __attribute__((packed)) desc_ptr {
    uint16_t limit;
    uint64_t base;
} desc_ptr_t;

static int p_checks;
static int p_fails;

static void post_print(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        hal_console_write(buf, (size_t)n);
    }
}

static void post_check(const char *name, int ok, const char *detail)
{
    p_checks++;
    if (!ok) {
        p_fails++;
    }
    if (detail && *detail) {
        post_print("[post] %s: %s (%s)\n", name, ok ? "ok" : "FAIL", detail);
    } else {
        post_print("[post] %s: %s\n", name, ok ? "ok" : "FAIL");
    }
}

static uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static uint64_t read_efer(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    return ((uint64_t)hi << 32) | lo;
}

static uint16_t read_cs(void)
{
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

void finte_kernel_post(uint64_t boot_arg, uint64_t magic)
{
    desc_ptr_t gdtr;
    desc_ptr_t idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));

    uint64_t cr0 = read_cr0();
    uint64_t cr4 = read_cr4();
    uint64_t efer = read_efer();
    uint16_t cs = read_cs();

    int custom = magic == FINTE_BOOT_MAGIC;
    const fboot_info_t *bi = custom ?
        (const fboot_info_t *)(uintptr_t)boot_arg : NULL;
    int uefi = custom && bi && bi->magic == FINTE_BOOT_MAGIC &&
               bi->console_type == 2;

    post_check("boot protocol", custom || magic == FINTE_MULTIBOOT_MAGIC,
               uefi ? "custom UEFI loader" :
               (custom ? "custom BIOS loader" : "Multiboot"));
    if (custom) {
        post_check("boot info", bi != NULL &&
                                 bi->magic == FINTE_BOOT_MAGIC &&
                                 bi->kernel_size > 0 &&
                                 bi->ramdisk_size <= 0x40000,
                   "magic/ramdisk bounds");
    }
    post_check("long mode", (cr0 & 0x80000001ull) == 0x80000001ull &&
                            (cr4 & 0x20ull) != 0 &&
                            (efer & 0x500ull) == 0x500ull,
               "PE|PG, PAE, LME|LMA");
    post_check("gdt/cs", gdtr.base != 0 && gdtr.limit >= 0x17 &&
                         (uefi ? cs != 0 : cs == 0x08),
               uefi ? "UEFI 64-bit CS" : "64-bit CS 0x08");
    post_check("idt", idtr.base == faults_idt_base() &&
                      idtr.limit >= faults_idt_limit(),
               "32 exception gates");
    post_check("kernel mapping", ((uintptr_t)_start64 >= FINTE_KERNEL_BASE) &&
                                 *(const volatile uint8_t *)_start64 == 0xFA,
               "64 MiB entry mapped");

    uint64_t t1 = hal_cycles();
    for (volatile int i = 0; i < 10000; i++) {
    }
    uint64_t t2 = hal_cycles();
    post_check("tsc", t2 > t1 && t1 != 0, "monotonic");

    void *p = hal_pages_alloc(64);
    int heap_ok = p != NULL &&
                  ((uintptr_t)p & 4095u) == 0 &&
                  *(uint64_t *)p == 0;
    if (p) {
        *(uint64_t *)p = 0xF17E5EEDull;
        heap_ok = heap_ok && *(uint64_t *)p == 0xF17E5EEDull;
    }
    post_check("heap", heap_ok, "page aligned, writable");

    post_print("[post] tsc: %llu Hz\n",
               (unsigned long long)hal_cycles_per_sec());
    post_print("[post] kernel: 0x%llx + %u B\n",
               (unsigned long long)FINTE_KERNEL_BASE,
               bi ? (unsigned)bi->kernel_size : 0);
    if (bi) {
        post_print("[post] ramdisk: %u B @ 0x%x\n",
                   (unsigned)bi->ramdisk_size, (unsigned)bi->ramdisk_addr);
        if (bi->fb_addr) {
            post_print("[post] display: %ux%u GOP @ 0x%llx\n",
                       (unsigned)bi->fb_width, (unsigned)bi->fb_height,
                       (unsigned long long)bi->fb_addr);
        } else {
            post_print("[post] display: VGA text + COM1\n");
        }
    }

    fselftest_report_t report;
    int rc = finte_selftest_run(2, &report);
    post_check("core self-check", rc == 0 && report.failures == 0,
               report.failed_section[0] ? report.failed_section : "6 sections");

    post_print("[post] startup: %s (%d POST checks, %u core checks)\n",
               p_fails == 0 && report.failures == 0 ? "PASS" : "FAIL",
               p_checks, report.checks);
    if (p_fails || report.failures) {
        hal_panic("startup self-check failed");
    }
}
