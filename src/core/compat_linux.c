#include "finte/compat_linux.h"
#include "finte/verb.h"

#define LX_PTR(x) ((void *)(uintptr_t)(x))

static int errno_of(int32_t status)
{
    switch (status) {
    case FINTE_ERR_INVAL: return FINTE_EINVAL;
    case FINTE_ERR_NOENT: return FINTE_ENOENT;
    case FINTE_ERR_NOMEM:
    case FINTE_ERR_LIMIT: return FINTE_ENOMEM;
    case FINTE_ERR_PERM: return FINTE_EPERM;
    case FINTE_ERR_BUSY:
    case FINTE_ERR_AGAIN: return FINTE_EAGAIN;
    case FINTE_ERR_IO: return 5; /* EIO */
    case FINTE_ERR_DEADLOCK: return 35; /* EDEADLK */
    case FINTE_ERR_NOSYS: return FINTE_ENOSYS;
    case FINTE_ERR_FAULT: return FINTE_EFAULT;
    case FINTE_ERR_RANGE: return 34; /* ERANGE */
    case FINTE_ERR_EXIST: return 17; /* EEXIST */
    case FINTE_ERR_TYPE: return FINTE_EINVAL;
    case FINTE_ERR_STATE: return FINTE_EBADF;
    default: return FINTE_EINVAL;
    }
}

static int64_t lx_result(fret_t r)
{
    if (finte_failed(r)) {
        return -(int64_t)errno_of(r.status);
    }
    return (int64_t)r.value;
}

static int fd_cap(fflow_t *f, int fd)
{
    if (!f || fd < 0 || fd >= FINTE_MAX_CAPS) {
        return -1;
    }
    return f->linux.fd_cap[fd];
}

static ffile_t *file_cap(fflow_t *f, uint16_t cap)
{
    fcap_t c;
    if (finte_cap_check(f, cap, FINTE_R_READ, &c) < 0) {
        return NULL;
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_FILE) {
        return NULL;
    }
    return &finte_core.files[n->index];
}

static int find_free_fd(fflow_t *f)
{
    for (int i = 3; i < FINTE_MAX_CAPS; i++) {
        if (f->linux.fd_cap[i] < 0) {
            return i;
        }
    }
    return -1;
}

static int lookup_relative(fflow_t *f, const char *path, fhandle_t *out)
{
    if (!f || !path || !*path || !out) {
        return -FINTE_ENOENT;
    }
    while (*path == '/') {
        path++;
    }
    if (!*path) {
        return -FINTE_EISDIR;
    }
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            return -FINTE_ENOENT; /* v0.1 directories are flat */
        }
    }
    if (f->linux.dir_cap < 0) {
        return -FINTE_EPERM;
    }
    fcap_t dc;
    if (finte_cap_check(f, (uint16_t)f->linux.dir_cap, FINTE_R_READ, &dc) < 0) {
        return -FINTE_EPERM;
    }
    fnode_t *dn = finte_node_get(dc.handle);
    if (!dn || dn->type != FINTE_NODE_DIR) {
        return -FINTE_ENOTDIR;
    }
    fdir_t *dir = &finte_core.dirs[dn->index];
    for (uint16_t i = 0; i < dir->count; i++) {
        if (finte_strcmp(dir->ents[i].name, path) == 0) {
            *out = dir->ents[i].file;
            return 0;
        }
    }
    return -FINTE_ENOENT;
}

static int open_relative(fflow_t *f, const char *path)
{
    fhandle_t file = FINTE_HANDLE_NONE;
    int r = lookup_relative(f, path, &file);
    if (r < 0) {
        return r;
    }
    int cap = finte_cap_install(f, file, FINTE_R_READ);
    if (cap < 0) {
        return -FINTE_ENOMEM;
    }
    int fd = find_free_fd(f);
    if (fd < 0) {
        finte_cap_drop_index(f, (uint16_t)cap);
        return -FINTE_ENOMEM;
    }
    f->linux.fd_cap[fd] = cap;
    f->linux.fd_pos[fd] = 0;
    return fd;
}

static int64_t ensure_heap(fflow_t *f, size_t need)
{
    if (f->linux.brk_end >= f->linux.brk && f->linux.brk_end - f->linux.brk >= need) {
        return 0;
    }
    if (f->linux.arena_cap < 0) {
        return -FINTE_ENOMEM;
    }
    size_t chunk = need > (1024u * 1024u) ? need : (1024u * 1024u);
    chunk = (chunk + 4095u) & ~(size_t)4095u;
    uint64_t off = 0;
    fret_t r = fx_arena_alloc(f, (uint16_t)f->linux.arena_cap, chunk, 16, &off);
    if (finte_failed(r)) {
        return -(int64_t)errno_of(r.status);
    }
    fcap_t c;
    if (finte_cap_check(f, (uint16_t)f->linux.arena_cap, FINTE_R_WRITE, &c) < 0) {
        return -FINTE_EPERM;
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_ARENA) {
        return -FINTE_EINVAL;
    }
    farena_t *a = &finte_core.arenas[n->index];
    uint8_t *base = a->base + off;
    f->linux.heap_base = (uint64_t)(uintptr_t)base;
    f->linux.brk = f->linux.heap_base;
    f->linux.brk_end = f->linux.heap_base + chunk;
    return 0;
}

static void fill_stat(ffile_t *file, flinux_stat_t *st)
{
    finte_memset(st, 0, sizeof(*st));
    st->mode = 0x8000u | 0444u; /* S_IFREG | r--r--r-- */
    st->nlink = 1;
    st->size = file ? file->size : 0;
    st->blksize = 4096;
    st->blocks = file ? (file->size + 511u) / 512u : 0;
}

static int stat_relative(fflow_t *f, const char *path, flinux_stat_t *st)
{
    fhandle_t h = FINTE_HANDLE_NONE;
    int r = lookup_relative(f, path, &h);
    if (r < 0) {
        return r;
    }
    fnode_t *n = finte_node_get(h);
    if (!n || n->type != FINTE_NODE_FILE) {
        return -FINTE_ENOENT;
    }
    fill_stat(&finte_core.files[n->index], st);
    return 0;
}

fret_t finte_linux_bootstrap(fflow_t *f, uint16_t console_cap,
                             uint16_t dir_cap, uint16_t arena_cap)
{
    if (!f) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    f->persona = FINTE_PERSONA_LINUX;
    for (int i = 0; i < FINTE_MAX_CAPS; i++) {
        f->linux.fd_cap[i] = -1;
        f->linux.fd_pos[i] = 0;
    }
    f->linux.console_cap = console_cap;
    f->linux.dir_cap = dir_cap;
    f->linux.arena_cap = arena_cap;
    for (int fd = 0; fd < 3; fd++) {
        f->linux.fd_cap[fd] = console_cap;
    }
    fcap_t c;
    if (finte_cap_check(f, console_cap, FINTE_R_WRITE, &c) < 0) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    if (finte_cap_check(f, dir_cap, FINTE_R_READ, &c) < 0) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    if (finte_cap_check(f, arena_cap, FINTE_R_WRITE, &c) < 0) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    return finte_ret_ok(0);
}

int64_t finte_linux_call(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    if (!f) {
        return -FINTE_EINVAL;
    }

    switch (nr) {
    case FINTE_NR_read: {
        int fd = (int)args[0];
        void *buf = LX_PTR(args[1]);
        size_t len = (size_t)args[2];
        int cap = fd_cap(f, fd);
        if (cap < 0 || !buf) {
            return -FINTE_EBADF;
        }
        fcap_info_t info;
        if (finte_failed(fx_cap_info(f, (uint16_t)cap, &info))) {
            return -FINTE_EBADF;
        }
        if (info.type == FINTE_NODE_CONSOLE) {
            return 0;
        }
        ffile_t *file = file_cap(f, (uint16_t)cap);
        if (!file) {
            return -FINTE_EBADF;
        }
        uint64_t pos = f->linux.fd_pos[fd];
        if (pos >= file->size) {
            return 0;
        }
        if (len > file->size - pos) {
            len = file->size - (size_t)pos;
        }
        fret_t r = fx_file_read(f, (uint16_t)cap, pos, buf, len);
        if (finte_failed(r)) {
            return lx_result(r);
        }
        f->linux.fd_pos[fd] = pos + len;
        return (int64_t)len;
    }

    case FINTE_NR_write: {
        int fd = (int)args[0];
        const void *buf = LX_PTR(args[1]);
        size_t len = (size_t)args[2];
        int cap = fd_cap(f, fd);
        if (cap < 0 || (!buf && len)) {
            return -FINTE_EBADF;
        }
        fcap_info_t info;
        if (finte_failed(fx_cap_info(f, (uint16_t)cap, &info))) {
            return -FINTE_EBADF;
        }
        if (info.type != FINTE_NODE_CONSOLE) {
            return -FINTE_EROFS;
        }
        fret_t r = fx_console_write(f, (uint16_t)cap, buf, len);
        return finte_failed(r) ? lx_result(r) : (int64_t)len;
    }

    case FINTE_NR_writev: {
        const flinux_iovec_t *iov = (const flinux_iovec_t *)LX_PTR(args[1]);
        int count = (int)args[2];
        if (!iov || count < 0) {
            return -FINTE_EINVAL;
        }
        int64_t total = 0;
        for (int i = 0; i < count; i++) {
            int64_t n = finte_linux_call(f, FINTE_NR_write,
                                         (uint64_t[]){args[0], iov[i].base,
                                                      iov[i].len, 0, 0, 0});
            if (n < 0) {
                return total ? total : n;
            }
            total += n;
        }
        return total;
    }

    case FINTE_NR_readv: {
        const flinux_iovec_t *iov = (const flinux_iovec_t *)LX_PTR(args[1]);
        int count = (int)args[2];
        if (!iov || count < 0) {
            return -FINTE_EINVAL;
        }
        int64_t total = 0;
        for (int i = 0; i < count; i++) {
            int64_t n = finte_linux_call(f, FINTE_NR_read,
                                         (uint64_t[]){args[0], iov[i].base,
                                                      iov[i].len, 0, 0, 0});
            if (n < 0) {
                return total ? total : n;
            }
            total += n;
            if ((uint64_t)n < iov[i].len) {
                break;
            }
        }
        return total;
    }

    case FINTE_NR_open: {
        const char *path = (const char *)LX_PTR(args[0]);
        int flags = (int)args[1];
        if (!path) {
            return -FINTE_EFAULT;
        }
        if ((flags & 3) != FINTE_O_RDONLY ||
            (flags & (FINTE_O_CREAT | FINTE_O_TRUNC | FINTE_O_APPEND))) {
            return -FINTE_EROFS;
        }
        return open_relative(f, path);
    }

    case FINTE_NR_openat: {
        const char *path = (const char *)LX_PTR(args[1]);
        int flags = (int)args[2];
        if (!path) {
            return -FINTE_EFAULT;
        }
        if ((flags & 3) != FINTE_O_RDONLY ||
            (flags & (FINTE_O_CREAT | FINTE_O_TRUNC | FINTE_O_APPEND))) {
            return -FINTE_EROFS;
        }
        return open_relative(f, path);
    }

    case FINTE_NR_stat:
    case FINTE_NR_lstat: {
        const char *path = (const char *)LX_PTR(args[0]);
        flinux_stat_t *st = (flinux_stat_t *)LX_PTR(args[1]);
        if (!path || !st) {
            return -FINTE_EFAULT;
        }
        return stat_relative(f, path, st);
    }

    case FINTE_NR_access: {
        const char *path = (const char *)LX_PTR(args[0]);
        int mode = (int)args[1];
        fhandle_t h = FINTE_HANDLE_NONE;
        if (!path) {
            return -FINTE_EFAULT;
        }
        int r = lookup_relative(f, path, &h);
        (void)h;
        if (r < 0) {
            return r;
        }
        if (mode & FINTE_W_OK) {
            return -FINTE_EROFS;
        }
        if (mode & FINTE_X_OK) {
            return -FINTE_EACCES;
        }
        return 0;
    }

    case FINTE_NR_dup: {
        int oldfd = (int)args[0];
        int cap = fd_cap(f, oldfd);
        fcap_t c;
        if (cap < 0 ||
            finte_cap_check(f, (uint16_t)cap, 0, &c) < 0) {
            return -FINTE_EBADF;
        }
        int newcap = finte_cap_install(f, c.handle, c.rights);
        if (newcap < 0) {
            return -FINTE_ENOMEM;
        }
        int fd = find_free_fd(f);
        if (fd < 0) {
            finte_cap_drop_index(f, (uint16_t)newcap);
            return -FINTE_ENOMEM;
        }
        f->linux.fd_cap[fd] = newcap;
        f->linux.fd_pos[fd] = f->linux.fd_pos[oldfd];
        return fd;
    }

    case FINTE_NR_dup2: {
        int oldfd = (int)args[0];
        int newfd = (int)args[1];
        if (newfd < 0 || newfd >= FINTE_MAX_CAPS) {
            return -FINTE_EBADF;
        }
        if (oldfd == newfd) {
            return fd_cap(f, oldfd) >= 0 ? newfd : -FINTE_EBADF;
        }
        int cap = fd_cap(f, oldfd);
        fcap_t c;
        if (cap < 0 ||
            finte_cap_check(f, (uint16_t)cap, 0, &c) < 0) {
            return -FINTE_EBADF;
        }
        int existing = f->linux.fd_cap[newfd];
        if (existing >= 0 && existing != f->linux.console_cap) {
            finte_cap_drop_index(f, (uint16_t)existing);
        }
        int newcap = finte_cap_install(f, c.handle, c.rights);
        if (newcap < 0) {
            return -FINTE_ENOMEM;
        }
        f->linux.fd_cap[newfd] = newcap;
        f->linux.fd_pos[newfd] = f->linux.fd_pos[oldfd];
        return newfd;
    }

    case FINTE_NR_close: {
        int fd = (int)args[0];
        if (fd < 0 || fd >= FINTE_MAX_CAPS) {
            return -FINTE_EBADF;
        }
        int cap = f->linux.fd_cap[fd];
        if (cap < 0) {
            return -FINTE_EBADF;
        }
        if (cap != f->linux.console_cap) {
            finte_cap_drop_index(f, (uint16_t)cap);
        }
        f->linux.fd_cap[fd] = -1;
        return 0;
    }

    case FINTE_NR_lseek: {
        int fd = (int)args[0];
        int64_t off = (int64_t)args[1];
        int whence = (int)args[2];
        int cap = fd_cap(f, fd);
        ffile_t *file = cap >= 0 ? file_cap(f, (uint16_t)cap) : NULL;
        if (!file) {
            return -FINTE_EBADF;
        }
        int64_t base = whence == 0 ? 0 : (whence == 1 ? (int64_t)f->linux.fd_pos[fd]
                                                      : (int64_t)file->size);
        if (whence < 0 || whence > 2 || base + off < 0) {
            return -FINTE_EINVAL;
        }
        f->linux.fd_pos[fd] = (uint64_t)(base + off);
        return base + off;
    }

    case FINTE_NR_pread64: {
        int fd = (int)args[0];
        int cap = fd_cap(f, fd);
        ffile_t *file = cap >= 0 ? file_cap(f, (uint16_t)cap) : NULL;
        if (!file) {
            return -FINTE_EBADF;
        }
        uint64_t off = args[3];
        if (off >= file->size) {
            return 0;
        }
        return lx_result(fx_file_read(f, (uint16_t)cap, off, LX_PTR(args[1]),
                                      (size_t)args[2]));
    }

    case FINTE_NR_pwrite64:
        return -FINTE_EROFS;

    case FINTE_NR_mmap: {
        uint64_t len = args[1];
        int prot = (int)args[2];
        int flags = (int)args[3];
        int fd = (int)args[4];
        uint64_t off = args[5];
        if (len == 0) {
            return -FINTE_EINVAL;
        }
        if (flags & FINTE_MAP_ANONYMOUS) {
            if (prot == FINTE_PROT_NONE) {
                prot = FINTE_PROT_READ | FINTE_PROT_WRITE;
            }
            size_t rounded = (len + 4095u) & ~(size_t)4095u;
            int64_t grown = ensure_heap(f, rounded);
            if (grown < 0) {
                return grown;
            }
            uint8_t *at = (uint8_t *)(uintptr_t)f->linux.brk;
            f->linux.brk += rounded;
            finte_memset(at, 0, rounded);
            return (int64_t)(uintptr_t)at;
        }
        int cap = fd_cap(f, fd);
        ffile_t *file = cap >= 0 ? file_cap(f, (uint16_t)cap) : NULL;
        if (!file) {
            return -FINTE_EBADF;
        }
        if (off >= file->size) {
            return -FINTE_EINVAL;
        }
        if (len > file->size - off) {
            len = file->size - off;
        }
        return (int64_t)(uintptr_t)(file->data + off);
    }

    case FINTE_NR_mprotect:
    case FINTE_NR_munmap:
        return 0;

    case FINTE_NR_brk: {
        uint64_t target = args[0];
        if (target == 0) {
            return (int64_t)f->linux.brk;
        }
        if (target < f->linux.heap_base || target > f->linux.brk_end) {
            return (int64_t)f->linux.brk;
        }
        f->linux.brk = target;
        return (int64_t)f->linux.brk;
    }

    case FINTE_NR_rt_sigaction:
    case FINTE_NR_fcntl:
    case FINTE_NR_arch_prctl:
        return 0;

    case FINTE_NR_ioctl:
        return -FINTE_ENOSYS;

    case FINTE_NR_nanosleep: {
        const flinux_timespec_t *req =
            (const flinux_timespec_t *)LX_PTR(args[0]);
        flinux_timespec_t *rem = (flinux_timespec_t *)LX_PTR(args[1]);
        if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000ll) {
            return -FINTE_EINVAL;
        }
        uint64_t ns = (uint64_t)req->tv_sec * 1000000000ull +
                      (uint64_t)req->tv_nsec;
        finte_flow_sleep(f, ns);
        if (rem) {
            rem->tv_sec = 0;
            rem->tv_nsec = 0;
        }
        return 0;
    }

    case FINTE_NR_getpid:
    case FINTE_NR_gettid:
        return (int64_t)f->slot + 1;

    case FINTE_NR_getppid:
        return 0;

    case FINTE_NR_getuid:
    case FINTE_NR_getgid:
        return 0;

    case FINTE_NR_getcwd: {
        char *buf = (char *)LX_PTR(args[0]);
        uint64_t size = args[1];
        if (!buf) {
            return -FINTE_EFAULT;
        }
        if (size < 2) {
            return -FINTE_ERANGE;
        }
        buf[0] = '/';
        buf[1] = '\0';
        return 2;
    }

    case FINTE_NR_readlink:
        return -FINTE_EINVAL;

    case FINTE_NR_sched_yield:
        if (finte_flow_current() == f && hal_cpu_self()->sched_ctx) {
            fx_yield(f);
        }
        return 0;

    case FINTE_NR_sched_getaffinity: {
        uint64_t len = args[1];
        void *mask = LX_PTR(args[2]);
        if (!mask) {
            return -FINTE_EFAULT;
        }
        if (len < 8) {
            return -FINTE_EINVAL;
        }
        if (len > 1024) {
            len = 1024;
        }
        finte_memset(mask, 0, (size_t)len);
        *(uint64_t *)mask = 1; /* one CPU in this version */
        return 8;
    }

    case FINTE_NR_prlimit64:
        return 0;

    case FINTE_NR_set_tid_address:
        return (int64_t)f->slot + 1;

    case FINTE_NR_uname: {
        flinux_utsname_t *u = (flinux_utsname_t *)LX_PTR(args[0]);
        if (!u) {
            return -FINTE_EFAULT;
        }
        finte_memset(u, 0, sizeof(*u));
        finte_strlcpy(u->sysname, "Linux", sizeof(u->sysname));
        finte_strlcpy(u->nodename, "finte", sizeof(u->nodename));
        finte_strlcpy(u->release, "0.0.0-finte", sizeof(u->release));
        finte_strlcpy(u->version, "#1 Finte", sizeof(u->version));
        finte_strlcpy(u->machine, "x86_64", sizeof(u->machine));
        finte_strlcpy(u->domainname, "(none)", sizeof(u->domainname));
        return 0;
    }

    case FINTE_NR_clock_gettime: {
        int clk = (int)args[0];
        flinux_timespec_t *ts = (flinux_timespec_t *)LX_PTR(args[1]);
        if (!ts || (clk != FINTE_CLOCK_REALTIME && clk != FINTE_CLOCK_MONOTONIC)) {
            return -FINTE_EINVAL;
        }
        uint64_t ns = hal_now_ns();
        ts->tv_sec = (int64_t)(ns / 1000000000ull);
        ts->tv_nsec = (int64_t)(ns % 1000000000ull);
        return 0;
    }

    case FINTE_NR_clock_getres: {
        int clk = (int)args[0];
        flinux_timespec_t *ts = (flinux_timespec_t *)LX_PTR(args[1]);
        if (!ts || (clk != FINTE_CLOCK_REALTIME && clk != FINTE_CLOCK_MONOTONIC)) {
            return -FINTE_EINVAL;
        }
        ts->tv_sec = 0;
        ts->tv_nsec = 1;
        return 0;
    }

    case FINTE_NR_gettimeofday: {
        flinux_timeval_t *tv = (flinux_timeval_t *)LX_PTR(args[0]);
        if (!tv) {
            return -FINTE_EFAULT;
        }
        uint64_t ns = hal_now_ns();
        tv->tv_sec = (int64_t)(ns / 1000000000ull);
        tv->tv_usec = (int64_t)((ns % 1000000000ull) / 1000ull);
        return 0;
    }

    case FINTE_NR_getrandom: {
        void *buf = LX_PTR(args[0]);
        size_t len = (size_t)args[1];
        if (!buf && len) {
            return -FINTE_EFAULT;
        }
        for (size_t i = 0; i < len; i++) {
            finte_core.rng ^= finte_core.rng << 13;
            finte_core.rng ^= finte_core.rng >> 17;
            finte_core.rng ^= finte_core.rng << 5;
            ((uint8_t *)buf)[i] = (uint8_t)(finte_core.rng >> 24);
        }
        return (int64_t)len;
    }

    case FINTE_NR_fstat:
    case FINTE_NR_newfstatat: {
        int fd = (int)args[0];
        flinux_stat_t *st = (flinux_stat_t *)LX_PTR(args[2]);
        if (nr == FINTE_NR_fstat) {
            st = (flinux_stat_t *)LX_PTR(args[1]);
        }
        if (!st) {
            return -FINTE_EFAULT;
        }
        if (nr == FINTE_NR_newfstatat && args[1]) {
            const char *path = (const char *)LX_PTR(args[1]);
            return stat_relative(f, path, st);
        }
        int cap = fd_cap(f, fd);
        ffile_t *file = cap >= 0 ? file_cap(f, (uint16_t)cap) : NULL;
        if (!file) {
            return -FINTE_EBADF;
        }
        fill_stat(file, st);
        return 0;
    }

    case FINTE_NR_exit:
    case FINTE_NR_exit_group:
        finte_flow_exit(f, (int)args[0]);
        return 0;

    default:
        return -FINTE_ENOSYS;
    }
}
