#ifndef FINTE_USERMODE_H
#define FINTE_USERMODE_H

#include <stdint.h>

int finte_usermode_run(const char *name);
void usermode_prepare(void);
void usermode_prepare_cpu(uint32_t cpu);

/* Called from the exception handler for a fault taken with CPL=3.  If the
 * current flow owns a user process it prints the fault, kills that process and
 * never returns; otherwise it returns so the kernel panic path can run. */
void finte_user_fault(uint64_t vec, uint64_t rip, uint64_t cr2);

/* Runs the isolated multi-process IPC/time/scheduling demo. */
int finte_proc_demo(void);

#endif
