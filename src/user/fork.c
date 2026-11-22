/* fork() test: child returns 0, parent gets the child pid, and an open fd is
 * inherited across the address-space clone. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_READ 3
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_GETPID 39
#define SYS_EXIT 60
#define SYS_FORK 9

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
    char buf[8];
    long fd = sc(SYS_OPEN, (long)"hello.txt", 1, 9);
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        long n = fd >= 0 ? sc(SYS_READ, fd, (long)buf, 8) : -1;
        print("[fork] child pid=");
        print_hex((unsigned long)sc(SYS_GETPID, 0, 0, 0));
        print(" fdread=");
        print_hex((unsigned long)n);
        print("\n");
        sc(SYS_EXIT, n > 0 ? 0 : 1, 0, 0);
    } else if (pid > 0) {
        if (fd >= 0) {
            sc(SYS_CLOSE, fd, 0, 0);
        }
        print("[fork] parent child=");
        print_hex((unsigned long)pid);
        print("\n");
        sc(SYS_EXIT, 0, 0, 0);
    } else {
        print("[fork] FAIL\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    for (;;) {
    }
}
