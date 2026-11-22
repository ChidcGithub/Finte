/* QEMU virt GICv2 + generic timer IRQ and exception reporting. */
#include "finte/finte.h"

#define GICD_BASE 0x08000000u
#define GICC_BASE 0x08010000u

#define GICD_CTLR      (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER (*(volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_ICENABLER (*(volatile uint32_t *)(GICD_BASE + 0x180))
#define GICD_IPRIORITY ((volatile uint8_t *)(GICD_BASE + 0x400))
#define GICD_ITARGETS  ((volatile uint8_t *)(GICD_BASE + 0x800))
#define GICD_PIDR2     (*(volatile uint32_t *)(GICD_BASE + 0xFE8))
#define GICD_SGIR      (*(volatile uint32_t *)(GICD_BASE + 0xF00))
#define GICC_CTLR      (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR       (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR       (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR      (*(volatile uint32_t *)(GICC_BASE + 0x010))

#define GIC_IRQ_TIMER 30u
#define GIC_IRQ_RESCHED 0u

#define FRAME_ELR  31u
#define FRAME_SPSR 32u
#define FRAME_ESR  33u
#define FRAME_FAR  34u

static int g_gic_version;

int gic_version(void)
{
    return g_gic_version;
}

void gic_init(void)
{
    g_gic_version = (int)((GICD_PIDR2 >> 4) & 0xFu);
    if (g_gic_version != 2) {
        return;
    }
    GICD_CTLR = 0;
    for (uint32_t i = 0; i < 32; i++) {
        (&GICD_ICENABLER)[i] = 0xFFFFFFFFu;
    }
    for (uint32_t i = 0; i < 256; i++) {
        GICD_IPRIORITY[i * 4] = 0xA0;
        GICD_IPRIORITY[i * 4 + 1] = 0xA0;
        GICD_IPRIORITY[i * 4 + 2] = 0xA0;
        GICD_IPRIORITY[i * 4 + 3] = 0xA0;
    }
    GICD_ITARGETS[GIC_IRQ_TIMER] = 1;
    GICD_ITARGETS[GIC_IRQ_RESCHED] = 1;
    GICD_CTLR = 1;
}

void gic_init_cpu(void)
{
    if (g_gic_version != 2) {
        return;
    }
    GICC_CTLR = 0;
    GICC_PMR = 0xFF;
    GICC_CTLR = 1;
    /* SGI 0 is a reschedule IPI; PPI 30 is the EL1 physical timer. */
    GICD_ISENABLER = (1u << GIC_IRQ_RESCHED) | (1u << GIC_IRQ_TIMER);
}

void gic_send_sgi(uint32_t cpu, uint32_t irq)
{
    if (g_gic_version != 2 || cpu >= 8 || irq > 15) {
        return;
    }
    GICD_SGIR = ((1u << cpu) << 16) | (irq & 0xFu);
    __asm__ volatile("dsb sy" : : : "memory");
}

static uint32_t gic_ack(void)
{
    return GICC_IAR;
}

static void gic_eoi(uint32_t iar)
{
    GICC_EOIR = iar;
}

static void arm64_timer_irq(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (cpu) {
        cpu->ticks++;
    }
    __asm__ volatile("msr cntp_ctl_el0, xzr" : : : "memory");
    uint64_t freq = hal_cycles_per_sec();
    if (freq) {
        uint64_t tval = freq / 100u;
        if (tval == 0) {
            tval = 1;
        }
        __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(tval) : "memory");
        __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1ull) : "memory");
    }
    finte_sched_tick();
    finte_proc_timer_tick();
    if (cpu && cpu->current) {
        finte_sched_preempt(cpu->current);
    }
}

void arm64_irq_handler(uint64_t *frame)
{
    if (g_gic_version != 2) {
        return;
    }
    uint32_t iar = gic_ack();
    uint32_t irq = iar & 0x3FFu;
    int from_el0 = (frame[FRAME_SPSR] & 0xFu) == 0;
    if (irq < 1020u) {
        gic_eoi(iar);
    }
    if (irq == GIC_IRQ_TIMER) {
        arm64_timer_irq();
        finte_proc_cpu_tick(10000000ull, from_el0);
        if (from_el0) {
            fflow_t *f = finte_flow_current();
            fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
            if (p && finte_proc_signal_pending(p)) {
                finte_process_signal_check(p, frame);
            }
        }
    }
}

void arm64_sync_handler(uint64_t *frame)
{
    uint64_t esr = frame[FRAME_ESR];
    uint64_t far = frame[FRAME_FAR];
    uint64_t elr = frame[FRAME_ELR];
    uint64_t spsr = frame[FRAME_SPSR];
    unsigned ec = (unsigned)((esr >> 26) & 0x3Fu);
    int from_el0 = (spsr & 0xFu) == 0;

    if (from_el0 && ec == 0x15u) { /* SVC from AArch64 EL0 */
        fflow_t *f = finte_flow_current();
        fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
        uint64_t nr = frame[8];
        uint64_t a0 = frame[0];
        uint64_t a1 = frame[1];
        uint64_t a2 = frame[2];
        frame[0] = finte_proc_syscall(p, nr, a0, a1, a2, frame);
        if (p) {
            finte_process_signal_check(p, frame);
        }
        /* SVC's preferred return address already points past the
         * instruction; do not advance ELR again. */
        return;
    }
    if (from_el0 && ec == 0x24u && (esr & (1ull << 6))) {
        fflow_t *f = finte_flow_current();
        fproc_t *p = (f && f->user) ? (fproc_t *)f->user : NULL;
        if (p && p->space && finte_as_cow_fault(p->space, far, 1) == 0) {
            return; /* retry the write with the private page */
        }
    }
    if (from_el0 && (ec == 0x20u || ec == 0x21u || ec == 0x22u ||
                     ec == 0x24u || ec == 0x25u || ec == 0x26u ||
                     ec == 0x3Cu)) {
        finte_user_fault(ec, elr, far);
        /* No user process owned this fault: fall through to the panic. */
    }

    char buf[192];
    int n = finte_format(buf, sizeof(buf),
                         "\n[arm64] exception ec=0x%x esr=0x%llx far=0x%llx "
                         "elr=0x%llx spsr=0x%llx\n",
                         ec, (unsigned long long)esr,
                         (unsigned long long)far, (unsigned long long)elr,
                         (unsigned long long)spsr);
    if (n > 0) {
        hal_console_write(buf, (size_t)n);
    }
    hal_panic("arm64 exception");
}
