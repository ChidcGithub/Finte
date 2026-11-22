/* Finte CLI shell. No libc: all formatting uses finte_format. */
#include "finte/finte.h"
#include "finte/cas.h"
#include "finte/demo.h"
#include "finte/guest.h"
#include "finte/initrd.h"
#include "finte/selftest.h"
#include "shell.h"
#include "input.h"

#define LINE_MAX 160
#define ARG_MAX 16

#define SH_RESET   "\x1b[0m"
#define SH_CYAN    "\x1b[36m"
#define SH_BLUE    "\x1b[34m"
#define SH_GREEN   "\x1b[32m"
#define SH_RED     "\x1b[31m"
#define SH_YELLOW  "\x1b[33m"
#define SH_BOLD_CYAN "\x1b[1;36m"
#define SH_BOLD_GREEN "\x1b[1;32m"
#define SH_BOLD_RED "\x1b[1;31m"

static fflow_t *g_sh_flow;
static char g_cwd[FDISK_PATH_MAX] = "/";
volatile uint64_t g_perf_last_us;

static void sh_puts(const char *s)
{
    hal_console_write(s, finte_strlen(s));
}

/* Prompt and live input: no timestamp prefix, so redraws stay stable. */
static void sh_puts_raw(const char *s)
{
    hal_console_write_raw(s, finte_strlen(s));
}

static void sh_printf(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        hal_console_write(buf, (size_t)n);
    }
}

/* Every command receives the same context, so the registry does not need a
 * special case for each argument shape. */
typedef struct sh_ctx {
    int argc;
    char **argv;
    const fboot_info_t *boot;
} sh_ctx_t;

typedef struct sh_command {
    const char *name;
    const char *desc;
    void (*run)(const sh_ctx_t *ctx);
} sh_command_t;

static const char *sh_arg(const sh_ctx_t *ctx, int index)
{
    return index < ctx->argc ? ctx->argv[index] : NULL;
}

_Static_assert(FINTE_FLOW_DEAD == 6,
               "flow state names must match the flow state enum");

static const char *state_name(uint8_t state)
{
    static const char *const names[] = {
        "free", "ready", "running", "blocked", "sleeping", "throttled", "dead"
    };
    return state < FINTE_ARRAY_SIZE(names) ? names[state] : "?";
}

static void cmd_color(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_puts(SH_BOLD_CYAN "ANSI colors" SH_RESET "\n");
    sh_puts("\x1b[30mblack  \x1b[31mred  \x1b[32mgreen  \x1b[33myellow  "
            "\x1b[34mblue  \x1b[35mmagenta  \x1b[36mcyan  \x1b[37mwhite"
            SH_RESET "\n");
    sh_puts("\x1b[90mgray  \x1b[91mbright red  \x1b[92mbright green  "
            "\x1b[93mbright yellow  \x1b[94mbright blue  "
            "\x1b[95mbright magenta  \x1b[96mbright cyan  "
            "\x1b[97mbright white" SH_RESET "\n");
}

static int join_words(int argc, char **argv, int start, int end,
                      char *buf, size_t cap)
{
    (void)argc;
    size_t used = 0;
    if (start >= end) {
        return -1;
    }
    buf[0] = 0;
    for (int i = start; i < end; i++) {
        size_t n = finte_strlen(argv[i]);
        size_t need = n + (i > start ? 1u : 0u);
        if (used + need + 1 > cap) {
            return -1;
        }
        if (i > start) {
            buf[used++] = ' ';
        }
        finte_memcpy(buf + used, argv[i], n);
        used += n;
        buf[used] = 0;
    }
    return 0;
}

static int parse_rational_arg(const char *s, int64_t *on, int64_t *od)
{
    if (!s || !*s) {
        return -1;
    }
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (*s < '0' || *s > '9') {
        return -1;
    }
    uint64_t n = 0;
    while (*s >= '0' && *s <= '9') {
        uint64_t digit = (uint64_t)(*s - '0');
        if (n > (UINT64_MAX - digit) / 10u) {
            return -1;
        }
        n = n * 10u + digit;
        s++;
    }
    uint64_t d = 1;
    if (*s == '/') {
        s++;
        if (*s < '0' || *s > '9') {
            return -1;
        }
        d = 0;
        while (*s >= '0' && *s <= '9') {
            uint64_t digit = (uint64_t)(*s - '0');
            if (d > (UINT64_MAX - digit) / 10u) {
                return -1;
            }
            d = d * 10u + digit;
            s++;
        }
        if (d == 0) {
            return -1;
        }
    }
    if (*s) {
        return -1;
    }
    if (n > (uint64_t)INT64_MAX || d > (uint64_t)INT64_MAX) {
        return -1;
    }
    *on = neg ? -(int64_t)n : (int64_t)n;
    *od = (int64_t)d;
    return 0;
}

static void cmd_cas(const sh_ctx_t *ctx)
{
    int argc = ctx->argc;
    char **argv = ctx->argv;
    char expr[512];
    char out[512];
    char err[128];
    fcas_poly_t poly;
    fcas_poly_t result;

    if (argc < 2) {
        sh_puts("usage: cas <expr>\n");
        sh_puts("       cas diff <expr>\n");
        sh_puts("       cas eval <expr> <x>\n");
        sh_puts("  examples: cas x^2+2*x+1\n");
        sh_puts("            cas diff x^3+2*x\n");
        sh_puts("            cas eval x^2+1 3\n");
        return;
    }

    const char *mode = argv[1];
    int rc;
    if (finte_strcmp(mode, "diff") == 0) {
        if (argc < 3 || join_words(argc, argv, 2, argc, expr,
                                   sizeof(expr)) != 0) {
            sh_printf(SH_RED "cas: expression too long" SH_RESET "\n");
            return;
        }
        rc = fcas_parse(expr, &poly, err, sizeof(err));
        if (rc == 0) {
            rc = fcas_diff(&poly, &result);
        }
        if (rc != 0) {
            sh_printf(SH_RED "cas: %s" SH_RESET "\n",
                      err[0] ? err : "cannot differentiate");
            return;
        }
    } else if (finte_strcmp(mode, "eval") == 0) {
        if (argc < 4 ||
            join_words(argc, argv, 2, argc - 1, expr, sizeof(expr)) != 0) {
            sh_puts("usage: cas eval <expr> <x>\n");
            return;
        }
        int64_t xn;
        int64_t xd;
        if (parse_rational_arg(argv[argc - 1], &xn, &xd) != 0) {
            sh_printf(SH_RED "cas: invalid x value" SH_RESET "\n");
            return;
        }
        rc = fcas_parse(expr, &poly, err, sizeof(err));
        if (rc == 0) {
            rc = fcas_eval(&poly, xn, xd, &xn, &xd);
        }
        if (rc != 0) {
            sh_printf(SH_RED "cas: %s" SH_RESET "\n",
                      err[0] ? err : "cannot evaluate");
            return;
        }
        if (xd == 1) {
            sh_printf(SH_BOLD_GREEN "%lld" SH_RESET "\n", (long long)xn);
        } else {
            sh_printf(SH_BOLD_GREEN "%lld/%lld" SH_RESET "\n",
                      (long long)xn, (long long)xd);
        }
        return;
    } else {
        if (join_words(argc, argv, 1, argc, expr, sizeof(expr)) != 0) {
            sh_printf(SH_RED "cas: expression too long" SH_RESET "\n");
            return;
        }
        rc = fcas_parse(expr, &poly, err, sizeof(err));
        if (rc != 0) {
            sh_printf(SH_RED "cas: %s" SH_RESET "\n", err);
            return;
        }
        result = poly;
    }

    rc = fcas_format(&result, out, sizeof(out));
    if (rc != 0) {
        sh_puts(SH_RED "cas: result too long" SH_RESET "\n");
        return;
    }
    sh_printf(SH_BOLD_GREEN "%s" SH_RESET "\n", out);
}

static void sh_size(uint64_t bytes)
{
    if (bytes >= 1024ull * 1024ull) {
        uint64_t whole = bytes / (1024ull * 1024ull);
        uint64_t frac = (bytes % (1024ull * 1024ull)) * 10ull /
                        (1024ull * 1024ull);
        sh_printf("%llu.%llu MiB", (unsigned long long)whole,
                  (unsigned long long)frac);
    } else if (bytes >= 1024ull) {
        uint64_t whole = bytes / 1024ull;
        uint64_t frac = (bytes % 1024ull) * 10ull / 1024ull;
        sh_printf("%llu.%llu KiB", (unsigned long long)whole,
                  (unsigned long long)frac);
    } else {
        sh_printf("%llu B", (unsigned long long)bytes);
    }
}

static void cmd_disks(const sh_ctx_t *ctx)
{
    (void)ctx;
    int n = finte_disk_count();
    if (n == 0) {
        sh_puts("no block devices\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        const fdisk_dev_t *d = finte_disk_get(i);
        if (!d) {
            continue;
        }
        sh_printf("%s  ", d->name);
        sh_size(d->sectors * d->sector_size);
        sh_printf("  sector=%u  %s  partitions=%d\n", d->sector_size,
                  d->scheme == 2 ? "GPT" : d->scheme == 1 ? "MBR" : "raw",
                  d->part_count);
    }
}

static void cmd_parts(const sh_ctx_t *ctx)
{
    (void)ctx;
    int n = finte_disk_count();
    if (n == 0) {
        sh_puts("no block devices\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        const fdisk_dev_t *d = finte_disk_get(i);
        if (!d) {
            continue;
        }
        for (int j = 0; j < d->part_count; j++) {
            const fdisk_part_t *p = &d->parts[j];
            if (p->number) {
                sh_printf("%sp%u  ", d->name, p->number);
            } else {
                sh_printf("%s    ", d->name);
            }
            sh_size(p->sectors * d->sector_size);
            sh_printf("  start=%llu  %s", (unsigned long long)p->start,
                      p->type_name[0] ? p->type_name : "?");
            if (p->label[0]) {
                sh_printf("  \"%s\"", p->label);
            }
            sh_puts("\n");
        }
    }
}

static void cmd_mount(const sh_ctx_t *ctx)
{
    const char *source = sh_arg(ctx, 1);
    const char *target = sh_arg(ctx, 2);
    const char *fstype = sh_arg(ctx, 3);
    if (!source && !target) {
        fmount_info_t m[FMOUNT_MAX];
        int n = finte_disk_mount_list(m, FMOUNT_MAX);
        if (n == 0) {
            sh_puts("(no mounts)\n");
            return;
        }
        for (int i = 0; i < n; i++) {
            sh_printf("%s on %s type %s\n", m[i].source, m[i].target,
                      m[i].fstype);
        }
        return;
    }
    if (!source || !target) {
        sh_puts("usage: mount <dev> <dir> [ntfs|ext4|lfc1]\n");
        return;
    }
    int rc = finte_disk_mount(source, target, fstype);
    if (rc != 0) {
        sh_printf("mount: %s\n", finte_disk_error(rc));
        return;
    }
    sh_printf("mounted %s on %s", source, target);
    fdfs_info_t info;
    if (finte_disk_info(target, &info) == 0) {
        sh_printf(" type=%s label=%s", info.fstype,
                  info.label[0] ? info.label : "-");
    }
    sh_puts("\n");
}

static void cmd_umount(const sh_ctx_t *ctx)
{
    const char *target = sh_arg(ctx, 1);
    if (!target) {
        sh_puts("usage: umount <dir>\n");
        return;
    }
    int rc = finte_disk_umount(target);
    if (rc != 0) {
        sh_printf("umount: %s\n", finte_disk_error(rc));
        return;
    }
    size_t n = finte_strlen(target);
    if (finte_strncmp(g_cwd, target, n) == 0 &&
        (g_cwd[n] == '\0' || g_cwd[n] == '/')) {
        finte_strlcpy(g_cwd, "/", sizeof(g_cwd));
    }
    sh_printf("unmounted %s\n", target);
}

static void cmd_fsinfo(const sh_ctx_t *ctx)
{
    const char *path = sh_arg(ctx, 1);
    if (!path) {
        sh_puts("usage: fsinfo <dir>\n");
        return;
    }
    fdfs_info_t info;
    int rc = finte_disk_info(path, &info);
    if (rc != 0) {
        sh_printf("fsinfo: %s\n", finte_disk_error(rc));
        return;
    }
    sh_printf("fstype=%s label=%s block=%u\n", info.fstype,
              info.label[0] ? info.label : "-", info.block_size);
    sh_puts("size=");
    sh_size(info.total_bytes);
    if (info.used_bytes) {
        sh_puts(" used=");
        sh_size(info.used_bytes);
    }
    sh_puts("\n");
}

static void shell_dirent(void *ctx, const char *name, const fdfs_stat_t *st)
{
    (void)ctx;
    if (st->mode == FDISK_MODE_DIR) {
        sh_printf("  " SH_BLUE "d %s/" SH_RESET "\n", name);
    } else {
        sh_printf("  f %s (%llu bytes)\n", name,
                  (unsigned long long)st->size);
    }
}

static int path_normalize(const char *arg, char *out, size_t cap)
{
    char tmp[FDISK_PATH_MAX * 2 + 2];
    if (!arg || !*arg) {
        arg = g_cwd;
    }
    if (arg[0] == '/') {
        finte_strlcpy(tmp, arg, sizeof(tmp));
    } else if (finte_strcmp(g_cwd, "/") == 0) {
        finte_format(tmp, sizeof(tmp), "/%s", arg);
    } else {
        finte_format(tmp, sizeof(tmp), "%s/%s", g_cwd, arg);
    }
    size_t o = 0;
    out[o++] = '/';
    const char *p = tmp;
    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *q = p;
        while (*q && *q != '/') {
            q++;
        }
        size_t n = (size_t)(q - p);
        if (n == 1 && p[0] == '.') {
            /* keep the current directory */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (o > 1) {
                o--;
                while (o > 1 && out[o - 1] != '/') {
                    o--;
                }
            }
        } else {
            if (o > 1) {
                if (o + 1 >= cap) {
                    return FINTE_ERR_INVAL;
                }
                out[o++] = '/';
            }
            if (o + n >= cap) {
                return FINTE_ERR_INVAL;
            }
            finte_memcpy(out + o, p, n);
            o += n;
        }
        p = q;
    }
    if (o == 0) {
        out[o++] = '/';
    }
    out[o] = 0;
    return 0;
}

static int is_disk_arg(const char *arg)
{
    return arg && (arg[0] == '/' || finte_strcmp(g_cwd, "/") != 0);
}

static int disk_arg(const char *arg, char *out)
{
    return path_normalize(arg, out, FDISK_PATH_MAX);
}

static const char *path_base(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            b = p + 1;
        }
    }
    return b;
}

static int sh_file_open(const char *path, uint32_t flags, fhandle_t *h,
                        uint16_t *cap, uint32_t rights)
{
    int rc = finte_disk_file_open(path, flags, h);
    if (rc != 0) {
        return rc;
    }
    int c = finte_cap_install(g_sh_flow, *h, rights);
    if (c < 0) {
        finte_file_handle_close(*h);
        return c;
    }
    *cap = (uint16_t)c;
    return 0;
}

static void sh_file_close(fhandle_t h, uint16_t cap)
{
    finte_cap_drop_index(g_sh_flow, cap);
    finte_file_handle_close(h);
}

static void sh_sync(void)
{
    int rc = finte_disk_sync();
    if (rc != 0) {
        sh_printf("sync: %s\n", finte_disk_error(rc));
        hal_watchdog_strike("disk sync failure");
    }
}

static void cmd_cat_ramdisk(const char *name)
{
    int i = finte_initrd_find(name);
    if (i < 0) {
        sh_puts("cat: no such file\n");
        return;
    }
    const char *data = (const char *)finte_initrd_data((uint32_t)i);
    uint32_t size = finte_initrd_size((uint32_t)i);
    hal_console_write(data, size);
    if (size && data[size - 1] != '\n') {
        sh_puts("\n");
    }
}

static void ramdisk_ls(void)
{
    uint32_t n = finte_initrd_count();
    if (n == 0) {
        sh_puts("(empty ramdisk)\n");
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        sh_printf("  %s (%u bytes)\n", finte_initrd_name(i),
                  (unsigned)finte_initrd_size(i));
    }
}

static void cmd_ls(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    char path[FDISK_PATH_MAX];
    if (!arg) {
        if (finte_strcmp(g_cwd, "/") == 0) {
            ramdisk_ls();
            return;
        }
        finte_strlcpy(path, g_cwd, sizeof(path));
    } else {
        if (!is_disk_arg(arg)) {
            ramdisk_ls();
            return;
        }
        if (disk_arg(arg, path) != 0) {
            sh_puts("ls: path too long\n");
            return;
        }
    }
    int rc = finte_disk_ls(path, shell_dirent, NULL);
    if (rc != 0) {
        sh_printf("ls: %s\n", finte_disk_error(rc));
    }
}

static void cmd_cat(const sh_ctx_t *ctx)
{
    const char *name = sh_arg(ctx, 1);
    if (!name) {
        sh_puts("usage: cat <file>\n");
        return;
    }
    if (!is_disk_arg(name)) {
        cmd_cat_ramdisk(name);
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(name, path) != 0) {
        sh_puts("cat: path too long\n");
        return;
    }
    fhandle_t h;
    uint16_t cap;
    int rc = sh_file_open(path, FINTE_OPEN_READ, &h, &cap, FINTE_R_READ);
    if (rc != 0) {
        sh_printf("cat: %s\n", finte_disk_error(rc));
        return;
    }
    uint8_t buf[512];
    uint64_t off = 0;
    char last = '\n';
    for (;;) {
        fret_t r = fx_file_read(g_sh_flow, cap, off, buf, sizeof(buf));
        if (r.status != FINTE_OK) {
            sh_printf("cat: %s\n", finte_strstatus(r.status));
            break;
        }
        if (r.value == 0) {
            break;
        }
        hal_console_write((const char *)buf, (size_t)r.value);
        last = (char)buf[r.value - 1];
        off += r.value;
    }
    sh_file_close(h, cap);
    if (last != '\n') {
        sh_puts("\n");
    }
}

static void cmd_stat(const sh_ctx_t *ctx)
{
    const char *name = sh_arg(ctx, 1);
    if (!name) {
        sh_puts("usage: stat <file>\n");
        return;
    }
    if (!is_disk_arg(name)) {
        int i = finte_initrd_find(name);
        if (i < 0) {
            sh_puts("stat: no such file\n");
            return;
        }
        sh_printf("name=%s size=%u\n", finte_initrd_name((uint32_t)i),
                  (unsigned)finte_initrd_size((uint32_t)i));
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(name, path) != 0) {
        sh_puts("stat: path too long\n");
        return;
    }
    fdfs_stat_t st;
    int rc = finte_disk_stat(path, &st);
    if (rc != 0) {
        sh_printf("stat: %s\n", finte_disk_error(rc));
        return;
    }
    sh_printf("name=%s type=%s size=%llu\n", path,
              st.mode == FDISK_MODE_DIR ? "dir" : "file",
              (unsigned long long)st.size);
}

static void cmd_write(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    int argc = ctx->argc;
    char **argv = ctx->argv;
    if (!arg || argc < 3) {
        sh_puts("usage: write <path> <text...>\n");
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(arg, path) != 0) {
        sh_puts("write: path too long\n");
        return;
    }
    fhandle_t h;
    uint16_t cap;
    int rc = sh_file_open(path, FINTE_OPEN_WRITE | FINTE_OPEN_CREATE |
                          FINTE_OPEN_TRUNC, &h, &cap, FINTE_R_WRITE);
    if (rc != 0) {
        sh_printf("write: %s\n", finte_disk_error(rc));
        return;
    }
    uint64_t off = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2) {
            fret_t sp = fx_file_write(g_sh_flow, cap, off, " ", 1);
            if (sp.status != FINTE_OK) {
                sh_printf("write: %s\n", finte_strstatus(sp.status));
                break;
            }
            off++;
        }
        size_t len = finte_strlen(argv[i]);
        fret_t r = fx_file_write(g_sh_flow, cap, off, argv[i], len);
        if (r.status != FINTE_OK) {
            sh_printf("write: %s\n", finte_strstatus(r.status));
            break;
        }
        off += len;
    }
    sh_file_close(h, cap);
    rc = finte_disk_sync();
    if (rc != 0) {
        sh_printf("sync: %s\n", finte_disk_error(rc));
    }
}

static void cmd_mkdir(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    if (!arg) {
        sh_puts("usage: mkdir <dir>\n");
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(arg, path) != 0) {
        sh_puts("mkdir: path too long\n");
        return;
    }
    int rc = finte_disk_create(path, 1);
    if (rc != 0) {
        sh_printf("mkdir: %s\n", finte_disk_error(rc));
        return;
    }
    sh_sync();
}

static void cmd_rm(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    if (!arg) {
        sh_puts("usage: rm <path>\n");
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(arg, path) != 0) {
        sh_puts("rm: path too long\n");
        return;
    }
    int rc = finte_disk_unlink(path);
    if (rc != 0) {
        sh_printf("rm: %s\n", finte_disk_error(rc));
        return;
    }
    sh_sync();
}

static void cmd_touch(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    if (!arg) {
        sh_puts("usage: touch <path>\n");
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(arg, path) != 0) {
        sh_puts("touch: path too long\n");
        return;
    }
    fdfs_stat_t st;
    int rc = finte_disk_stat(path, &st);
    if (rc == FINTE_ERR_NOENT) {
        rc = finte_disk_create(path, 0);
        if (rc == 0) {
            sh_sync();
        }
    }
    if (rc != 0) {
        sh_printf("touch: %s\n", finte_disk_error(rc));
    }
}

static void cmd_mv(const sh_ctx_t *ctx)
{
    const char *from = sh_arg(ctx, 1);
    const char *to = sh_arg(ctx, 2);
    if (!from || !to) {
        sh_puts("usage: mv <src> <dst>\n");
        return;
    }
    char fpath[FDISK_PATH_MAX], tpath[FDISK_PATH_MAX];
    if (disk_arg(from, fpath) != 0 || disk_arg(to, tpath) != 0) {
        sh_puts("mv: path too long\n");
        return;
    }
    fdfs_stat_t st;
    if (finte_disk_stat(tpath, &st) == 0 && st.mode == FDISK_MODE_DIR) {
        char joined[FDISK_PATH_MAX];
        if (finte_format(joined, sizeof(joined), "%s/%s", tpath,
                         path_base(fpath)) < 0) {
            sh_puts("mv: path too long\n");
            return;
        }
        finte_strlcpy(tpath, joined, sizeof(tpath));
    }
    int rc = finte_disk_rename(fpath, tpath);
    if (rc != 0) {
        sh_printf("mv: %s\n", finte_disk_error(rc));
        return;
    }
    sh_sync();
}

static void cmd_cp(const sh_ctx_t *ctx)
{
    const char *from = sh_arg(ctx, 1);
    const char *to = sh_arg(ctx, 2);
    if (!from || !to) {
        sh_puts("usage: cp <src> <dst>\n");
        return;
    }
    char dst[FDISK_PATH_MAX];
    if (disk_arg(to, dst) != 0) {
        sh_puts("cp: destination too long\n");
        return;
    }
    fdfs_stat_t dstst;
    if (finte_disk_stat(dst, &dstst) == 0 && dstst.mode == FDISK_MODE_DIR) {
        char joined[FDISK_PATH_MAX];
        if (finte_format(joined, sizeof(joined), "%s/%s", dst,
                         path_base(from)) < 0) {
            sh_puts("cp: destination too long\n");
            return;
        }
        finte_strlcpy(dst, joined, sizeof(dst));
    }

    int src_disk = is_disk_arg(from);
    char src[FDISK_PATH_MAX];
    const uint8_t *ram = NULL;
    uint32_t ram_size = 0;
    int ram_idx = -1;
    if (src_disk) {
        if (disk_arg(from, src) != 0) {
            sh_puts("cp: source too long\n");
            return;
        }
        if (finte_strcmp(src, dst) == 0) {
            sh_puts("cp: source and destination are the same\n");
            return;
        }
    } else {
        ram_idx = finte_initrd_find(from);
        if (ram_idx < 0) {
            sh_puts("cp: no such file\n");
            return;
        }
        ram = (const uint8_t *)finte_initrd_data((uint32_t)ram_idx);
        ram_size = finte_initrd_size((uint32_t)ram_idx);
    }

    fhandle_t dh;
    uint16_t dcap;
    int rc = sh_file_open(dst, FINTE_OPEN_WRITE | FINTE_OPEN_CREATE |
                          FINTE_OPEN_TRUNC, &dh, &dcap, FINTE_R_WRITE);
    if (rc != 0) {
        sh_printf("cp: %s\n", finte_disk_error(rc));
        return;
    }
    fhandle_t sh = FINTE_HANDLE_NONE;
    uint16_t scap = 0;
    if (src_disk) {
        rc = sh_file_open(src, FINTE_OPEN_READ, &sh, &scap, FINTE_R_READ);
        if (rc != 0) {
            sh_printf("cp: %s\n", finte_disk_error(rc));
            sh_file_close(dh, dcap);
            return;
        }
    }

    uint8_t buf[512];
    uint64_t off = 0;
    for (;;) {
        size_t len = 0;
        if (src_disk) {
            fret_t r = fx_file_read(g_sh_flow, scap, off, buf, sizeof(buf));
            if (r.status != FINTE_OK) {
                sh_printf("cp: %s\n", finte_strstatus(r.status));
                break;
            }
            if (r.value == 0) {
                break;
            }
            len = (size_t)r.value;
        } else {
            if (off >= ram_size) {
                break;
            }
            len = ram_size - (uint32_t)off;
            if (len > sizeof(buf)) {
                len = sizeof(buf);
            }
            finte_memcpy(buf, ram + off, len);
        }
        fret_t w = fx_file_write(g_sh_flow, dcap, off, buf, len);
        if (w.status != FINTE_OK) {
            sh_printf("cp: %s\n", finte_strstatus(w.status));
            break;
        }
        off += len;
    }
    if (src_disk) {
        sh_file_close(sh, scap);
    }
    sh_file_close(dh, dcap);
    rc = finte_disk_sync();
    if (rc != 0) {
        sh_printf("sync: %s\n", finte_disk_error(rc));
    }
}

static void cmd_df(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    if (arg) {
        char path[FDISK_PATH_MAX];
        if (disk_arg(arg, path) != 0) {
            sh_puts("df: path too long\n");
            return;
        }
        fdfs_info_t info;
        int rc = finte_disk_info(path, &info);
        if (rc != 0) {
            sh_printf("df: %s\n", finte_disk_error(rc));
            return;
        }
        sh_printf("%s: %s total=", path, info.fstype);
        sh_size(info.total_bytes);
        sh_puts(" free=");
        sh_size(info.free_bytes ? info.free_bytes
                                : info.total_bytes - info.used_bytes);
        sh_puts("\n");
        return;
    }
    fmount_info_t m[FMOUNT_MAX];
    int n = finte_disk_mount_list(m, FMOUNT_MAX);
    if (n == 0) {
        sh_puts("(no mounts)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        fdfs_info_t info;
        if (finte_disk_info(m[i].target, &info) != 0) {
            continue;
        }
        sh_printf("%s on %s: %s ", m[i].source, m[i].target, info.fstype);
        sh_size(info.total_bytes);
        sh_puts(" free=");
        sh_size(info.free_bytes ? info.free_bytes
                                : info.total_bytes - info.used_bytes);
        sh_puts("\n");
    }
}

static int parse_count(const char *arg, int def)
{
    if (!arg || !*arg) {
        return def;
    }
    int v = 0;
    while (*arg >= '0' && *arg <= '9') {
        v = v * 10 + (*arg++ - '0');
    }
    return v > 0 ? v : def;
}

static int read_all(const char *path, uint8_t **out, size_t *out_len,
                    size_t *out_alloc)
{
    fdfs_stat_t st;
    int rc = finte_disk_stat(path, &st);
    if (rc != 0) {
        return rc;
    }
    if (st.mode != FDISK_MODE_FILE || st.size > 1024u * 1024u) {
        return FINTE_ERR_LIMIT;
    }
    size_t alloc = ((size_t)st.size + 1u + 4095u) & ~(size_t)4095u;
    uint8_t *buf = (uint8_t *)hal_pages_alloc(alloc);
    if (!buf) {
        return FINTE_ERR_NOMEM;
    }
    fhandle_t h;
    uint16_t cap;
    rc = sh_file_open(path, FINTE_OPEN_READ, &h, &cap, FINTE_R_READ);
    if (rc != 0) {
        hal_pages_free(buf, alloc);
        return rc;
    }
    size_t got = 0;
    while (got < st.size) {
        fret_t r = fx_file_read(g_sh_flow, cap, got, buf + got,
                                (size_t)st.size - got);
        if (r.status != FINTE_OK) {
            rc = r.status;
            break;
        }
        if (r.value == 0) {
            break;
        }
        got += (size_t)r.value;
    }
    sh_file_close(h, cap);
    if (rc != 0) {
        hal_pages_free(buf, alloc);
        return rc;
    }
    buf[got] = 0;
    *out = buf;
    *out_len = got;
    *out_alloc = alloc;
    return 0;
}

static void cmd_head(const sh_ctx_t *ctx)
{
    const char *patharg = sh_arg(ctx, 1);
    const char *countarg = sh_arg(ctx, 2);
    if (!patharg) {
        sh_puts("usage: head <file> [lines]\n");
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(patharg, path) != 0) {
        sh_puts("head: path too long\n");
        return;
    }
    uint8_t *buf;
    size_t len;
    size_t alloc;
    int rc = read_all(path, &buf, &len, &alloc);
    if (rc != 0) {
        sh_printf("head: %s\n", finte_disk_error(rc));
        return;
    }
    int want = parse_count(countarg, 10);
    int lines = 0;
    size_t i = 0;
    while (i < len && lines < want) {
        char one[2] = { (char)buf[i], 0 };
        sh_puts(one);
        if (buf[i] == '\n') {
            lines++;
        }
        i++;
    }
    if (len && i == len && buf[len - 1] != '\n') {
        sh_puts("\n");
    }
    hal_pages_free(buf, alloc);
}

static void cmd_tail(const sh_ctx_t *ctx)
{
    const char *patharg = sh_arg(ctx, 1);
    const char *countarg = sh_arg(ctx, 2);
    if (!patharg) {
        sh_puts("usage: tail <file> [lines]\n");
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(patharg, path) != 0) {
        sh_puts("tail: path too long\n");
        return;
    }
    uint8_t *buf;
    size_t len;
    size_t alloc;
    int rc = read_all(path, &buf, &len, &alloc);
    if (rc != 0) {
        sh_printf("tail: %s\n", finte_disk_error(rc));
        return;
    }
    int want = parse_count(countarg, 10);
    size_t start = 0;
    int lines = 0;
    for (size_t i = len; i > 0; i--) {
        if (buf[i - 1] == '\n') {
            if (lines == want) {
                start = i;
                break;
            }
            lines++;
        }
    }
    for (size_t i = start; i < len; i++) {
        char one[2] = { (char)buf[i], 0 };
        sh_puts(one);
    }
    if (len && buf[len - 1] != '\n') {
        sh_puts("\n");
    }
    hal_pages_free(buf, alloc);
}

static void cmd_hexdump(const sh_ctx_t *ctx)
{
    const char *patharg = sh_arg(ctx, 1);
    if (!patharg) {
        sh_puts("usage: hexdump <file>\n");
        return;
    }
    char path[FDISK_PATH_MAX];
    if (disk_arg(patharg, path) != 0) {
        sh_puts("hexdump: path too long\n");
        return;
    }
    fhandle_t h;
    uint16_t cap;
    int rc = sh_file_open(path, FINTE_OPEN_READ, &h, &cap, FINTE_R_READ);
    if (rc != 0) {
        sh_printf("hexdump: %s\n", finte_disk_error(rc));
        return;
    }
    uint8_t buf[16];
    uint64_t off = 0;
    for (;;) {
        fret_t r = fx_file_read(g_sh_flow, cap, off, buf, sizeof(buf));
        if (r.status != FINTE_OK) {
            sh_printf("hexdump: %s\n", finte_strstatus(r.status));
            break;
        }
        if (r.value == 0) {
            break;
        }
        sh_printf("%08llx  ", (unsigned long long)off);
        for (uint32_t i = 0; i < 16; i++) {
            if (i < r.value) {
                sh_printf("%02x ", buf[i]);
            } else {
                sh_puts("   ");
            }
        }
        sh_puts(" |");
        for (uint32_t i = 0; i < r.value; i++) {
            char ch = (char)buf[i];
            char one[2] = { (ch >= 32 && ch < 127) ? ch : '.', 0 };
            sh_puts(one);
        }
        sh_puts("|\n");
        off += r.value;
    }
    sh_file_close(h, cap);
}

typedef struct tree_ctx {
    int depth;
    char path[FDISK_PATH_MAX];
} tree_ctx_t;

static void tree_cb(void *vctx, const char *name, const fdfs_stat_t *st)
{
    tree_ctx_t *ctx = (tree_ctx_t *)vctx;
    for (int i = 0; i < ctx->depth; i++) {
        sh_puts("  ");
    }
    if (st->mode == FDISK_MODE_DIR) {
        sh_printf(SH_BLUE "d %s/" SH_RESET "\n", name);
        if (ctx->depth < 8) {
            char child[FDISK_PATH_MAX];
            if (finte_format(child, sizeof(child), "%s/%s", ctx->path,
                             name) > 0) {
                tree_ctx_t next;
                next.depth = ctx->depth + 1;
                finte_strlcpy(next.path, child, sizeof(next.path));
                (void)finte_disk_ls(child, tree_cb, &next);
            }
        }
    } else {
        sh_printf("f %s (%llu bytes)\n", name,
                  (unsigned long long)st->size);
    }
}

static void cmd_tree(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    char path[FDISK_PATH_MAX];
    if (arg) {
        if (disk_arg(arg, path) != 0) {
            sh_puts("tree: path too long\n");
            return;
        }
    } else if (finte_strcmp(g_cwd, "/") != 0) {
        finte_strlcpy(path, g_cwd, sizeof(path));
    } else {
        sh_puts("usage: tree <dir>\n");
        return;
    }
    tree_ctx_t tree;
    tree.depth = 0;
    finte_strlcpy(tree.path, path, sizeof(tree.path));
    int rc = finte_disk_ls(path, tree_cb, &tree);
    if (rc != 0) {
        sh_printf("tree: %s\n", finte_disk_error(rc));
    }
}

static void cmd_sync(const sh_ctx_t *ctx)
{
    (void)ctx;
    int rc = finte_disk_sync();
    if (rc != 0) {
        sh_printf(SH_RED "sync: %s" SH_RESET "\n", finte_disk_error(rc));
    } else {
        sh_puts(SH_GREEN "synced" SH_RESET "\n");
    }
}

static void cmd_cd(const sh_ctx_t *ctx)
{
    const char *arg = sh_arg(ctx, 1);
    char path[FDISK_PATH_MAX];
    if (!arg || !*arg) {
        finte_strlcpy(g_cwd, "/", sizeof(g_cwd));
        return;
    }
    if (disk_arg(arg, path) != 0) {
        sh_puts("cd: path too long\n");
        return;
    }
    if (finte_strcmp(path, "/") == 0) {
        finte_strlcpy(g_cwd, "/", sizeof(g_cwd));
        return;
    }
    fdfs_stat_t st;
    int rc = finte_disk_stat(path, &st);
    if (rc != 0) {
        sh_printf("cd: %s\n", finte_disk_error(rc));
        return;
    }
    if (st.mode != FDISK_MODE_DIR) {
        sh_puts("cd: not a directory\n");
        return;
    }
    finte_strlcpy(g_cwd, path, sizeof(g_cwd));
}

static void cmd_pwd(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_printf("%s\n", g_cwd);
}

static void cmd_user(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_sync();
    sh_puts("entering ring 3...\n");
    int rc = finte_usermode_run("init.elf");
    if (rc != 0) {
        sh_printf("user: %s\n", finte_disk_error(rc));
    }
}

static void cmd_procs(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_puts("starting isolated user services...\n");
    int rc = finte_proc_demo();
    if (rc != 0) {
        sh_printf("procs: failed (status %d)\n", rc);
    }
}

static void cmd_services(const sh_ctx_t *ctx)
{
    cmd_procs(ctx);
}

static void cmd_abi(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_puts("starting process ABI tests...\n");
    int rc = finte_proc_abi_test();
    sh_printf("abi: %s\n", rc == 0 ? "PASS" : "FAIL");
}



static void cmd_ps(const sh_ctx_t *ctx)
{
    (void)ctx;
    int any = 0;
    for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
        const fflow_t *f = &finte_core.flows[i];
        if (!f->in_use) {
            continue;
        }
        any = 1;
        sh_printf("  %s state=%s cpu=%u fuel=%u/%u\n", f->name,
                  state_name(f->state),
                  (unsigned)(f->thread ? f->thread->cpu : 0),
                  (unsigned)f->fuel, (unsigned)f->fuel_max);
    }
    if (!any) {
        sh_puts("(no live flows)\n");
    }
}

static void cmd_demo(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_puts("[shell] starting native pipeline\n");
    fret_t r = finte_demo_bootstrap();
    if (finte_failed(r)) {
        sh_printf("[shell] bootstrap failed: %s\n", finte_strstatus(r.status));
        return;
    }
    uint64_t t0 = hal_now_ns();
    finte_run();
    uint64_t dt = hal_now_ns() - t0;
    g_perf_last_us = dt / 1000;
    const fdemo_result_t *d = finte_demo_result();
    fstats_t s = finte_get_stats();
    sh_printf("[shell] pipeline=%s checksum=%llu/%llu denied=%llu\n",
              d->ok ? "OK" : "FAILED",
              (unsigned long long)d->checksum,
              (unsigned long long)d->expected_checksum,
              (unsigned long long)d->denied);
    sh_printf("[shell] syscalls=%llu msgs=%llu switches=%llu wall=%llu us\n",
              (unsigned long long)s.syscalls,
              (unsigned long long)s.messages_sent,
              (unsigned long long)s.context_switches,
              (unsigned long long)(dt / 1000));
}

/* Two CPU-bound flows with no cooperative yields. The spinner only stops
 * after the stopper runs, so this only completes when the timer preempts. */
static volatile uint32_t g_spin_stop;
static volatile uint64_t g_spin_count;
static volatile uint64_t g_stop_count;

static void preempt_spinner(fflow_t *f)
{
    (void)f;
    while (!g_spin_stop) {
        g_spin_count++;
    }
}

static void preempt_stopper(fflow_t *f)
{
    (void)f;
    for (uint64_t i = 0; i < 60000000ull; i++) {
        g_stop_count++;
    }
    g_spin_stop = 1;
}

static const fprogram_t g_spin_prog = { "spin", preempt_spinner, 4000 };
static const fprogram_t g_stop_prog = { "stop", preempt_stopper, 4000 };

static void cmd_preempt(const sh_ctx_t *ctx)
{
    (void)ctx;
    g_spin_stop = 0;
    g_spin_count = 0;
    g_stop_count = 0;
    fhandle_t a = finte_flow_create("spin", &g_spin_prog, 4000,
                                    FINTE_PERSONA_NATIVE);
    fhandle_t b = finte_flow_create("stop", &g_stop_prog, 4000,
                                    FINTE_PERSONA_NATIVE);
    if (!a || !b) {
        sh_puts("preempt: cannot create flows\n");
        return;
    }
    if (finte_failed(finte_flow_start(a)) ||
        finte_failed(finte_flow_start(b))) {
        sh_puts("preempt: cannot start flows\n");
        return;
    }
    uint64_t t0 = hal_now_ns();
    finte_run();
    uint64_t dt = hal_now_ns() - t0;
    g_perf_last_us = dt / 1000;
    sh_printf("[shell] preempt=%s spins=%llu stops=%llu wall=%llu us\n",
              g_spin_stop ? "OK" : "FAILED",
              (unsigned long long)g_spin_count,
              (unsigned long long)g_stop_count,
              (unsigned long long)(dt / 1000));
}

static void cmd_smp(const sh_ctx_t *ctx)
{
    (void)ctx;
    uint32_t n = hal_cpu_count();
    sh_printf("cpus: %u online\n", n);
    for (uint32_t i = 0; i < n; i++) {
        fcpu_t *cpu = hal_cpu_slot(i);
        if (!cpu || !cpu->online) {
            continue;
        }
        sh_printf("cpu%u hwid=%u ticks=%llu ipis=%llu switches=%llu "
                  "steals=%u migrations=%u %s\n",
                  (unsigned)i, (unsigned)cpu->hw_id,
                  (unsigned long long)cpu->ticks,
                  (unsigned long long)cpu->ipis,
                  (unsigned long long)cpu->switches,
                  (unsigned)cpu->steals, (unsigned)cpu->migrations,
                  cpu->current ? cpu->current->name : "idle");
    }
    hal_cpu_wake_all();
}

static void cmd_compat(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_puts("[shell] starting Linux x86_64 persona\n");
    fret_t r = finte_compat_bootstrap();
    if (finte_failed(r)) {
        sh_printf("[shell] persona bootstrap failed: %s\n",
                  finte_strstatus(r.status));
        return;
    }
    uint64_t t0 = hal_now_ns();
    finte_run();
    uint64_t dt = hal_now_ns() - t0;
    g_perf_last_us = dt / 1000;
    const fguest_result_t *g = finte_guest_result();
    sh_printf("[shell] persona=%s read=%lld pid=%lld uname=%s/%s\n",
              g->ok ? "OK" : "FAILED", (long long)g->bytes_read,
              (long long)g->pid, g->sysname, g->machine);
}

static void cmd_selfcheck(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_puts("[shell] running core self-check\n");
    fselftest_report_t report;
    int rc = finte_selftest_run(2, &report);
    sh_printf("[shell] self-check %s%s%s: %u sections, %u checks, %u failures\n",
              rc == 0 ? SH_BOLD_GREEN : SH_BOLD_RED,
              rc == 0 ? "PASS" : "FAIL", SH_RESET,
              report.sections, report.checks, report.failures);
}

static void cmd_watchdog(const sh_ctx_t *ctx)
{
    int argc = ctx->argc;
    char **argv = ctx->argv;
    if (argc > 1 && finte_strcmp(argv[1], "test") == 0) {
        sh_puts("watchdog: injecting abnormal event\n");
        hal_watchdog_strike("manual test");
        return;
    }
    if (argc > 1 && finte_strcmp(argv[1], "reset") == 0) {
        hal_watchdog_clear();
        sh_puts(SH_GREEN "watchdog: strikes cleared" SH_RESET "\n");
        return;
    }
    const char *last = hal_watchdog_last();
    sh_printf("watchdog: %d/3 strikes", hal_watchdog_strikes());
    if (last && last[0]) {
        sh_printf(", last: %s", last);
    }
    sh_puts("\n  usage: watchdog | watchdog test | watchdog reset\n");
}

static void cmd_mem(const sh_ctx_t *ctx)
{
    (void)ctx;
    uint64_t used = 0;
    uint64_t total = 0;
    hal_heap_info(&used, &total);
    fstats_t s = finte_get_stats();
    sh_printf("heap: %llu / %llu bytes\n",
              (unsigned long long)used, (unsigned long long)total);
    sh_printf("nodes=%u flows=%u channels=%u arenas=%u\n",
              s.nodes_live, s.flows_live, s.channels_live, s.arenas_live);
    sh_printf("syscalls=%llu switches=%llu msgs=%llu fuel=%llu epochs=%llu\n",
              (unsigned long long)s.syscalls,
              (unsigned long long)s.context_switches,
              (unsigned long long)s.messages_sent,
              (unsigned long long)s.fuel_burned,
              (unsigned long long)s.epochs);
}

static void cmd_boot(const sh_ctx_t *ctx)
{
    const fboot_info_t *bi = ctx->boot;
    if (!bi || bi->magic != FINTE_BOOT_MAGIC) {
        sh_puts("boot info: unavailable\n");
        return;
    }
    sh_printf("magic=0x%x version=%u drive=0x%x\n",
              (unsigned)bi->magic, (unsigned)bi->version,
              (unsigned)bi->boot_drive);
    sh_printf("kernel=%u bytes ramdisk=%u bytes at 0x%x\n",
              (unsigned)bi->kernel_size, (unsigned)bi->ramdisk_size,
              (unsigned)bi->ramdisk_addr);
    sh_printf("console=%s\n",
              bi->console_type == 2 ? "UEFI GOP framebuffer" : "VGA+serial");
}

static void cmd_uname(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_printf("Finte %s x86_64\n", FINTE_VERSION_STRING);
}

static void cmd_uptime(const sh_ctx_t *ctx)
{
    (void)ctx;
    uint64_t ns = hal_now_ns();
    sh_printf("up %llu.%03llu seconds\n",
              (unsigned long long)(ns / 1000000000ull),
              (unsigned long long)((ns / 1000000ull) % 1000ull));
}

static void cmd_reboot(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_sync();
    sh_puts("rebooting...\n");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void cmd_halt(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_sync();
    sh_puts("halted.\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void sh_shutdown_ok(const char *text)
{
    sh_puts("  " SH_GREEN "[ ok ]" SH_RESET " ");
    sh_puts(text);
    sh_puts("\n");
    hal_idle(120000000ull);
}

static void cmd_shutdown(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_puts(SH_BOLD_CYAN "[shutdown]" SH_RESET " Finte is going down\n");
    hal_idle(150000000ull);

    sh_shutdown_ok("stopped shell input");
    sh_shutdown_ok("parked idle CPUs");

    int rc = finte_disk_sync();
    if (rc != 0) {
        sh_printf("  " SH_BOLD_RED "[fail]" SH_RESET " sync: %s\n",
                  finte_disk_error(rc));
        hal_idle(150000000ull);
    } else {
        sh_shutdown_ok("flushed writable filesystems");
    }

    hal_timer_stop_cpu();
    sh_shutdown_ok("stopped timer preemption");

    hal_irq_disable();
    sh_shutdown_ok("disabled interrupts");

    sh_puts(SH_BOLD_CYAN "[shutdown]" SH_RESET " power off\n");
    hal_idle(200000000ull);
    hal_exit(0);
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

static void cmd_version(const sh_ctx_t *ctx)
{
    (void)ctx;
    sh_printf("Finte %s\n", FINTE_VERSION_STRING);
}

static void cmd_echo(const sh_ctx_t *ctx)
{
    for (int i = 1; i < ctx->argc; i++) {
        if (i > 1) {
            sh_puts(" ");
        }
        sh_puts(ctx->argv[i]);
    }
    sh_puts("\n");
}

static void cmd_clear(const sh_ctx_t *ctx)
{
    (void)ctx;
    hal_console_clear();
}

static void cmd_motd(const sh_ctx_t *ctx)
{
    (void)ctx;
    cmd_cat_ramdisk("motd.txt");
}

static void cmd_help(const sh_ctx_t *ctx);

/* One table drives completion, help text and dispatch. Adding a command
 * means adding one row, not editing three separate lists. */
static const sh_command_t sh_command_table[] = {
    { "boot", "boot protocol and ramdisk info", cmd_boot },
    { "cas", "cas <expr> | diff <expr> | eval <expr> <x>", cmd_cas },
    { "cat", "print a file", cmd_cat },
    { "cd", "change directory (disk paths)", cmd_cd },
    { "clear", "clear the screen", cmd_clear },
    { "color", "show the ANSI color palette", cmd_color },
    { "compat", "run the Linux x86_64 persona", cmd_compat },
    { "cp", "copy a ramdisk/disk file to disk", cmd_cp },
    { "demo", "run the native capability pipeline", cmd_demo },
    { "df", "filesystem usage", cmd_df },
    { "disks", "list block devices", cmd_disks },
    { "echo", "print text", cmd_echo },
    { "fsinfo", "filesystem details", cmd_fsinfo },
    { "halt", "halt the CPU", cmd_halt },
    { "head", "first lines of a file", cmd_head },
    { "help", "this list; -v shows descriptions", cmd_help },
    { "hexdump", "hex and ASCII dump", cmd_hexdump },
    { "ls", "list ramdisk or mounted files", cmd_ls },
    { "mem", "heap and core counters", cmd_mem },
    { "mkdir", "create a directory", cmd_mkdir },
    { "motd", "print the message of the day", cmd_motd },
    { "mount", "mount <dev> <dir> [fs]  or list mounts", cmd_mount },
    { "mv", "rename a file on one filesystem", cmd_mv },
    { "parts", "list partitions", cmd_parts },
    { "preempt", "prove timer preemption with two flows", cmd_preempt },
    { "procs", "run isolated user services (IPC/arena/time demo)", cmd_procs },
    { "abi", "run process ABI tests (fd/mmap/fork/exec/signal)", cmd_abi },

    { "ps", "list live flows", cmd_ps },
    { "pwd", "current directory", cmd_pwd },
    { "reboot", "reset the machine", cmd_reboot },
    { "rm", "remove a file or empty directory", cmd_rm },
    { "selfcheck", "run the 147-check core suite", cmd_selfcheck },
    { "services", "run isolated user services (IPC/arena demo)", cmd_services },
    { "shutdown", "stop the machine", cmd_shutdown },
    { "smp", "show per-CPU scheduler state", cmd_smp },
    { "stat", "show file size", cmd_stat },
    { "sync", "flush writable filesystems", cmd_sync },
    { "tail", "last lines of a file", cmd_tail },
    { "touch", "create an empty file", cmd_touch },
    { "tree", "recursive directory listing", cmd_tree },
    { "umount", "unmount a filesystem", cmd_umount },
    { "uname", "system identity", cmd_uname },
    { "uptime", "time since boot", cmd_uptime },
    { "user", "run init.elf in ring 3", cmd_user },
    { "version", "show the Finte version", cmd_version },
    { "watchdog", "show or test the 3-strike system watchdog", cmd_watchdog },
    { "write", "write <path> <text...>", cmd_write },
};

#define SH_COMMAND_COUNT FINTE_ARRAY_SIZE(sh_command_table)

static const sh_command_t *sh_lookup(const char *name)
{
    for (size_t i = 0; i < SH_COMMAND_COUNT; i++) {
        if (finte_strcmp(sh_command_table[i].name, name) == 0) {
            return &sh_command_table[i];
        }
    }
    return NULL;
}

static void sh_help_verbose(void)
{
    sh_puts(SH_BOLD_CYAN "commands:" SH_RESET "\n");
    for (size_t i = 0; i < SH_COMMAND_COUNT; i++) {
        const char *name = sh_command_table[i].name;
        int len = (int)finte_strlen(name);
        sh_puts("  ");
        sh_puts(name);
        int pad = 16 - len;
        while (pad-- > 0) {
            sh_puts(" ");
        }
        sh_puts(sh_command_table[i].desc);
        sh_puts("\n");
    }
}

static void sh_help_columns(void)
{
    int count = (int)SH_COMMAND_COUNT;
    int width = hal_console_cols();
    if (width < 24) {
        width = 80;
    }
    int field = 18;
    int ncols = (width - 2) / field;
    if (ncols < 1) {
        ncols = 1;
    } else if (ncols > 8) {
        ncols = 8;
    }
    int rows = (count + ncols - 1) / ncols;
    for (int r = 0; r < rows; r++) {
        char line[200];
        int used = 0;
        line[used++] = ' ';
        line[used++] = ' ';
        for (int c = 0; c < ncols; c++) {
            int idx = c * rows + r;
            if (idx >= count) {
                break;
            }
            const char *name = sh_command_table[idx].name;
            int len = (int)finte_strlen(name);
            if (used + len + 1 >= (int)sizeof(line)) {
                break;
            }
            finte_memcpy(line + used, name, (size_t)len);
            used += len;
            if (c + 1 < ncols && c * rows + rows + r < count) {
                int pad = field - len;
                while (pad-- > 0 && used + 1 < (int)sizeof(line)) {
                    line[used++] = ' ';
                }
            }
        }
        line[used] = 0;
        sh_puts(line);
        sh_puts("\n");
    }
}

static void cmd_help(const sh_ctx_t *ctx)
{
    const char *opt = sh_arg(ctx, 1);
    int verbose = opt && (finte_strcmp(opt, "-v") == 0 ||
                          finte_strcmp(opt, "--verbose") == 0 ||
                          finte_strcmp(opt, "long") == 0);
    if (verbose) {
        sh_help_verbose();
        return;
    }
    sh_puts(SH_BOLD_CYAN "commands:" SH_RESET "  " SH_BLUE "help -v" SH_RESET
            " for descriptions\n");
    sh_help_columns();
}

static char g_history[16][LINE_MAX];
static int g_history_count;
static int g_history_pos;

static void sh_redraw(const char *prompt, const char *line, int len, int pos)
{
    hal_console_clear_line();
    sh_puts_raw(prompt);
    for (int i = 0; i < len; i++) {
        char one[2] = { line[i], 0 };
        sh_puts_raw(one);
    }
    for (int i = pos; i < len; i++) {
        sh_puts_raw("\b");
    }
}

static void sh_history_add(const char *line)
{
    if (!line[0]) {
        return;
    }
    if (g_history_count > 0 &&
        finte_strcmp(g_history[g_history_count - 1], line) == 0) {
        return;
    }
    if (g_history_count == 16) {
        for (int i = 1; i < 16; i++) {
            finte_strlcpy(g_history[i - 1], g_history[i],
                          sizeof(g_history[i - 1]));
        }
        g_history_count--;
    }
    finte_strlcpy(g_history[g_history_count++], line,
                  sizeof(g_history[g_history_count]));
}

static void sh_complete(char *line, int *len, int *pos)
{
    for (int i = 0; i < *len; i++) {
        if (line[i] == ' ') {
            return;
        }
    }
    if (*len == 0) {
        return;
    }
    const char *match = NULL;
    int matches = 0;
    for (size_t i = 0; i < SH_COMMAND_COUNT; i++) {
        if (finte_strncmp(sh_command_table[i].name, line, (size_t)*len) == 0) {
            match = sh_command_table[i].name;
            matches++;
        }
    }
    if (matches == 1) {
        finte_strlcpy(line, match, LINE_MAX);
        *len = (int)finte_strlen(line);
        *pos = *len;
    }
}

static int sh_readline(const char *prompt, char *line, size_t cap)
{
    int len = 0;
    int pos = 0;
    g_history_pos = g_history_count;
    hal_console_clear_line();
    sh_puts_raw(prompt);
    for (;;) {
        int wheel = hal_console_trywheel();
        if (wheel != 0) {
            hal_console_scroll(-wheel * 3);
        }
        int c = hal_console_trykey();
        if (c < 0) {
            hal_idle(100000ull);
            continue;
        }
        int page = hal_console_rows() - 1;
        if (page < 1) {
            page = 1;
        }
        if (hal_console_scroll_offset() > 0) {
            if (c == KEY_UP || c == KEY_SHIFT_UP) {
                hal_console_scroll(-1);
                continue;
            }
            if (c == KEY_DOWN || c == KEY_SHIFT_DOWN) {
                hal_console_scroll(1);
                continue;
            }
            if (c == KEY_PGUP) {
                hal_console_scroll(-page);
                continue;
            }
            if (c == KEY_PGDN) {
                hal_console_scroll(page);
                continue;
            }
            if (c == KEY_HOME) {
                hal_console_scroll_top();
                continue;
            }
            if (c == KEY_END) {
                hal_console_scroll_bottom();
                continue;
            }
            /* Any other key returns to the live prompt first. */
            hal_console_scroll_bottom();
        } else {
            if (c == KEY_PGUP) {
                hal_console_scroll(-page);
                continue;
            }
            if (c == KEY_PGDN) {
                hal_console_scroll(page);
                continue;
            }
            if (c == KEY_SHIFT_UP) {
                hal_console_scroll(-1);
                continue;
            }
            if (c == KEY_SHIFT_DOWN) {
                hal_console_scroll(1);
                continue;
            }
        }
        if (c == '\r') {
            c = '\n';
        }
        if (c == '\n') {
            sh_puts_raw("\n");
            line[len] = 0;
            sh_history_add(line);
            return len;
        }
        if (c == 3) { /* Ctrl+C */
            sh_puts_raw("^C\n");
            return 0;
        }
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                if (pos == len) {
                    len--;
                    pos--;
                    line[len] = 0;
                    sh_puts_raw("\b \b");
                } else {
                    for (int i = pos - 1; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--;
                    pos--;
                    sh_redraw(prompt, line, len, pos);
                }
            }
            continue;
        }
        if (c == KEY_LEFT) {
            if (pos > 0) {
                pos--;
                sh_redraw(prompt, line, len, pos);
            }
            continue;
        }
        if (c == KEY_RIGHT) {
            if (pos < len) {
                pos++;
                sh_redraw(prompt, line, len, pos);
            }
            continue;
        }
        if (c == KEY_HOME) {
            pos = 0;
            sh_redraw(prompt, line, len, pos);
            continue;
        }
        if (c == KEY_END) {
            pos = len;
            sh_redraw(prompt, line, len, pos);
            continue;
        }
        if (c == KEY_DELETE) {
            if (pos < len) {
                for (int i = pos; i < len - 1; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                sh_redraw(prompt, line, len, pos);
            }
            continue;
        }
        if (c == KEY_UP) {
            if (g_history_count && g_history_pos > 0) {
                g_history_pos--;
                finte_strlcpy(line, g_history[g_history_pos], cap);
                len = (int)finte_strlen(line);
                pos = len;
                sh_redraw(prompt, line, len, pos);
            }
            continue;
        }
        if (c == KEY_DOWN) {
            if (g_history_pos + 1 < g_history_count) {
                g_history_pos++;
                finte_strlcpy(line, g_history[g_history_pos], cap);
                len = (int)finte_strlen(line);
                pos = len;
                sh_redraw(prompt, line, len, pos);
            } else if (g_history_pos + 1 == g_history_count) {
                g_history_pos = g_history_count;
                line[0] = 0;
                len = 0;
                pos = 0;
                sh_redraw(prompt, line, len, pos);
            }
            continue;
        }
        if (c == '\t') {
            sh_complete(line, &len, &pos);
            sh_redraw(prompt, line, len, pos);
            continue;
        }
        if (c >= 32 && c < 127 && len + 1 < (int)cap) {
            if (pos == len) {
                line[len++] = (char)c;
                pos++;
                char one[2] = { (char)c, 0 };
                sh_puts_raw(one);
            } else {
                for (int i = len; i > pos; i--) {
                    line[i] = line[i - 1];
                }
                line[pos] = (char)c;
                len++;
                pos++;
                sh_redraw(prompt, line, len, pos);
            }
        }
    }
}

static void run_line(char *line, const fboot_info_t *bi)
{
    char *argv[ARG_MAX];
    int argc = 0;
    char *p = line;
    while (*p && argc < ARG_MAX) {
        while (*p && is_space(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        while (*p && !is_space(*p)) {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    if (argc == 0) {
        return;
    }
    const char *cmd = argv[0];
    const sh_command_t *entry = sh_lookup(cmd);
    if (!entry) {
        sh_printf(SH_RED "unknown command: %s" SH_RESET "\n", cmd);
        sh_puts("type 'help' for commands\n");
        return;
    }
    sh_ctx_t ctx = { argc, argv, bi };
    entry->run(&ctx);
}

static void sh_boot_banner(void)
{
    sh_puts_raw("\n" SH_BOLD_CYAN
                "   ___  _         _         \n"
                "  / __\\(_) _ __  | |_   ___ \n"
                " / _\\  | || '_ \\ | __| / _ \\\n"
                "/ /    | || | | || |_ |  __/\n"
                "\\/     |_||_| |_| \\__| \\___|\n"
                SH_RESET "\n");
}

void finte_shell_run(const fboot_info_t *bi)
{
    hal_watchdog_clear();
    sh_boot_banner();
    sh_puts("Finte shell. Type 'help' for commands.\n");
    sh_puts("boot: ");
    if (bi && bi->magic == FINTE_BOOT_MAGIC) {
        sh_puts(bi->console_type == 2 ? "UEFI loader" : "custom BIOS loader");
    } else {
        sh_puts("Multiboot fallback");
    }
    sh_puts("\n");
    sh_printf("Finte (%s) %s\n", FINTE_NICKNAME, FINTE_VERSION_STRING);
    int idx = finte_initrd_find("motd.txt");
    if (idx >= 0) {
        hal_console_write(finte_initrd_data((uint32_t)idx),
                          finte_initrd_size((uint32_t)idx));
    }

    char line[LINE_MAX];
    static fflow_t shell_flow;
    finte_memset(&shell_flow, 0, sizeof(shell_flow));
    shell_flow.in_use = 1;
    shell_flow.state = FINTE_FLOW_RUNNING;
    finte_strlcpy(shell_flow.name, "shell", sizeof(shell_flow.name));
    g_sh_flow = &shell_flow;
    finte_strlcpy(g_cwd, "/", sizeof(g_cwd));

    for (;;) {
        int n = sh_readline(SH_CYAN "finte>" SH_RESET " ", line, sizeof(line));
        if (n > 0) {
            run_line(line, bi);
        }
    }
}
