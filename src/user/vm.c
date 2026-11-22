/* Demand paging and swap test: lazy anonymous/file mappings, mincore
 * residency, MADV_PAGEOUT to a disk swap file and fault-in with intact data. */
#include "syscall.h"

#define SYS_WRITE 1
#define SYS_READ 3
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_LSEEK 6
#define SYS_MMAP 7
#define SYS_MUNMAP 8
#define SYS_FORK 9
#define SYS_MMAP_FILE 19
#define SYS_MINCORE 27
#define SYS_MADVISE 28
#define SYS_WAITPID 40
#define SYS_SWAPON 167
#define SYS_SWAPOFF 168
#define SYS_EXIT 60

#define FPROC_OPEN_READ 1
#define MADV_PAGEOUT 21
#define PAGE 4096
#define PAGES 16

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

static int all_value(const unsigned char *vec, int n, unsigned char v)
{
    for (int i = 0; i < n; i++) {
        if (vec[i] != v) {
            return 0;
        }
    }
    return 1;
}

static int swapon_any(void)
{
    static const char *dev = "hd1";
    int n = 0;
    while (dev[n]) {
        n++;
    }
    return sc(SYS_SWAPON, (long)dev, n, 0) == 0;
}

void _start(void)
{
    int ok = 1;
    unsigned char vec[PAGES];

    ok = ok && swapon_any();

    long va = sc(SYS_MMAP, 0, PAGES * PAGE, 3);
    ok = ok && va > 0;
    if (va > 0) {
        ok = ok && sc(SYS_MINCORE, va, PAGES * PAGE, (long)vec) == 0;
        ok = ok && all_value(vec, PAGES, 0);

        volatile unsigned char *m = (volatile unsigned char *)va;
        for (int i = 0; i < PAGES; i++) {
            m[i * PAGE] = (unsigned char)(0x40 + i);
            m[i * PAGE + 17] = (unsigned char)(0x80 + i);
            m[i * PAGE + PAGE - 1] = (unsigned char)(0xC0 + i);
        }
        ok = ok && sc(SYS_MINCORE, va, PAGES * PAGE, (long)vec) == 0;
        ok = ok && all_value(vec, PAGES, 1);

        ok = ok && sc(SYS_MADVISE, va, PAGES * PAGE, MADV_PAGEOUT) == 0;
        ok = ok && sc(SYS_MINCORE, va, PAGES * PAGE, (long)vec) == 0;
        ok = ok && all_value(vec, PAGES, 0);

        /* A fork child shares the swapped description and faults the same
         * slots; both parent and child must restore the original bytes. */
        long pid = sc(SYS_FORK, 0, 0, 0);
        if (pid == 0) {
            int child_ok = 1;
            for (int i = 0; i < PAGES; i++) {
                child_ok = child_ok &&
                           m[i * PAGE] == (unsigned char)(0x40 + i) &&
                           m[i * PAGE + 17] == (unsigned char)(0x80 + i) &&
                           m[i * PAGE + PAGE - 1] ==
                               (unsigned char)(0xC0 + i);
            }
            sc(SYS_EXIT, child_ok ? 0 : 1, 0, 0);
            for (;;) {
            }
        }
        int status = -1;
        long w = sc(SYS_WAITPID, pid, (long)&status, 0);
        ok = ok && pid > 0 && w == pid && status == 0;

        for (int i = 0; i < PAGES; i++) {
            ok = ok && m[i * PAGE] == (unsigned char)(0x40 + i);
            ok = ok && m[i * PAGE + 17] == (unsigned char)(0x80 + i);
            ok = ok && m[i * PAGE + PAGE - 1] == (unsigned char)(0xC0 + i);
        }
        ok = ok && sc(SYS_MINCORE, va, PAGES * PAGE, (long)vec) == 0;
        ok = ok && all_value(vec, PAGES, 1);
        ok = ok && sc(SYS_MUNMAP, va, PAGES * PAGE, 0) == 0;
    }

    /* File-backed mappings are demand-loaded and private writes survive
     * pageout while the underlying file remains unchanged. */
    int fd = (int)sc(SYS_OPEN, (long)"hello.txt", FPROC_OPEN_READ, 9);
    ok = ok && fd >= 0;
    long fva = fd >= 0 ? sc(SYS_MMAP_FILE, fd, PAGE, 3) : -1;
    ok = ok && fva > 0;
    if (fva > 0) {
        volatile unsigned char *fm = (volatile unsigned char *)fva;
        ok = ok && sc(SYS_MINCORE, fva, PAGE, (long)vec) == 0;
        ok = ok && vec[0] == 0;
        ok = ok && fm[0] == 'H' && fm[1] == 'e' && fm[2] == 'l' &&
             fm[3] == 'l' && fm[4] == 'o';
        fm[0] = 'X';
        ok = ok && sc(SYS_MADVISE, fva, PAGE, MADV_PAGEOUT) == 0;
        ok = ok && sc(SYS_MINCORE, fva, PAGE, (long)vec) == 0;
        ok = ok && vec[0] == 0;
        ok = ok && fm[0] == 'X' && fm[1] == 'e';
        ok = ok && sc(SYS_LSEEK, fd, 0, 0) == 0;
        unsigned char c = 0;
        ok = ok && sc(SYS_READ, fd, (long)&c, 1) == 1 && c == 'H';
        ok = ok && sc(SYS_MUNMAP, fva, PAGE, 0) == 0;
    }
    if (fd >= 0) {
        sc(SYS_CLOSE, fd, 0, 0);
    }

    ok = ok && sc(SYS_SWAPOFF, 0, 0, 0) == 0;
    print(ok ? "[vm] OK\n" : "[vm] FAIL\n");
    sc(SYS_EXIT, ok ? 0 : 1, 0, 0);
    for (;;) {
    }
}
