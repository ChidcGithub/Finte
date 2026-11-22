/* Job-control test: stop, continue and wait status reporting. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_FORK 9
#define SYS_KILL 11
#define SYS_GETPID 39
#define SYS_WAITPID 40
#define SYS_EXIT 60
#define SYS_YIELD 202

#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19

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
#define WIFCONTINUED(s) ((s) == 0xFFFF)
#define WIFEXITED(s) (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

void _start(void)
{
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        for (;;) {
            sc(SYS_GETPID, 0, 0, 0);
            sc(SYS_YIELD, 0, 0, 0);
        }
    }
    if (pid < 0) {
        print("[job] fork FAIL\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    int ok = 1;
    int status = 0;
    long w = -1;

    ok = ok && sc(SYS_KILL, pid, SIGSTOP, 0) == 0;
    w = sc(SYS_WAITPID, pid, (long)&status, WUNTRACED);
    ok = ok && w == pid && WIFSTOPPED(status);

    ok = ok && sc(SYS_KILL, pid, SIGCONT, 0) == 0;
    status = 0;
    w = sc(SYS_WAITPID, pid, (long)&status, WCONTINUED);
    ok = ok && w == pid && WIFCONTINUED(status);

    ok = ok && sc(SYS_KILL, pid, SIGTERM, 0) == 0;
    status = 0;
    w = sc(SYS_WAITPID, pid, (long)&status, 0);
    ok = ok && w == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 128 + SIGTERM;

    print(ok ? "[job] OK\n" : "[job] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
