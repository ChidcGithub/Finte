/* Process-group test: setpgid/getpgrp, group stop/continue and group kill. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_FORK 9
#define SYS_KILL 11
#define SYS_GETPID 39
#define SYS_WAITPID 40
#define SYS_SETPGID 42
#define SYS_GETPGRP 43
#define SYS_EXIT 60
#define SYS_YIELD 202

#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 4

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

#define WIFSTOPPED(s) (((s) & 0x7F) == 0x7F)
#define WSTOPSIG(s) (((s) >> 8) & 0xFF)
#define WIFCONTINUED(s) ((s) == 0xFFFF)
#define WIFEXITED(s) (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

static void child_loop(void)
{
    for (;;) {
        sc(SYS_GETPID, 0, 0, 0);
        sc(SYS_YIELD, 0, 0, 0);
    }
}

void _start(void)
{
    int ok = 1;
    long self = sc(SYS_GETPID, 0, 0, 0);
    ok = ok && self > 0 && sc(SYS_GETPGRP, 0, 0, 0) == self;

    long a = sc(SYS_FORK, 0, 0, 0);
    if (a == 0) {
        child_loop();
    }
    long b = sc(SYS_FORK, 0, 0, 0);
    if (b == 0) {
        child_loop();
    }
    if (a <= 0 || b <= 0 || a == b) {
        print("[pgroup] fork FAIL\n");
        sc(SYS_EXIT, 1, 0, 0);
    }

    /* Both children join a new group named after the first child. */
    int group = (int)a;
    ok = ok && sc(SYS_SETPGID, a, group, 0) == 0;
    ok = ok && sc(SYS_SETPGID, b, group, 0) == 0;
    ok = ok && sc(SYS_KILL, -group, 0, 0) == 0;
    ok = ok && sc(SYS_KILL, -9999, 0, 0) < 0;
    int status = 0;
    ok = ok && sc(SYS_WAITPID, -group, (long)&status, WNOHANG) == 0;

    int s1 = 0;
    int s2 = 0;
    long w1;
    long w2;

    ok = ok && sc(SYS_KILL, -group, SIGSTOP, 0) == 0;
    w1 = sc(SYS_WAITPID, -group, (long)&s1, WUNTRACED);
    w2 = sc(SYS_WAITPID, -group, (long)&s2, WUNTRACED);
    ok = ok && ((w1 == a && w2 == b) || (w1 == b && w2 == a));
    ok = ok && WIFSTOPPED(s1) && WIFSTOPPED(s2);
    ok = ok && WSTOPSIG(s1) == SIGSTOP && WSTOPSIG(s2) == SIGSTOP;

    ok = ok && sc(SYS_KILL, -group, SIGCONT, 0) == 0;
    w1 = sc(SYS_WAITPID, -group, (long)&s1, WCONTINUED);
    w2 = sc(SYS_WAITPID, -group, (long)&s2, WCONTINUED);
    ok = ok && ((w1 == a && w2 == b) || (w1 == b && w2 == a));
    ok = ok && WIFCONTINUED(s1) && WIFCONTINUED(s2);

    ok = ok && sc(SYS_KILL, -group, SIGTERM, 0) == 0;
    w1 = sc(SYS_WAITPID, -group, (long)&s1, 0);
    w2 = sc(SYS_WAITPID, -group, (long)&s2, 0);
    ok = ok && ((w1 == a && w2 == b) || (w1 == b && w2 == a));
    ok = ok && WIFEXITED(s1) && WIFEXITED(s2);
    ok = ok && WEXITSTATUS(s1) == 128 + SIGTERM;
    ok = ok && WEXITSTATUS(s2) == 128 + SIGTERM;

    print(ok ? "[pgroup] OK\n" : "[pgroup] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
