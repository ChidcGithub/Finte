/* Shared ABI smoke test: initrd fd, read/lseek/close, anonymous mmap. */
#include "syscall.h"

#define SYS_WRITE_FD 14
#define SYS_READ 3
#define SYS_OPEN 4
#define SYS_CLOSE 5
#define SYS_LSEEK 6
#define SYS_MMAP 7
#define SYS_MUNMAP 8
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
    sc(SYS_WRITE_FD, 1, (long)s, n);
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
    sc(SYS_WRITE_FD, 1, (long)out, sizeof(out));
}

static void flag(const char *name, int ok)
{
    print(" ");
    print(name);
    print("=");
    print(ok ? "OK" : "FAIL");
}

void _start(void)
{
    char buf[64];
    long fd = sc(SYS_OPEN, (long)"hello.txt", 1, 9);
    long n = fd >= 0 ? sc(SYS_READ, fd, (long)buf, sizeof(buf) - 1) : -1;
    long seek = fd >= 0 ? sc(SYS_LSEEK, fd, 0, 0) : -1;
    long close_rc = fd >= 0 ? sc(SYS_CLOSE, fd, 0, 0) : -1;

    long va = sc(SYS_MMAP, 0, 4096, 3);
    int mmap_ok = va > 0;
    if (mmap_ok) {
        volatile unsigned char *p = (volatile unsigned char *)va;
        p[0] = 0x5A;
        p[4095] = 0xA5;
        mmap_ok = p[0] == 0x5A && p[4095] == 0xA5;
    }
    long unmap_rc = va > 0 ? sc(SYS_MUNMAP, va, 4096, 0) : -1;

    print("[abi]");
    flag("open", fd >= 0);
    flag("read", n > 0);
    flag("lseek", seek == 0);
    flag("mmap", mmap_ok);
    flag("munmap", unmap_rc == 0);
    flag("close", close_rc == 0);
    print(" data=");
    print_hex(n > 0 ? (unsigned long)(unsigned char)buf[0] : 0);
    print("\n");
    sc(SYS_EXIT, (fd >= 0 && n > 0 && seek == 0 && mmap_ok && unmap_rc == 0 &&
                  close_rc == 0) ? 0 : 1, 0, 0);
    for (;;) {
    }
}
