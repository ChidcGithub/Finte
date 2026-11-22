/* Minimal ring-3 program. Loaded as an ELF64 image by the kernel. */
#include "syscall.h"

static const char message[] = "Hello from ring 3 via syscall\n";

static long syscall3(long nr, long a, long b)
{
    return fuser_syscall(nr, a, b, 0);
}

void _start(void)
{
    syscall3(1, (long)message, (long)sizeof(message) - 1);
    syscall3(60, 0, 0);
    for (;;) {
    }
}
