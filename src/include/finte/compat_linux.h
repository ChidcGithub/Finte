/* Linux x86_64 personality.
 *
 * This is a translator, not the core ABI. A flow marked FINTE_PERSONA_LINUX
 * sees Linux syscall numbers and POSIX-shaped results while every operation
 * underneath is validated as a Finte capability.
 *
 * v0.1 supports a deliberately small, honest subset (see docs/compat.md).
 */
#ifndef FINTE_COMPAT_LINUX_H
#define FINTE_COMPAT_LINUX_H

#include "finte/object.h"

#define FINTE_LINUX_AT_FDCWD (-100)

/* Syscall numbers used by the personality. */
enum {
    FINTE_NR_read = 0,
    FINTE_NR_write = 1,
    FINTE_NR_open = 2,
    FINTE_NR_close = 3,
    FINTE_NR_stat = 4,
    FINTE_NR_fstat = 5,
    FINTE_NR_lstat = 6,
    FINTE_NR_lseek = 8,
    FINTE_NR_mmap = 9,
    FINTE_NR_mprotect = 10,
    FINTE_NR_munmap = 11,
    FINTE_NR_brk = 12,
    FINTE_NR_rt_sigaction = 13,
    FINTE_NR_ioctl = 16,
    FINTE_NR_pread64 = 17,
    FINTE_NR_pwrite64 = 18,
    FINTE_NR_readv = 19,
    FINTE_NR_writev = 20,
    FINTE_NR_access = 21,
    FINTE_NR_sched_yield = 24,
    FINTE_NR_dup = 32,
    FINTE_NR_dup2 = 33,
    FINTE_NR_nanosleep = 35,
    FINTE_NR_getpid = 39,
    FINTE_NR_exit = 60,
    FINTE_NR_uname = 63,
    FINTE_NR_fcntl = 72,
    FINTE_NR_getcwd = 79,
    FINTE_NR_readlink = 89,
    FINTE_NR_gettimeofday = 96,
    FINTE_NR_getuid = 102,
    FINTE_NR_getgid = 104,
    FINTE_NR_getppid = 110,
    FINTE_NR_arch_prctl = 158,
    FINTE_NR_gettid = 186,
    FINTE_NR_sched_getaffinity = 204,
    FINTE_NR_clock_getres = 229,
    FINTE_NR_clock_gettime = 228,
    FINTE_NR_exit_group = 231,
    FINTE_NR_openat = 257,
    FINTE_NR_newfstatat = 262,
    FINTE_NR_set_tid_address = 218,
    FINTE_NR_prlimit64 = 302,
    FINTE_NR_getrandom = 318
};

enum {
    FINTE_F_OK = 0,
    FINTE_R_OK = 4,
    FINTE_W_OK = 2,
    FINTE_X_OK = 1
};

enum {
    FINTE_AT_FDCWD = -100,
    FINTE_O_RDONLY = 0,
    FINTE_O_WRONLY = 1,
    FINTE_O_RDWR = 2,
    FINTE_O_CREAT = 64,
    FINTE_O_TRUNC = 512,
    FINTE_O_APPEND = 1024
};

enum {
    FINTE_PROT_NONE = 0,
    FINTE_PROT_READ = 1,
    FINTE_PROT_WRITE = 2,
    FINTE_PROT_EXEC = 4
};

enum {
    FINTE_MAP_SHARED = 1,
    FINTE_MAP_PRIVATE = 2,
    FINTE_MAP_FIXED = 0x10,
    FINTE_MAP_ANONYMOUS = 0x20
};

enum {
    FINTE_CLOCK_REALTIME = 0,
    FINTE_CLOCK_MONOTONIC = 1
};

enum {
    FINTE_EPERM = 1,
    FINTE_ENOENT = 2,
    FINTE_EBADF = 9,
    FINTE_EAGAIN = 11,
    FINTE_ENOMEM = 12,
    FINTE_EACCES = 13,
    FINTE_EFAULT = 14,
    FINTE_EINVAL = 22,
    FINTE_ERANGE = 34,
    FINTE_ENOSYS = 38,
    FINTE_EROFS = 30,
    FINTE_ENOTDIR = 20,
    FINTE_EISDIR = 21
};

typedef struct flinux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
} flinux_timespec_t;

typedef struct flinux_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
} flinux_timeval_t;

typedef struct flinux_iovec {
    uint64_t base;
    uint64_t len;
} flinux_iovec_t;

typedef struct flinux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} flinux_utsname_t;

typedef struct flinux_stat {
    uint64_t dev;
    uint64_t ino;
    uint64_t nlink;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad;
    uint64_t rdev;
    uint64_t size;
    uint64_t blksize;
    uint64_t blocks;
    uint64_t atime;
    uint64_t atime_nsec;
    uint64_t mtime;
    uint64_t mtime_nsec;
    uint64_t ctime;
    uint64_t ctime_nsec;
    uint64_t pad2[3];
} flinux_stat_t;

/* Returns a Linux-style result: >= 0 on success, -errno on failure. */
int64_t finte_linux_call(fflow_t *f, uint64_t nr, const uint64_t args[6]);

/* Persona bootstrap: installs the hidden console/dir/arena caps and the fd
 * table. dir_cap and arena_cap are consumed by the persona. */
fret_t finte_linux_bootstrap(fflow_t *f, uint16_t console_cap,
                             uint16_t dir_cap, uint16_t arena_cap);

#endif /* FINTE_COMPAT_LINUX_H */
