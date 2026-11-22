/* COW fork test: the parent and child share the data page read-only until
 * each writes. The child's private value must not leak back to the parent. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_FORK 9
#define SYS_WAITPID 40
#define SYS_EXIT 60

static volatile unsigned long cow_value = 0x1111;

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
    cow_value = 0x2222;
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        cow_value = 0x3333;
        int ok = cow_value == 0x3333;
        print(ok ? "[cow] child private OK\n" : "[cow] child FAIL\n");
        sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
        for (;;) {
        }
    }
    if (pid < 0) {
        print("[cow] fork FAIL\n");
        sc(SYS_EXIT, 1, 0, 0);
    }
    int status = -1;
    long reaped = sc(SYS_WAITPID, pid, (long)&status, 0);
    int ok = reaped == pid && status == 0 && cow_value == 0x2222;
    print("[cow] parent value=");
    print_hex(cow_value);
    print(ok ? " OK\n" : " FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
