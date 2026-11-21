#ifndef FINTE_TIMER_H
#define FINTE_TIMER_H

#include <stdint.h>

extern volatile uint64_t timer_ticks;
void timer_init(void);
void timer_irq(void);

#endif
