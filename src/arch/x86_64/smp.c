/* SMP bring-up: xAPIC mapping, INIT/SIPI trampoline, AP entry.
 *
 * APs are started with a broadcast INIT/SIPI and identify themselves from the
 * local APIC ID. The BSP waits briefly for the online count to settle, so no
 * ACPI table parsing is required for the first SMP implementation. */
#include "finte/finte.h"
#include "smp.h"

#define AP_TRAMP_BASE 0x8000u
#define AP_BOOT_BASE 0x8F00u
#define LAPIC_BASE 0xFEE00000ull
#define LAPIC_ICR_LOW 0x300u
#define LAPIC_ICR_HIGH 0x310u
#define LAPIC_EOI 0xB0u
#define LAPIC_SVR 0xF0u
#define LAPIC_TPR 0x80u
#define LAPIC_LVT_TIMER 0x320u
#define LAPIC_LVT_LINT0 0x350u
#define LAPIC_LVT_LINT1 0x360u
#define LAPIC_LVT_ERROR 0x370u

extern const unsigned char ap_trampoline_bin[];
extern const unsigned int ap_trampoline_bin_len;

static volatile uint32_t g_ap_online;
static volatile int g_smp_ready;
static int g_tsc_deadline;
static uint8_t g_saved_gdt[256] __attribute__((aligned(16)));

static void delay_ns(uint64_t ns);

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void map_lapic_page(void)
{
    uint64_t cr3 = read_cr3() & ~0xFFFull;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
    if (!(pml4[0] & 1u)) {
        return;
    }
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & ~0xFFFull);
    uint64_t *pd = NULL;
    if (!(pdpt[3] & 1u)) {
        pd = (uint64_t *)hal_pages_alloc(4096);
        if (!pd) {
            return;
        }
        finte_memset(pd, 0, 4096);
        pdpt[3] = (uint64_t)(uintptr_t)pd | 0x3u;
    } else {
        pd = (uint64_t *)(uintptr_t)(pdpt[3] & ~0xFFFull);
    }
    uint32_t idx = (uint32_t)((LAPIC_BASE >> 21) & 0x1FFu);
    if (!(pd[idx] & 1u)) {
        pd[idx] = (LAPIC_BASE & ~0x1FFFFFull) | 0x9Bu; /* P|RW|PS|PCD|PWT */
    } else if (!(pd[idx] & 0x80u)) {
        uint64_t *pt = (uint64_t *)(uintptr_t)(pd[idx] & ~0xFFFull);
        uint32_t pti = (uint32_t)((LAPIC_BASE >> 12) & 0x1FFu);
        if (!(pt[pti] & 1u)) {
            pt[pti] = (LAPIC_BASE & ~0xFFFull) | 0x1Bu;
        }
    }
    __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)LAPIC_BASE)
                     : "memory");
}

static uint32_t lapic_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + off);
}

static void lapic_write(uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + off) = value;
}

uint32_t smp_apic_id(void)
{
    return lapic_read(0x20u) >> 24;
}

static void lapic_init(void)
{
    lapic_write(LAPIC_SVR, 0x1FFu); /* enabled, spurious vector 0xFF */
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_EOI, 0);
    lapic_write(LAPIC_LVT_TIMER, 0x10000u); /* masked for now */
    lapic_write(LAPIC_LVT_LINT0, 0x10000u);
    lapic_write(LAPIC_LVT_LINT1, 0x10000u);
    lapic_write(LAPIC_LVT_ERROR, 0x10000u);
    lapic_write(LAPIC_EOI, 0);
}

static void lapic_wait_icr(void)
{
    while (lapic_read(LAPIC_ICR_LOW) & (1u << 12)) {
        __asm__ volatile("pause");
    }
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

void smp_lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

void smp_timer_rearm(void)
{
    uint64_t hz = hal_cycles_per_sec();
    if (!hz) {
        hz = 1000000000ull;
    }
    uint64_t delta = hz / 100ull;
    if (delta == 0) {
        delta = 1;
    }
    wrmsr(0x6E0u, hal_cycles() + delta);
}

void smp_timer_init_ap(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (g_tsc_deadline) {
        cpu->timer_kind = 1;
        lapic_write(LAPIC_LVT_TIMER, 0x10000u);
        return;
    }
    /* Calibrate this AP's LAPIC timer against the TSC. The BSP's LAPIC is
     * left untouched because OVMF uses it for firmware events. */
    lapic_write(0x3E0u, 0x8u); /* divide by 1 */
    lapic_write(LAPIC_LVT_TIMER, 0x10000u);
    lapic_write(0x380u, 0xFFFFFFFFu);
    delay_ns(200000ull);
    uint32_t c0 = lapic_read(0x390u);
    delay_ns(10000000ull);
    uint32_t c1 = lapic_read(0x390u);
    cpu->lapic_period = c0 - c1;
    lapic_write(0x380u, 0);
    lapic_write(LAPIC_LVT_TIMER, 0x10000u);
    if (cpu->lapic_period < 1000u) {
        cpu->timer_kind = 0;
    } else {
        cpu->timer_kind = 2;
    }
}

void hal_timer_start_cpu(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (cpu->timer_kind == 1) {
        lapic_write(LAPIC_LVT_TIMER, 0x20u | (1u << 18)); /* TSC-deadline */
        smp_timer_rearm();
    } else if (cpu->timer_kind == 2) {
        lapic_write(0x380u, cpu->lapic_period);
        lapic_write(LAPIC_LVT_TIMER, 0x20u | (1u << 17)); /* periodic */
    }
}

void hal_timer_stop_cpu(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (cpu->timer_kind) {
        lapic_write(LAPIC_LVT_TIMER, 0x10000u);
    }
}

static void lapic_send_ipi(uint32_t apic_id, uint32_t value)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, value);
    lapic_wait_icr();
}

void smp_send_resched(uint32_t apic_id)
{
    lapic_send_ipi(apic_id, 0x21u);
}

void smp_ipi_irq(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (cpu) {
        cpu->ipis++;
    }
    lapic_write(LAPIC_EOI, 0);
}

void smp_broadcast_resched(void)
{
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, 0x21u | (3u << 18));
    lapic_wait_icr();
}

void hal_cpu_wake(uint32_t index)
{
    if (!g_smp_ready) {
        return;
    }
    fcpu_t *cpu = hal_cpu_slot(index);
    if (!cpu || !cpu->online || index == hal_cpu_self()->id) {
        return;
    }
    if (!cpu->idle) {
        return;
    }
    smp_send_resched(cpu->apic_id);
}

void hal_cpu_wake_all(void)
{
    if (!g_smp_ready) {
        return;
    }
    smp_broadcast_resched();
}

void hal_cpu_idle(void)
{
    __asm__ volatile("sti; hlt; cli");
}

static void delay_ns(uint64_t ns)
{
    uint64_t end = hal_now_ns() + ns;
    while (hal_now_ns() < end) {
        __asm__ volatile("pause");
    }
}

typedef struct __attribute__((packed)) smp_desc {
    uint16_t limit;
    uint64_t base;
} smp_desc_t;

static void patch64(uint32_t off, uint64_t value)
{
    *(uint64_t *)(uintptr_t)(AP_BOOT_BASE + off) = value;
}

static void patch_desc(uint32_t off, const smp_desc_t *d)
{
    *(uint16_t *)(uintptr_t)(AP_BOOT_BASE + off) = d->limit;
    *(uint64_t *)(uintptr_t)(AP_BOOT_BASE + off + 2) = d->base;
}

void ap_main(void *boot, uint32_t index);

void ap_main(void *boot, uint32_t index)
{
    (void)boot;
    fcpu_t *cpu = hal_cpu_slot(index);
    if (!cpu) {
        for (;;) {
            __asm__ volatile("cli; hlt");
        }
    }
    cpu->id = index;
    cpu->apic_id = lapic_read(0x20u) >> 24;
    cpu->online = 1;
    cpu->timer_kind = 1;
    hal_cpu_install(cpu);
    hal_cpu_mark_online(index);
    lapic_init();
    smp_timer_init_ap();
    __atomic_fetch_add(&g_ap_online, 1, __ATOMIC_SEQ_CST);

    char buf[64];
    int n = finte_format(buf, sizeof(buf), "[smp] cpu %u online (apic %u)\n",
                         (unsigned)index, (unsigned)cpu->apic_id);
    hal_console_write(buf, (size_t)n);

    finte_sched_ap_run();
}

int smp_init(void)
{
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    if (!(edx & (1u << 9))) {
        return 0; /* no local APIC */
    }
    g_tsc_deadline = (ecx & (1u << 24)) != 0;
    map_lapic_page();
    g_smp_ready = 1;

    smp_desc_t gdtr;
    smp_desc_t idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    uint16_t cs;
    uint16_t ds;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));

    /* The trampoline overwrites low memory, and the BIOS stage2 GDT lives
     * there. Preserve the live GDT in kernel memory for the APs. */
    uint32_t glen = (uint32_t)gdtr.limit + 1u;
    if (glen > sizeof(g_saved_gdt)) {
        glen = sizeof(g_saved_gdt);
    }
    finte_memcpy(g_saved_gdt, (const void *)(uintptr_t)gdtr.base, glen);
    gdtr.base = (uint64_t)(uintptr_t)g_saved_gdt;
    smp_desc_t newgdt = { (uint16_t)(glen - 1),
                          (uint64_t)(uintptr_t)g_saved_gdt };
    __asm__ volatile("lgdt %0" : : "m"(newgdt));

    if (ap_trampoline_bin_len > 0x1000u) {
        return 0;
    }
    finte_memset((void *)(uintptr_t)AP_TRAMP_BASE, 0, 0x1000);
    finte_memcpy((void *)(uintptr_t)AP_TRAMP_BASE, ap_trampoline_bin,
                 ap_trampoline_bin_len);

    patch64(0x00, read_cr3());
    patch64(0x10, (uint64_t)(uintptr_t)ap_main);
    patch64(0x18, cs);
    patch_desc(0x20, &gdtr);
    patch_desc(0x30, &idtr);
    patch64(0x40, ds);
    patch64(0x48, AP_BOOT_BASE);
    patch64(0x50, LAPIC_BASE);

    *(uint32_t *)(uintptr_t)(AP_BOOT_BASE + 0xA0) = 1; /* skip BSP slot */
    for (uint32_t i = 1; i < FINTE_MAX_CPUS; i++) {
        void *stack = hal_pages_alloc(16384);
        uint64_t top = stack ?
            ((uint64_t)(uintptr_t)stack + 16384u) & ~0xFull : 0;
        *(uint64_t *)(uintptr_t)(AP_BOOT_BASE + 0x60 + i * 8) = top;
    }

    /* Broadcast INIT, then two SIPIs with the trampoline at 0x8000. */
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, (5u << 8) | (1u << 14) | (3u << 18));
    lapic_wait_icr();
    delay_ns(10000000ull); /* 10 ms */

    lapic_write(LAPIC_ICR_LOW, 0x08u | (6u << 8) | (1u << 14) | (3u << 18));
    lapic_wait_icr();
    delay_ns(200000ull);
    lapic_write(LAPIC_ICR_LOW, 0x08u | (6u << 8) | (1u << 14) | (3u << 18));
    lapic_wait_icr();


    uint64_t last_change = hal_now_ns();
    uint32_t last = 0;
    for (uint32_t i = 0; i < 200; i++) {
        delay_ns(1000000ull);
        uint32_t now = __atomic_load_n(&g_ap_online, __ATOMIC_SEQ_CST);
        if (now != last) {
            last = now;
            last_change = hal_now_ns();
        } else if (now && hal_now_ns() - last_change > 20000000ull) {
            break;
        }
    }
    return (int)__atomic_load_n(&g_ap_online, __ATOMIC_SEQ_CST);
}

uint32_t smp_cpu_count(void)
{
    uint32_t n = hal_cpu_count();
    uint32_t ap = __atomic_load_n(&g_ap_online, __ATOMIC_SEQ_CST);
    return ap + 1 < n ? n : ap + 1;
}
