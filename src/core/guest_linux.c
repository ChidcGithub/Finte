#include "finte/finte.h"
#include "finte/guest.h"

static fguest_result_t g_guest;

static int64_t lx(fflow_t *f, uint64_t nr, uint64_t a0, uint64_t a1,
                  uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    uint64_t args[6] = { a0, a1, a2, a3, a4, a5 };
    return finte_linux_call(f, nr, args);
}

static void lwrite(fflow_t *f, const char *s)
{
    lx(f, FINTE_NR_write, 1, (uint64_t)(uintptr_t)s, finte_strlen(s), 0, 0, 0);
}

static void lprintf(fflow_t *f, const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        lx(f, FINTE_NR_write, 1, (uint64_t)(uintptr_t)buf, (uint64_t)n, 0, 0, 0);
    }
}

static void guest_main(fflow_t *f)
{
    finte_memset(&g_guest, 0, sizeof(g_guest));
    g_guest.ok = 0;
    g_guest.fd = -1;

    lwrite(f, "linux persona: syscall numbers in, capability checks underneath\n");

    int64_t fd = lx(f, FINTE_NR_openat, (uint64_t)(int64_t)FINTE_AT_FDCWD,
                    (uint64_t)(uintptr_t)"/hello.txt", FINTE_O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        lprintf(f, "openat failed: %lld\n", fd);
        lx(f, FINTE_NR_exit, 1, 0, 0, 0, 0, 0);
    }
    g_guest.fd = fd;

    char buf[128];
    finte_memset(buf, 0, sizeof(buf));
    int64_t n = lx(f, FINTE_NR_read, (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                   sizeof(buf) - 1, 0, 0, 0);
    if (n >= 0) {
        buf[n] = '\0';
        g_guest.bytes_read = n;
        lprintf(f, "openat+read -> fd=%lld n=%lld: %s", (long long)fd,
                (long long)n, buf);
    } else {
        lprintf(f, "read failed: %lld\n", (long long)n);
    }

    flinux_utsname_t u;
    if (lx(f, FINTE_NR_uname, (uint64_t)(uintptr_t)&u, 0, 0, 0, 0, 0) == 0) {
        finte_strlcpy(g_guest.sysname, u.sysname, sizeof(g_guest.sysname));
        finte_strlcpy(g_guest.machine, u.machine, sizeof(g_guest.machine));
        lprintf(f, "uname -> %s %s %s\n", u.sysname, u.release, u.machine);
    }

    flinux_timespec_t ts;
    if (lx(f, FINTE_NR_clock_gettime, FINTE_CLOCK_MONOTONIC,
           (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0) == 0) {
        g_guest.monotonic_ns = (uint64_t)ts.tv_sec * 1000000000ull +
                               (uint64_t)ts.tv_nsec;
        lprintf(f, "clock_gettime(MONOTONIC) -> %lld.%09lld\n",
                (long long)ts.tv_sec, (long long)ts.tv_nsec);
    }

    int64_t pid = lx(f, FINTE_NR_getpid, 0, 0, 0, 0, 0, 0);
    g_guest.pid = pid;
    lprintf(f, "getpid -> %lld\n", (long long)pid);

    g_guest.brk_before = (uint64_t)lx(f, FINTE_NR_brk, 0, 0, 0, 0, 0, 0);
    char *mem = (char *)(uintptr_t)lx(f, FINTE_NR_mmap, 0, 4096,
                                      FINTE_PROT_READ | FINTE_PROT_WRITE,
                                      FINTE_MAP_PRIVATE | FINTE_MAP_ANONYMOUS,
                                      (uint64_t)-1, 0);
    if ((intptr_t)mem > 0) {
        const char *text = "mmap: zero-copy pointer from a Finte arena\n";
        finte_memcpy(mem, text, finte_strlen(text));
        lx(f, FINTE_NR_write, 1, (uint64_t)(uintptr_t)mem,
           finte_strlen(text), 0, 0, 0);
    } else {
        lprintf(f, "mmap failed: %lld\n", (long long)(intptr_t)mem);
    }
    g_guest.brk_after = (uint64_t)lx(f, FINTE_NR_brk, 0, 0, 0, 0, 0, 0);
    lprintf(f, "brk -> 0x%llx (before) / 0x%llx (after mmap)\n",
            (unsigned long long)g_guest.brk_before,
            (unsigned long long)g_guest.brk_after);

    uint8_t rnd[8];
    int64_t rn = lx(f, FINTE_NR_getrandom, (uint64_t)(uintptr_t)rnd,
                    sizeof(rnd), 0, 0, 0, 0);
    if (rn == (int64_t)sizeof(rnd)) {
        lprintf(f, "getrandom -> %02x%02x%02x%02x%02x%02x%02x%02x\n",
                rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
    }

    lx(f, FINTE_NR_close, (uint64_t)fd, 0, 0, 0, 0, 0);
    g_guest.ok = 1;
    lwrite(f, "linux persona: exit(0)\n");
    lx(f, FINTE_NR_exit, 0, 0, 0, 0, 0, 0);
}

const fprogram_t finte_linux_guest_program = {
    "linux-guest", guest_main, 8192
};

const fguest_result_t *finte_guest_result(void)
{
    return &g_guest;
}
