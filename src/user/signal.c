/* Signal receiver: install SIGUSR1, wait for a sender, then report. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_SIGNAL 12
#define SYS_SIGRETURN 13
#define SYS_EXIT 60
#define SYS_SLEEP 201

#define SIGUSR1 10

static volatile long sig_count;

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

static void handler(int sig)
{
    (void)sig;
    sig_count++;
}

static void restorer(void)
{
    sc(SYS_SIGRETURN, 0, 0, 0);
}

void _start(void)
{
    if (sc(SYS_SIGNAL, SIGUSR1, (long)handler, (long)restorer) != 0) {
        print("[signal] install FAIL\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    for (int i = 0; i < 30 && sig_count == 0; i++) {
        sc(SYS_SLEEP, 10000000, 0, 0);
    }
    print(sig_count ? "[signal] OK\n" : "[signal] FAIL\n");
    sc(SYS_EXIT, sig_count ? 0 : 1, 0, 0);
    for (;;) {
    }
}
