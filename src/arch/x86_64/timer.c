/* PIT timer and preemptive scheduling. */
#include "finte/finte.h"
#include "timer.h"
#include "smp.h"

#define PIT_HZ 100u
#define PIT_DIVISOR (1193182u / PIT_HZ)

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

volatile uint64_t timer_ticks;

void timer_init(void)
{
    /* Remap the 8259 PIC to vectors 0x20..0x2F and route only IRQ0. */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFE); /* mask all but IRQ0 */
    outb(0xA1, 0xFF);

    /* PIT channel 0, mode 3, 100 Hz. */
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(PIT_DIVISOR & 0xFF));
    outb(0x40, (uint8_t)(PIT_DIVISOR >> 8));

    /* The timer gate is installed by faults_init. */
    __asm__ volatile("sti");
}

void timer_irq(void)
{
    fcpu_t *cpu = hal_cpu_self();
    if (cpu && cpu->timer_mode) {
        smp_lapic_eoi();
        if (cpu->timer_mode == FINTE_TIMER_ONESHOT && cpu->current) {
            smp_timer_rearm();
        }
    } else {
        outb(0x20, 0x20); /* EOI before a possible context switch */
    }
    timer_ticks++;
    if (cpu) {
        cpu->ticks++;
    }
    finte_sched_tick();
    finte_proc_timer_tick();
    fflow_t *f = cpu ? cpu->current : NULL;
    if (f && f->state == FINTE_FLOW_RUNNING && finte_sched_active() &&
        finte_sched_cpu_has_work()) {
        finte_sched_preempt(f);
    }
}
