/* Extended signal test: masks, pending delivery, multiple signals, SIGCHLD
 * and the default terminate actions for SIGTERM/SIGINT/SIGSEGV. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_FORK 9
#define SYS_KILL 11
#define SYS_SIGNAL 12
#define SYS_SIGRETURN 13
#define SYS_GETPID 39
#define SYS_WAITPID 40
#define SYS_SIGPROCMASK 41
#define SYS_EXIT 60
#define SYS_YIELD 202

#define SIGINT 2
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCHLD 17

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1

static volatile unsigned long sig_seen;
static volatile unsigned long chld_count;

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

static void mark(int sig)
{
    sig_seen |= 1ul << sig;
}

static void on_usr1(int sig)
{
    mark(sig);
}

static void on_usr2(int sig)
{
    mark(sig);
}

static void on_chld(int sig)
{
    (void)sig;
    chld_count++;
}

static void restorer(void)
{
    sc(SYS_SIGRETURN, 0, 0, 0);
}

static int install(int sig, void (*handler)(int))
{
    return sc(SYS_SIGNAL, sig, (long)handler, (long)restorer) == 0;
}

void _start(void)
{
    int ok = 1;
    ok = ok && install(SIGUSR1, on_usr1);
    ok = ok && install(SIGUSR2, on_usr2);
    ok = ok && install(SIGCHLD, on_chld);

    /* Block both signals, queue them, then unblock and deliver both. */
    unsigned long pair = (1ul << SIGUSR1) | (1ul << SIGUSR2);
    long old = sc(SYS_SIGPROCMASK, SIG_BLOCK, (long)pair, 0);
    ok = ok && old == 0;
    long self = sc(SYS_GETPID, 0, 0, 0);
    ok = ok && sc(SYS_KILL, self, SIGUSR1, 0) == 0;
    ok = ok && sc(SYS_KILL, self, SIGUSR2, 0) == 0;
    ok = ok && sig_seen == 0;
    long prev = sc(SYS_SIGPROCMASK, SIG_UNBLOCK, (long)pair, 0);
    ok = ok && prev == (long)pair;
    sc(SYS_YIELD, 0, 0, 0);
    ok = ok && (sig_seen & (1ul << SIGUSR1)) != 0;
    ok = ok && (sig_seen & (1ul << SIGUSR2)) != 0;

    /* Default terminate actions. */
    static const int fatal[] = { SIGTERM, SIGINT, SIGSEGV };
    for (unsigned i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        long pid = sc(SYS_FORK, 0, 0, 0);
        if (pid == 0) {
            long me = sc(SYS_GETPID, 0, 0, 0);
            sc(SYS_KILL, me, fatal[i], 0);
            for (;;) {
            }
        }
        int status = -1;
        long w = sc(SYS_WAITPID, pid, (long)&status, 0);
        ok = ok && pid > 0 && w == pid && status == 128 + fatal[i];
    }
    ok = ok && chld_count == 3;

    /* A normal child exit also queues SIGCHLD. */
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        sc(SYS_EXIT, 0, 0, 0);
        for (;;) {
        }
    }
    int status = -1;
    long w = sc(SYS_WAITPID, pid, (long)&status, 0);
    ok = ok && pid > 0 && w == pid && status == 0 && chld_count == 4;

    print(ok ? "[sigset] OK\n" : "[sigset] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
