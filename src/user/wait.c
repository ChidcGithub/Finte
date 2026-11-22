/* waitpid() test: fork a child that exits with 7, block until it is reaped,
 * verify the status, then check that a second wait fails. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_FORK 9
#define SYS_EXIT 60
#define SYS_WAITPID 40

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

static void print_hex(unsigned long v)
{
    static const char digits[] = "0123456789abcdef";
    char out[18];
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        out[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    }
    sc(SYS_WRITE, (long)out, sizeof(out), 0);
}

void _start(void)
{
    int status = -1;
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        sc(SYS_EXIT, 7, 0, 0);
        for (;;) {
        }
    }
    if (pid < 0) {
        print("[wait] fork FAIL\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    long reaped = sc(SYS_WAITPID, pid, (long)&status, 0);
    long again = sc(SYS_WAITPID, pid, (long)&status, 0);
    long missing = sc(SYS_WAITPID, 9999, (long)&status, 0);
    int ok = reaped == pid && status == 7 && again < 0 && missing < 0;
    print("[wait] pid=");
    print_hex((unsigned long)reaped);
    print(" status=");
    print_hex((unsigned long)status);
    print(ok ? " OK\n" : " FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
