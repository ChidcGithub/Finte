#include "finte/finte.h"
#include "finte/cas.h"
#include "finte/demo.h"
#include "finte/guest.h"
#include "finte/selftest.h"
#include "hal_host.h"

#include <stdio.h>
#include <string.h>

int finte_bench_run(void);
int finte_tests_run(void);

static void print_limits(void)
{
    printf("Finte %s\n", FINTE_VERSION_STRING);
    printf("  model      nodes + capabilities + finite flows\n");
    printf("  limits     nodes=%u flows=%u channels=%u caps/flow=%u\n",
           FINTE_MAX_NODES, FINTE_MAX_FLOWS, FINTE_MAX_CHANNELS, FINTE_MAX_CAPS);
    printf("  message    %u bytes inline, %u attached caps\n",
           FINTE_MSG_DATA, FINTE_MSG_CAPS);
}

static void print_stats(const fstats_t *s)
{
    printf("  stats      syscalls=%llu flows=%llu/%llu switches=%llu\n",
           (unsigned long long)s->syscalls,
           (unsigned long long)s->flows_finished,
           (unsigned long long)s->flows_started,
           (unsigned long long)s->context_switches);
    printf("             msgs=%llu/%llu caps=%llu/%llu denied=%llu fuel=%llu\n",
           (unsigned long long)s->messages_sent,
           (unsigned long long)s->messages_received,
           (unsigned long long)s->caps_installed,
           (unsigned long long)s->caps_dropped,
           (unsigned long long)s->caps_denied,
           (unsigned long long)s->fuel_burned);
    printf("             nodes=%u channels=%u arenas=%u epochs=%llu deadlocks=%llu\n",
           s->nodes_live, s->channels_live, s->arenas_live,
           (unsigned long long)s->epochs,
           (unsigned long long)s->deadlocks);
}

static int cmd_demo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    finte_init();
    printf("\nFinte %s | native pipeline demo\n\n", FINTE_VERSION_STRING);
    fret_t r = finte_demo_bootstrap();
    if (finte_failed(r)) {
        printf("bootstrap failed: %s\n", finte_strstatus(r.status));
        return 1;
    }
    uint64_t t0 = hal_now_ns();
    finte_run();
    uint64_t dt = hal_now_ns() - t0;
    const fdemo_result_t *d = finte_demo_result();
    fstats_t s = finte_get_stats();

    printf("\n");
    print_stats(&s);
    printf("  demo       produced=%llu transformed=%llu consumed=%llu\n",
           (unsigned long long)d->produced,
           (unsigned long long)d->transformed,
           (unsigned long long)d->consumed);
    printf("             checksum=%llu expected=%llu authority-denied=%llu\n",
           (unsigned long long)d->checksum,
           (unsigned long long)d->expected_checksum,
           (unsigned long long)d->denied);
    if (d->denied_status[0])
        printf("             denied statuses: producer=%s transform=%s\n",
               finte_strstatus(d->denied_status[0]),
               finte_strstatus(d->denied_status[1]));
    printf("  wall       %.3f ms, %.0f ns/message\n",
           (double)dt / 1e6,
           s.messages_sent ? (double)dt / (double)s.messages_sent : 0.0);
    printf("  result     %s\n\n",
           d->ok && finte_run_status() == FINTE_OK ? "OK" : "FAILED");
    return d->ok && finte_run_status() == FINTE_OK ? 0 : 1;
}

static int cmd_compat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    finte_init();
    printf("\nFinte %s | Linux x86_64 persona demo\n\n", FINTE_VERSION_STRING);

    fret_t br = finte_compat_bootstrap();
    if (finte_failed(br)) {
        printf("persona bootstrap failed: %s\n", finte_strstatus(br.status));
        return 1;
    }

    uint64_t t0 = hal_now_ns();
    finte_run();
    uint64_t dt = hal_now_ns() - t0;
    const fguest_result_t *gr = finte_guest_result();
    fstats_t s = finte_get_stats();

    printf("\n");
    print_stats(&s);
    printf("  persona    ok=%d read=%lld fd=%lld pid=%lld brk=0x%llx->0x%llx\n",
           gr->ok, (long long)gr->bytes_read, (long long)gr->fd,
           (long long)gr->pid, (unsigned long long)gr->brk_before,
           (unsigned long long)gr->brk_after);
    printf("             uname=%s/%s clock=%llu ns\n", gr->sysname, gr->machine,
           (unsigned long long)gr->monotonic_ns);
    printf("  wall       %.3f ms\n", (double)dt / 1e6);
    printf("  result     %s\n\n",
           gr->ok && finte_run_status() == FINTE_OK ? "OK" : "FAILED");
    return gr->ok && finte_run_status() == FINTE_OK ? 0 : 1;
}

static int cmd_selfcheck(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    finte_init();
    printf("\nFinte %s | startup self-check\n\n", FINTE_VERSION_STRING);
    fselftest_report_t report;
    int rc = finte_selftest_run(2, &report);
    printf("\n  result     %s (%u sections, %u checks, %u failures)\n\n",
           rc == 0 ? "OK" : "FAILED", report.sections, report.checks,
           report.failures);
    return rc == 0 ? 0 : 1;
}

static int cmd_inspect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    finte_init();
    fhandle_t ch = finte_channel_create("inspect-chan", 8);
    fhandle_t ar = finte_arena_create("inspect-arena", 64 * 1024);
    fhandle_t file = finte_file_create("notes.txt", "finte", 5);
    fhandle_t dir = finte_dir_create("/");
    if (file && dir) {
        finte_dir_add(dir, "notes.txt", file);
    }
    print_limits();
    printf("\n");
    finte_dump("core");
    printf("  handles     channel=%llu arena=%llu file=%llu dir=%llu\n",
           (unsigned long long)ch, (unsigned long long)ar,
           (unsigned long long)file, (unsigned long long)dir);
    printf("\n");
    return 0;
}

static void disk_dirent(void *ctx, const char *name, const fdfs_stat_t *st)
{
    (void)ctx;
    if (st->mode == FDISK_MODE_DIR) {
        printf("  d %s/\n", name);
    } else {
        printf("  f %s (%llu bytes)\n", name,
               (unsigned long long)st->size);
    }
}

static const char *disk_source(const char *arg, char *buf, size_t cap)
{
    if (strncmp(arg, "hd", 2) == 0) {
        return arg;
    }
    snprintf(buf, cap, "hd0p%s", arg);
    return buf;
}

static void disk_size(uint64_t bytes)
{
    printf("%llu bytes", (unsigned long long)bytes);
    if (bytes >= 1024ull * 1024ull) {
        printf(" (%.1f MiB)", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        printf(" (%.1f KiB)", (double)bytes / 1024.0);
    }
}

static void disk_list(void)
{
    int n = finte_disk_count();
    if (n == 0) {
        printf("no block devices\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        const fdisk_dev_t *d = finte_disk_get(i);
        if (!d) {
            continue;
        }
        printf("%s  ", d->name);
        disk_size(d->sectors * d->sector_size);
        printf("  sector=%u  scheme=%s\n", d->sector_size,
               d->scheme == 2 ? "GPT" : d->scheme == 1 ? "MBR" : "none");
        for (int j = 0; j < d->part_count; j++) {
            const fdisk_part_t *p = &d->parts[j];
            char dev[24];
            if (p->number) {
                snprintf(dev, sizeof(dev), "%sp%u", d->name, p->number);
            } else {
                snprintf(dev, sizeof(dev), "%s", d->name);
            }
            printf("  %-8s ", dev);
            disk_size(p->sectors * d->sector_size);
            printf("  start=%llu  type=%s",
                   (unsigned long long)p->start,
                   p->type_name[0] ? p->type_name : "?");
            if (p->label[0]) {
                printf("  label=%s", p->label);
            }
            printf("\n");
        }
    }
}

static int disk_mount(const char *part)
{
    char sbuf[FDISK_NAME_MAX];
    const char *src = disk_source(part, sbuf, sizeof(sbuf));
    int rc = finte_disk_mount(src, "/mnt", NULL);
    if (rc != 0) {
        printf("mount %s: %s\n", src, finte_disk_error(rc));
        return rc;
    }
    return 0;
}

static int cmd_disk(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: finte disk <image> list\n");
        printf("       finte disk <image> ls|cat|stat|info <partition> [path]\n");
        return 1;
    }
    if (hal_blk_add_image(argv[2]) != 0) {
        printf("cannot open image: %s\n", argv[2]);
        return 1;
    }
    finte_disk_rescan();
    const char *op = argv[3];
    if (strcmp(op, "list") == 0) {
        disk_list();
        return 0;
    }
    if (argc < 5) {
        printf("missing partition\n");
        return 1;
    }
    if (disk_mount(argv[4]) != 0) {
        return 1;
    }
    const char *rel = argc > 5 ? argv[5] : "";
    char path[FDISK_PATH_MAX];
    snprintf(path, sizeof(path), "/mnt/%s", rel);
    for (char *q = path; *q; q++) {
        if (*q == '\\') {
            *q = '/';
        }
    }
    if (strcmp(op, "mkdir") == 0) {
        int rc = finte_disk_create(path, 1);
        if (rc != 0) {
            printf("mkdir: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        finte_disk_sync();
        return 0;
    }
    if (strcmp(op, "write") == 0) {
        if (argc < 7) {
            printf("write: missing text\n");
            return 1;
        }
        fhandle_t fh;
        int rc = finte_disk_file_open(path, FINTE_OPEN_WRITE |
                                      FINTE_OPEN_CREATE | FINTE_OPEN_TRUNC,
                                      &fh);
        if (rc != 0) {
            printf("write: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        int64_t n = finte_file_handle_write(fh, argv[6], strlen(argv[6]));
        finte_file_handle_close(fh);
        if (n < 0) {
            printf("write: %s (%d)\n", finte_disk_error((int)n), (int)n);
            return 1;
        }
        rc = finte_disk_sync();
        if (rc != 0) {
            printf("sync: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        return 0;
    }
    if (strcmp(op, "rm") == 0) {
        int rc = finte_disk_unlink(path);
        if (rc != 0) {
            printf("rm: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        finte_disk_sync();
        return 0;
    }
    if (strcmp(op, "sync") == 0) {
        int rc = finte_disk_sync();
        if (rc != 0) {
            printf("sync: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        return 0;
    }
    if (strcmp(op, "ls") == 0) {
        int rc = finte_disk_ls(path, disk_dirent, NULL);
        if (rc != 0) {
            printf("ls: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        return 0;
    }
    if (strcmp(op, "stat") == 0) {
        fdfs_stat_t st;
        int rc = finte_disk_stat(path, &st);
        if (rc != 0) {
            printf("stat: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        printf("%s: %s, %llu bytes\n", rel,
               st.mode == FDISK_MODE_DIR ? "directory" : "file",
               (unsigned long long)st.size);
        return 0;
    }
    if (strcmp(op, "info") == 0) {
        fdfs_info_t info;
        int rc = finte_disk_info(path, &info);
        if (rc != 0) {
            printf("info: %s\n", finte_disk_error(rc));
            return 1;
        }
        printf("fstype=%s label=%s block=%u\n", info.fstype, info.label,
               info.block_size);
        disk_size(info.total_bytes);
        printf("\n");
        return 0;
    }
    if (strcmp(op, "cat") == 0) {
        fhandle_t fh;
        int rc = finte_disk_file_open(path, FINTE_OPEN_READ, &fh);
        if (rc != 0) {
            printf("cat: %s (%d)\n", finte_disk_error(rc), rc);
            return 1;
        }
        uint8_t buf[512];
        for (;;) {
            int64_t n = finte_file_handle_read(fh, buf, sizeof(buf));
            if (n < 0) {
                printf("cat: %s (%d)\n", finte_disk_error((int)n), (int)n);
                finte_file_handle_close(fh);
                return 1;
            }
            if (n == 0) {
                break;
            }
            fwrite(buf, 1, (size_t)n, stdout);
        }
        finte_file_handle_close(fh);
        return 0;
    }
    printf("unknown disk op: %s\n", op);
    return 1;
}

static int join_host_words(int argc, char **argv, int start, int end,
                           char *buf, size_t cap)
{
    (void)argc;
    size_t used = 0;
    if (start >= end) {
        return -1;
    }
    buf[0] = 0;
    for (int i = start; i < end; i++) {
        size_t n = strlen(argv[i]);
        if (used + n + (i > start ? 1u : 0u) + 1 > cap) {
            return -1;
        }
        if (i > start) {
            buf[used++] = ' ';
        }
        memcpy(buf + used, argv[i], n);
        used += n;
        buf[used] = 0;
    }
    return 0;
}

static int parse_host_rational(const char *s, int64_t *on, int64_t *od)
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
    }
    if (*s || d == 0 || n > (uint64_t)INT64_MAX ||
        d > (uint64_t)INT64_MAX) {
        return -1;
    }
    *on = neg ? -(int64_t)n : (int64_t)n;
    *od = (int64_t)d;
    return 0;
}

static int cmd_cas(int argc, char **argv)
{
    char expr[512];
    char out[512];
    char err[128];
    fcas_poly_t poly;
    fcas_poly_t result;
    if (argc < 3) {
        printf("usage: finte cas <expr>\n");
        printf("       finte cas diff <expr>\n");
        printf("       finte cas eval <expr> <x>\n");
        return 1;
    }
    const char *mode = argv[2];
    int rc;
    if (strcmp(mode, "diff") == 0) {
        if (argc < 4 || join_host_words(argc, argv, 3, argc, expr,
                                        sizeof(expr)) != 0) {
            printf("cas: expression too long\n");
            return 1;
        }
        rc = fcas_parse(expr, &poly, err, sizeof(err));
        if (rc == 0) {
            rc = fcas_diff(&poly, &result);
        }
        if (rc != 0) {
            printf("cas: %s\n", err[0] ? err : "cannot differentiate");
            return 1;
        }
    } else if (strcmp(mode, "eval") == 0) {
        if (argc < 5 ||
            join_host_words(argc, argv, 3, argc - 1, expr,
                            sizeof(expr)) != 0) {
            printf("usage: finte cas eval <expr> <x>\n");
            return 1;
        }
        int64_t xn = 0;
        int64_t xd = 1;
        if (parse_host_rational(argv[argc - 1], &xn, &xd) != 0) {
            printf("cas: invalid x value\n");
            return 1;
        }
        rc = fcas_parse(expr, &poly, err, sizeof(err));
        if (rc == 0) {
            rc = fcas_eval(&poly, xn, xd, &xn, &xd);
        }
        if (rc != 0) {
            printf("cas: %s\n", err[0] ? err : "cannot evaluate");
            return 1;
        }
        if (xd == 1) {
            printf("%lld\n", (long long)xn);
        } else {
            printf("%lld/%lld\n", (long long)xn, (long long)xd);
        }
        return 0;
    } else {
        if (join_host_words(argc, argv, 2, argc, expr, sizeof(expr)) != 0) {
            printf("cas: expression too long\n");
            return 1;
        }
        rc = fcas_parse(expr, &poly, err, sizeof(err));
        if (rc != 0) {
            printf("cas: %s\n", err);
            return 1;
        }
        result = poly;
    }
    if (fcas_format(&result, out, sizeof(out)) != 0) {
        printf("cas: result too long\n");
        return 1;
    }
    printf("%s\n", out);
    return 0;
}

typedef struct host_command {
    const char *name;
    const char *desc;
    int (*run)(int argc, char **argv);
} host_command_t;

static int cmd_bench(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return finte_bench_run();
}

static int cmd_test(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return finte_tests_run();
}

static int cmd_version(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Finte %s\n", FINTE_VERSION_STRING);
    return 0;
}

static int cmd_help(int argc, char **argv);
static void usage(void);

/* Command, description and entry point live in one place. */
static const host_command_t host_commands[] = {
    { "bench", "microbenchmarks (syscall, caps, channels, arena)", cmd_bench },
    { "cas", "native polynomial CAS (simplify/diff/eval)", cmd_cas },
    { "compat", "run the Linux x86_64 syscall persona", cmd_compat },
    { "demo", "run the native producer/transform/sink pipeline", cmd_demo },
    { "disk", "inspect/mount a raw disk image", cmd_disk },
    { "help", "this text", cmd_help },
    { "inspect", "show limits and object tables", cmd_inspect },
    { "selfcheck", "run the startup POST/self-check suite", cmd_selfcheck },
    { "test", "run the extended core self-tests", cmd_test },
    { "version", "print the version", cmd_version },
};

static const host_command_t *host_lookup(const char *name)
{
    for (size_t i = 0; i < FINTE_ARRAY_SIZE(host_commands); i++) {
        if (strcmp(host_commands[i].name, name) == 0) {
            return &host_commands[i];
        }
    }
    return NULL;
}

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    usage();
    return 0;
}

static void usage(void)
{
    printf("Finte %s - a capability core with finite flows\n\n",
           FINTE_VERSION_STRING);
    printf("usage: finte <command>\n\n");
    for (size_t i = 0; i < FINTE_ARRAY_SIZE(host_commands); i++) {
        printf("  %-9s %s\n", host_commands[i].name,
               host_commands[i].desc);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    hal_init();
    if (argc < 2) {
        usage();
        return 0;
    }
    const char *name = argv[1];
    if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
        name = "help";
    }
    const host_command_t *cmd = host_lookup(name);
    if (!cmd) {
        fprintf(stderr, "unknown command: %s\n", name);
        usage();
        return 1;
    }
    return cmd->run(argc, argv);
}
