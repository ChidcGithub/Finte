/* Exec-inheritance helper: read fd 9 and exit with the byte value. */
#include "syscall.h"

#define SYS_READ 3
#define SYS_EXIT 60

void _start(void)
{
    unsigned char c = 0;
    long n = fuser_syscall(SYS_READ, 9, (long)&c, 1);
    fuser_syscall(SYS_EXIT, n == 1 ? (long)c : 0, 0, 0);
    for (;;) {
    }
}
