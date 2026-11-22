/* Service provider: answer one request on its pre-registered channel caps.
 * Cap 0 is the request channel ("echo"), cap 1 the reply channel
 * ("echo-reply"); both are registered by the kernel before the flow starts. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_EXIT 60
#define SYS_IPC_SEND 210
#define SYS_IPC_RECV 211

static long sc(long nr, long a, long b, long c)
{
    return fuser_syscall(nr, a, b, c);
}

static void print(const char *s)
{
    long n = 0;
    while (s[n]) {
        n++;
    }
    sc(SYS_WRITE, (long)s, n, 0);
}

void _start(void)
{
    char buf[8];
    long n = sc(SYS_IPC_RECV, 0, (long)buf, 4);
    int ok = n == 4 && buf[0] == 'p' && buf[1] == 'i';
    if (ok) {
        sc(SYS_IPC_SEND, 1, (long)"pong", 4);
    }
    print(ok ? "[svc] provider OK\n" : "[svc] provider FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
