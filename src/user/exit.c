/* Immediate-exit image used by the process churn test. */
#include "syscall.h"

void _start(void)
{
    fuser_syscall(60, 0, 0, 0);
    for (;;) {
    }
}
