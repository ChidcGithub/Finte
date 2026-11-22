/* File-backed private mmap test: mapped bytes come from hello.txt, a write
 * changes only the private page, and the fd still reads the original file. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_READ 3
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_LSEEK 6
#define SYS_MUNMAP 8
#define SYS_MMAP_FILE 19
#define SYS_EXIT 60

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
    unsigned char buf[8];
    long fd = sc(SYS_OPEN, (long)"hello.txt", 1, 9);
    long va = fd >= 0 ? sc(SYS_MMAP_FILE, fd, 256, 3) : -1;
    int ok = va > 0;
    if (ok) {
        volatile unsigned char *m = (volatile unsigned char *)va;
        ok = m[0] == 'H' && m[1] == 'e' && m[2] == 'l' &&
             m[3] == 'l' && m[4] == 'o';
        m[0] = 'X';
        ok = ok && m[0] == 'X' && m[1] == 'e';
        sc(SYS_LSEEK, fd, 0, 0);
        long n = sc(SYS_READ, fd, (long)buf, 8);
        ok = ok && n == 8 && buf[0] == 'H' && buf[1] == 'e';
        sc(SYS_MUNMAP, va, 256, 0);
    }
    if (fd >= 0) {
        sc(SYS_CLOSE, fd, 0, 0);
    }
    print(ok ? "[filemap] OK\n" : "[filemap] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
