#ifndef FINTE_SMP_H
#define FINTE_SMP_H

#include <stdint.h>

int smp_init(void);
uint32_t smp_cpu_count(void);
uint32_t smp_apic_id(void);
void smp_send_resched(uint32_t apic_id);
void smp_broadcast_resched(void);
void smp_ipi_irq(void);
void smp_lapic_eoi(void);
void smp_timer_rearm(void);
void smp_timer_init_ap(void);

#endif
