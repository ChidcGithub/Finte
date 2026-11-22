/* General descriptor-table test: dynamic slots, dup/dup2/fcntl(F_DUPFD),
 * shared offsets, fork/fork+exec inheritance and close-on-exec. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_READ 3
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_LSEEK 6
#define SYS_FORK 9
#define SYS_EXEC 10
#define SYS_DUP 32
#define SYS_DUP2 33
#define SYS_WAITPID 40
#define SYS_EXIT 60
#define SYS_FCNTL 72

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define FD_CLOEXEC 1
#define FPROC_OPEN_READ 1

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

#define WIFEXITED(s) (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

static int open_read(void)
{
    return (int)sc(SYS_OPEN, (long)"hello.txt", FPROC_OPEN_READ, 9);
}

static int read_one(int fd, unsigned char expect)
{
    unsigned char c = 0;
    long n = sc(SYS_READ, fd, (long)&c, 1);
    return n == 1 && c == expect;
}

void _start(void)
{
    int ok = 1;
    unsigned char c;

    /* dup shares the offset; dup2 targets an exact fd; F_DUPFD obeys its
     * minimum and all descriptors keep reading from the same position. */
    int base = open_read();
    ok = ok && base >= 0 && read_one(base, 'H');
    int dupfd = (int)sc(SYS_DUP, base, 0, 0);
    ok = ok && dupfd >= 0 && dupfd != base;
    ok = ok && read_one(dupfd, 'e');
    ok = ok && read_one(base, 'l');
    ok = ok && sc(SYS_LSEEK, base, 0, 1) == 3;

    int exact = (int)sc(SYS_DUP2, base, 20, 0);
    ok = ok && exact == 20 && read_one(20, 'l');
    ok = ok && sc(SYS_CLOSE, base, 0, 0) == 0;
    ok = ok && read_one(20, 'o');

    int high = (int)sc(SYS_FCNTL, 20, F_DUPFD, 40);
    ok = ok && high >= 40 && read_one(high, ' ');
    ok = ok && sc(SYS_FCNTL, 20, F_GETFD, 0) == 0;
    ok = ok && sc(SYS_FCNTL, 20, F_SETFD, FD_CLOEXEC) == 0;
    ok = ok && sc(SYS_FCNTL, 20, F_GETFD, 0) == FD_CLOEXEC;
    ok = ok && sc(SYS_FCNTL, 20, F_SETFD, 0) == 0;
    ok = ok && (sc(SYS_FCNTL, 20, F_GETFL, 0) & FPROC_OPEN_READ) != 0;
    ok = ok && sc(SYS_CLOSE, 20, 0, 0) == 0;
    ok = ok && sc(SYS_CLOSE, high, 0, 0) == 0;
    ok = ok && sc(SYS_CLOSE, dupfd, 0, 0) == 0;

    /* Grow past the old 16-entry table, then reuse the lowest slot. */
    int fds[40];
    int count = 0;
    int grew = 1;
    for (int i = 0; i < 40; i++) {
        fds[i] = -1;
    }
    for (int i = 0; i < 40; i++) {
        int fd = open_read();
        if (fd < 0) {
            grew = 0;
            break;
        }
        fds[count++] = fd;
    }
    ok = ok && grew && count == 40 && fds[39] > 15;
    for (int i = 0; i < count; i++) {
        ok = ok && sc(SYS_CLOSE, fds[i], 0, 0) == 0;
    }
    int reuse = open_read();
    ok = ok && reuse == 3;
    ok = ok && sc(SYS_CLOSE, reuse, 0, 0) == 0;

    /* A fork child shares the open-file description and its offset. */
    int shared = open_read();
    ok = ok && read_one(shared, 'H') && read_one(shared, 'e') &&
         read_one(shared, 'l');
    long pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        unsigned char x = 0;
        long n = sc(SYS_READ, shared, (long)&x, 1);
        long n2 = sc(SYS_READ, shared, (long)&x, 1);
        sc(SYS_EXIT, (n == 1 && n2 == 1) ? 42 : 1, 0, 0);
        for (;;) {
        }
    }
    int status = -1;
    long w = sc(SYS_WAITPID, pid, (long)&status, 0);
    ok = ok && pid > 0 && w == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 42;
    ok = ok && read_one(shared, ' ');
    ok = ok && sc(SYS_CLOSE, shared, 0, 0) == 0;

    /* Closing in the child does not close the parent's descriptor. */
    int kept = open_read();
    ok = ok && read_one(kept, 'H');
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        sc(SYS_CLOSE, kept, 0, 0);
        sc(SYS_EXIT, 0, 0, 0);
        for (;;) {
        }
    }
    status = -1;
    w = sc(SYS_WAITPID, pid, (long)&status, 0);
    ok = ok && pid > 0 && w == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
    ok = ok && read_one(kept, 'e');
    ok = ok && sc(SYS_CLOSE, kept, 0, 0) == 0;

    /* Exec preserves descriptors and their shared offset. */
    int inherited = open_read();
    ok = ok && inherited >= 0;
    for (int i = 0; i < 5; i++) {
        ok = ok && sc(SYS_READ, inherited, (long)&c, 1) == 1;
    }
    ok = ok && c == 'o';
    ok = ok && sc(SYS_DUP2, inherited, 9, 0) == 9;
    static const char helper[] = "fdchild.elf";
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        sc(SYS_EXEC, (long)helper, sizeof(helper) - 1, 0);
        sc(SYS_EXIT, 0x7F, 0, 0);
        for (;;) {
        }
    }
    status = -1;
    w = sc(SYS_WAITPID, pid, (long)&status, 0);
    ok = ok && pid > 0 && w == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == ' ';
    ok = ok && read_one(inherited, 'f');
    ok = ok && sc(SYS_CLOSE, inherited, 0, 0) == 0;

    /* FD_CLOEXEC closes only the child's copy on exec. */
    int cloexec = open_read();
    ok = ok && cloexec >= 0 && sc(SYS_LSEEK, cloexec, 0, 0) == 0;
    ok = ok && sc(SYS_DUP2, cloexec, 9, 0) == 9;
    ok = ok && sc(SYS_FCNTL, 9, F_SETFD, FD_CLOEXEC) == 0;
    pid = sc(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        sc(SYS_EXEC, (long)helper, sizeof(helper) - 1, 0);
        sc(SYS_EXIT, 0x7F, 0, 0);
        for (;;) {
        }
    }
    status = -1;
    w = sc(SYS_WAITPID, pid, (long)&status, 0);
    ok = ok && pid > 0 && w == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
    ok = ok && read_one(9, 'H');
    ok = ok && sc(SYS_CLOSE, 9, 0, 0) == 0;
    ok = ok && sc(SYS_CLOSE, cloexec, 0, 0) == 0;

    print(ok ? "[fdtab] OK\n" : "[fdtab] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
