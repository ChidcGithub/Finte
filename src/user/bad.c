/* Bad-syscall negative test: an unknown number must return -1. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_EXIT 60

static void print(const char *s)
{
    long n = 0;
    while (s[n]) {
        n++;
    }
    fuser_syscall(SYS_WRITE, (long)s, n, 0);
}

void _start(void)
{
    long rc = fuser_syscall(999, 0, 0, 0);
    print(rc == -1 ? "[abi] bad syscall OK\n" : "[abi] bad syscall FAIL\n");
    fuser_syscall(SYS_EXIT, rc == -1 ? 0 : 1, 0, 0);
    for (;;) {
    }
}
