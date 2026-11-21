#include "finte/compat_linux.h"
#include "finte/verb.h"

#define LX_PTR(x) ((void *)(uintptr_t)(x))

static int errno_of(int32_t status)
{
    /* Indexed by -status. */
    static const int map[] = {
        FINTE_EINVAL,
        FINTE_EINVAL,
        FINTE_ENOENT,
        FINTE_ENOMEM,
        FINTE_EPERM,
        FINTE_EAGAIN,
        FINTE_EAGAIN,
        FINTE_ENOMEM,
        5,  /* EIO */
        35, /* EDEADLK */
        FINTE_ENOSYS,
        FINTE_EFAULT,
        34, /* ERANGE */
        17, /* EEXIST */
        FINTE_EINVAL,
        FINTE_EBADF,
        FINTE_EINVAL
    };
    if (status > 0 || status <= -(int32_t)FINTE_ARRAY_SIZE(map)) {
        return FINTE_EINVAL;
    }
    return map[-status];
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

typedef int64_t (*lx_handler_t)(fflow_t *f, uint64_t nr,
                                const uint64_t args[6]);

typedef struct lx_entry {
    uint64_t nr;
    lx_handler_t fn;
} lx_entry_t;

static int64_t lx_zero(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)f;
    (void)nr;
    (void)args;
    return 0;
}

static int64_t lx_enosys(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)f;
    (void)nr;
    (void)args;
    return -FINTE_ENOSYS;
}

static int64_t lx_einval(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)f;
    (void)nr;
    (void)args;
    return -FINTE_EINVAL;
}

static int64_t lx_erofs(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)f;
    (void)nr;
    (void)args;
    return -FINTE_EROFS;
}

static int64_t lx_read(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_write(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_writev(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_readv(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_open(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_openat(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_stat(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
    const char *path = (const char *)LX_PTR(args[0]);
    flinux_stat_t *st = (flinux_stat_t *)LX_PTR(args[1]);
    if (!path || !st) {
        return -FINTE_EFAULT;
    }
    return stat_relative(f, path, st);
}

static int64_t lx_access(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_dup(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_dup2(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_close(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_lseek(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_pread64(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_mmap(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_brk(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_nanosleep(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
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

static int64_t lx_getpid(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
    (void)args;
    return (int64_t)f->slot + 1;
}

static int64_t lx_getcwd(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)f;
    (void)nr;
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

static int64_t lx_sched_yield(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
    (void)args;
    if (finte_flow_current() == f && hal_cpu_self()->sched_ctx) {
        fx_yield(f);
    }
    return 0;
}

static int64_t lx_sched_getaffinity(fflow_t *f, uint64_t nr,
                                    const uint64_t args[6])
{
    (void)f;
    (void)nr;
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

static int64_t lx_uname(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)f;
    (void)nr;
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

static int64_t lx_clock_gettime(fflow_t *f, uint64_t nr,
                                const uint64_t args[6])
{
    (void)f;
    (void)nr;
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

static int64_t lx_clock_getres(fflow_t *f, uint64_t nr,
                               const uint64_t args[6])
{
    (void)f;
    (void)nr;
    int clk = (int)args[0];
    flinux_timespec_t *ts = (flinux_timespec_t *)LX_PTR(args[1]);
    if (!ts || (clk != FINTE_CLOCK_REALTIME && clk != FINTE_CLOCK_MONOTONIC)) {
        return -FINTE_EINVAL;
    }
    ts->tv_sec = 0;
    ts->tv_nsec = 1;
    return 0;
}

static int64_t lx_gettimeofday(fflow_t *f, uint64_t nr,
                               const uint64_t args[6])
{
    (void)f;
    (void)nr;
    flinux_timeval_t *tv = (flinux_timeval_t *)LX_PTR(args[0]);
    if (!tv) {
        return -FINTE_EFAULT;
    }
    uint64_t ns = hal_now_ns();
    tv->tv_sec = (int64_t)(ns / 1000000000ull);
    tv->tv_usec = (int64_t)((ns % 1000000000ull) / 1000ull);
    return 0;
}

static int64_t lx_getrandom(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)f;
    (void)nr;
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

static int64_t lx_fstat(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
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

static int64_t lx_exit(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    (void)nr;
    finte_flow_exit(f, (int)args[0]);
    return 0;
}

/* Sorted by syscall number for binary search. */
static const lx_entry_t lx_table[] = {
    { FINTE_NR_read, lx_read },
    { FINTE_NR_write, lx_write },
    { FINTE_NR_open, lx_open },
    { FINTE_NR_close, lx_close },
    { FINTE_NR_stat, lx_stat },
    { FINTE_NR_fstat, lx_fstat },
    { FINTE_NR_lstat, lx_stat },
    { FINTE_NR_lseek, lx_lseek },
    { FINTE_NR_mmap, lx_mmap },
    { FINTE_NR_mprotect, lx_zero },
    { FINTE_NR_munmap, lx_zero },
    { FINTE_NR_brk, lx_brk },
    { FINTE_NR_rt_sigaction, lx_zero },
    { FINTE_NR_ioctl, lx_enosys },
    { FINTE_NR_pread64, lx_pread64 },
    { FINTE_NR_pwrite64, lx_erofs },
    { FINTE_NR_readv, lx_readv },
    { FINTE_NR_writev, lx_writev },
    { FINTE_NR_access, lx_access },
    { FINTE_NR_sched_yield, lx_sched_yield },
    { FINTE_NR_dup, lx_dup },
    { FINTE_NR_dup2, lx_dup2 },
    { FINTE_NR_nanosleep, lx_nanosleep },
    { FINTE_NR_getpid, lx_getpid },
    { FINTE_NR_exit, lx_exit },
    { FINTE_NR_uname, lx_uname },
    { FINTE_NR_fcntl, lx_zero },
    { FINTE_NR_getcwd, lx_getcwd },
    { FINTE_NR_readlink, lx_einval },
    { FINTE_NR_gettimeofday, lx_gettimeofday },
    { FINTE_NR_getuid, lx_zero },
    { FINTE_NR_getgid, lx_zero },
    { FINTE_NR_getppid, lx_zero },
    { FINTE_NR_arch_prctl, lx_zero },
    { FINTE_NR_gettid, lx_getpid },
    { FINTE_NR_sched_getaffinity, lx_sched_getaffinity },
    { FINTE_NR_set_tid_address, lx_getpid },
    { FINTE_NR_clock_gettime, lx_clock_gettime },
    { FINTE_NR_clock_getres, lx_clock_getres },
    { FINTE_NR_exit_group, lx_exit },
    { FINTE_NR_openat, lx_openat },
    { FINTE_NR_newfstatat, lx_fstat },
    { FINTE_NR_prlimit64, lx_zero },
    { FINTE_NR_getrandom, lx_getrandom },
};

static const lx_entry_t *lx_lookup(uint64_t nr)
{
    size_t lo = 0;
    size_t hi = FINTE_ARRAY_SIZE(lx_table);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (lx_table[mid].nr == nr) {
            return &lx_table[mid];
        }
        if (lx_table[mid].nr < nr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

int64_t finte_linux_call(fflow_t *f, uint64_t nr, const uint64_t args[6])
{
    if (!f) {
        return -FINTE_EINVAL;
    }
    const lx_entry_t *entry = lx_lookup(nr);
    return entry ? entry->fn(f, nr, args) : -FINTE_ENOSYS;
}
