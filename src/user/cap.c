/* User-to-user capability transfer: send the channel cap from pid 1 to pid 2,
 * then have pid 2 use the received cap to send a message back. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_GETPID 39
#define SYS_EXIT 60
#define SYS_IPC_SEND 210
#define SYS_IPC_RECV 211
#define SYS_IPC_SEND_CAP 212
#define SYS_IPC_RECV_CAP 213

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
    long pid = sc(SYS_GETPID, 0, 0, 0);
    if (pid == 1) {
        /* Slot 0 = cap channel (SEND), slot 1 = reply channel (SEND|RECV). */
        long sent = sc(SYS_IPC_SEND_CAP, 0, 1, 0);
        long n = sc(SYS_IPC_RECV, 1, (long)buf, 4);
        int ok = sent >= 0 && n == 4 && buf[0] == 'p' && buf[1] == 'o';
        print(ok ? "[cap] p1 OK\n" : "[cap] p1 FAIL\n");
        sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    } else if (pid == 2) {
        /* Slot 0 = cap channel (RECV); the reply SEND cap arrives by transfer. */
        long cap = sc(SYS_IPC_RECV_CAP, 0, 0, 0);
        if (cap < 0) {
            print("[cap] p2 recv FAIL\n");
            sc(SYS_EXIT, 1, 0, 0);
        }
        long n = sc(SYS_IPC_SEND, cap, (long)"pong", 4);
        print(n == 4 ? "[cap] p2 OK\n" : "[cap] p2 FAIL\n");
        sc(SYS_EXIT, n == 4 ? 0 : 1, 0, 0);
    } else {
        print("[cap] unknown pid\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    for (;;) {
    }
}
