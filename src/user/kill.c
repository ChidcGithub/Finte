/* Signal sender: after a short delay, deliver SIGUSR1 to pid 1. */
#include "syscall.h"

#define SYS_KILL 11
#define SYS_EXIT 60
#define SYS_SLEEP 201

void _start(void)
{
    fuser_syscall(SYS_SLEEP, 10000000, 0, 0);
    fuser_syscall(SYS_KILL, 1, 10, 0);
    fuser_syscall(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}
