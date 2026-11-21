#include "finte/finte.h"
#include "finte/selftest.h"

static int st_verbose;
static int st_checks;
static int st_fails;
static char st_failed_section[FINTE_NAME_MAX];

static int st_noop_count;
static uint64_t st_sum;
static int st_yields;
static int st_target_done;
static int st_wait_done;

static void st_print(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        hal_console_write(buf, (size_t)n);
    }
}

static void st_check(int ok, const char *expr, int line)
{
    st_checks++;
    if (!ok) {
        st_fails++;
        if (st_verbose < 2) {
            st_print("[post]   FAIL line %d: %s\n", line, expr);
        }
    }
    if (st_verbose >= 2) {
        char text[96];
        int n = finte_format(text, sizeof(text), "#%03d %s: ", st_checks,
                             ok ? "ok" : "FAIL");
        if (n < 0) {
            n = 0;
        }
        if (n > (int)sizeof(text) - 48) {
            n = (int)sizeof(text) - 48;
        }
        size_t used = (size_t)n;
        size_t elen = finte_strlen(expr);
        /* Leave room for the timestamp prefix (15 chars) on an 80-column
         * console, so each check stays on one physical line. */
        size_t avail = used < 64 ? 64 - used : 0;
        if (elen > avail) {
            size_t keep = avail > 3 ? avail - 3 : 0;
            finte_memcpy(text + used, expr, keep);
            used += keep;
            text[used++] = '.';
            text[used++] = '.';
        } else {
            finte_memcpy(text + used, expr, elen);
            used += elen;
        }
        text[used++] = '\n';
        text[used] = '\0';
        hal_console_write(text, used);
    }
}

#define ST(cond) st_check(!!(cond), #cond, __LINE__)

static void st_run(const char *name, void (*fn)(void))
{
    finte_reset();
    if (st_verbose >= 2) {
        st_print("[post] %s\n", name);
    }
    int c0 = st_checks;
    int f0 = st_fails;
    fn();
    int checks = st_checks - c0;
    int fails = st_fails - f0;
    if (fails && st_failed_section[0] == '\0') {
        finte_strlcpy(st_failed_section, name, sizeof(st_failed_section));
    }
    if (st_verbose >= 1) {
        st_print("[post] %s: %s (%d checks, %d failures)\n", name,
                 fails ? "FAIL" : "ok", checks, fails);
    }
}

static void st_noop(fflow_t *f)
{
    (void)f;
    st_noop_count++;
}

static void st_produce(fflow_t *f)
{
    for (uint64_t i = 0; i < 5; i++) {
        fx_send(f, 0, 1, &i, sizeof(i));
    }
}

static void st_consume(fflow_t *f)
{
    for (int i = 0; i < 5; i++) {
        fmsg_t m;
        if (finte_failed(fx_recv(f, 0, &m))) {
            return;
        }
        uint64_t v = 0;
        finte_memcpy(&v, m.data, sizeof(v));
        st_sum += v;
    }
}

static void st_yielder(fflow_t *f)
{
    for (int i = 0; i < 3; i++) {
        st_yields++;
        fx_yield(f);
    }
}

static void st_target(fflow_t *f)
{
    fx_yield(f);
    fx_yield(f);
    st_target_done = 1;
}

static void st_waiter(fflow_t *f)
{
    fx_flow_wait(f, 0);
    st_wait_done = 1;
}

static void st_blocked(fflow_t *f)
{
    fmsg_t m;
    fx_recv(f, 0, &m);
}

static const fprogram_t st_p_noop = { "st-noop", st_noop, 4096 };
static const fprogram_t st_p_produce = { "st-produce", st_produce, 4096 };
static const fprogram_t st_p_consume = { "st-consume", st_consume, 4096 };
static const fprogram_t st_p_yielder = { "st-yielder", st_yielder, 24 };
static const fprogram_t st_p_target = { "st-target", st_target, 4096 };
static const fprogram_t st_p_waiter = { "st-waiter", st_waiter, 4096 };
static const fprogram_t st_p_blocked = { "st-blocked", st_blocked, 1024 };

static void st_objects(void)
{
    fhandle_t ch = finte_channel_create("st-ch", 4);
    ST(ch != FINTE_HANDLE_NONE);
    ST(finte_node_type(ch) == FINTE_NODE_CHANNEL);

    fflow_t *holder = finte_flow_lookup(
        finte_flow_create("st-holder", NULL, 1024, FINTE_PERSONA_NATIVE));
    ST(holder != NULL);
    if (!holder) {
        return;
    }
    int cap = finte_cap_install(holder, ch, FINTE_R_SEND | FINTE_R_RECV);
    ST(cap == 0);
    ST(finte_node_refs(ch) >= 1);
    ST(finte_cap_drop_index(holder, (uint16_t)cap) == 0);
    ST(finte_node_type(ch) == FINTE_NODE_FREE);

    fhandle_t ch2 = finte_channel_create("st-ch2", 4);
    ST(ch2 != FINTE_HANDLE_NONE);
    ST(ch2 != ch);
    ST(finte_node_type(ch) == FINTE_NODE_FREE);

    fhandle_t ar = finte_arena_create("st-arena", 64 * 1024);
    fhandle_t file = finte_file_create("st-file", "abc", 3);
    fhandle_t dir = finte_dir_create("/");
    ST(ar != FINTE_HANDLE_NONE);
    ST(file != FINTE_HANDLE_NONE);
    ST(dir != FINTE_HANDLE_NONE);
    ST(finte_node_type(ar) == FINTE_NODE_ARENA);
    ST(finte_node_type(file) == FINTE_NODE_FILE);
    ST(finte_node_type(dir) == FINTE_NODE_DIR);
    if (file && dir) {
        ST(finte_dir_add(dir, "st-file", file).status == FINTE_OK);
        ST(finte_node_refs(file) >= 1);
    }
}

static void st_caps(void)
{
    fhandle_t console = finte_console_create("st-console");
    fflow_t *sender = finte_flow_lookup(
        finte_flow_create("st-sender", NULL, 1024, FINTE_PERSONA_NATIVE));
    fflow_t *receiver = finte_flow_lookup(
        finte_flow_create("st-receiver", NULL, 1024, FINTE_PERSONA_NATIVE));
    ST(console != FINTE_HANDLE_NONE);
    ST(sender != NULL);
    ST(receiver != NULL);
    if (!sender || !receiver) {
        return;
    }

    int rd = finte_cap_install(sender, console, FINTE_R_READ);
    int wr = finte_cap_install(sender, console, FINTE_R_WRITE);
    ST(rd == 0);
    ST(wr == 1);
    ST(fx_console_write(sender, (uint16_t)rd, "", 0).status == FINTE_ERR_PERM);
    ST(fx_console_write(sender, (uint16_t)wr, "", 0).status == FINTE_OK);

    fcap_info_t info;
    ST(fx_cap_info(sender, (uint16_t)wr, &info).status == FINTE_OK);
    ST(info.type == FINTE_NODE_CONSOLE);
    ST(info.rights == FINTE_R_WRITE);

    int flow_cap = finte_cap_install(sender, receiver->self,
                                     FINTE_R_GRANT | FINTE_R_CONTROL);
    ST(flow_cap >= 0);
    ST(fx_cap_grant(sender, (uint16_t)flow_cap, (uint16_t)rd,
                    FINTE_R_READ).status == FINTE_OK);
    ST(receiver->caps[0].rights == FINTE_R_READ);
    ST(fx_console_write(receiver, 0, "", 0).status == FINTE_ERR_PERM);
    ST(fx_cap_grant(sender, (uint16_t)flow_cap, (uint16_t)rd,
                    FINTE_R_READ | FINTE_R_WRITE).status == FINTE_ERR_PERM);
    ST(fx_cap_dup(sender, (uint16_t)wr, FINTE_R_WRITE).status >= 0);
}

static void st_channels(void)
{
    fhandle_t ch = finte_channel_create("st-fifo", 4);
    fflow_t *tx = finte_flow_lookup(
        finte_flow_create("st-tx", NULL, 1024, FINTE_PERSONA_NATIVE));
    fflow_t *rx = finte_flow_lookup(
        finte_flow_create("st-rx", NULL, 1024, FINTE_PERSONA_NATIVE));
    ST(ch != FINTE_HANDLE_NONE);
    ST(tx != NULL);
    ST(rx != NULL);
    if (!ch || !tx || !rx) {
        return;
    }
    ST(finte_cap_install(tx, ch, FINTE_R_SEND) == 0);
    ST(finte_cap_install(rx, ch, FINTE_R_RECV) == 0);

    uint32_t v = 7;
    ST(fx_try_send(tx, 0, 7, &v, sizeof(v)).status == FINTE_OK);
    fmsg_t m;
    ST(fx_try_recv(rx, 0, &m).status == FINTE_OK);
    ST(m.tag == 7);
    ST(fx_try_recv(rx, 0, &m).status == FINTE_ERR_AGAIN);

    for (uint32_t i = 0; i < 4; i++) {
        ST(fx_try_send(tx, 0, i, &i, sizeof(i)).status == FINTE_OK);
    }
    ST(fx_try_send(tx, 0, 99, &v, sizeof(v)).status == FINTE_ERR_AGAIN);
    for (uint32_t i = 0; i < 4; i++) {
        ST(fx_try_recv(rx, 0, &m).status == FINTE_OK);
    }

    fhandle_t file = finte_file_create("st-cap-file", "CAP", 3);
    ST(file != FINTE_HANDLE_NONE);
    int file_cap = finte_cap_install(tx, file, FINTE_R_READ);
    ST(file_cap >= 0);
    fmsg_t cm;
    finte_memset(&cm, 0, sizeof(cm));
    cm.tag = 0xCAFE;
    cm.cap_count = 1;
    cm.caps[0] = (uint16_t)file_cap;
    ST(fx_send_msg(tx, 0, &cm, 0).status == FINTE_OK);
    ST(fx_recv_msg(rx, 0, &cm, 0).status == FINTE_OK);
    ST(cm.cap_count == 1);
    char tmp[4];
    finte_memset(tmp, 0, sizeof(tmp));
    ST(fx_file_read(rx, cm.caps[0], 0, tmp, 3).status == FINTE_OK);
    ST(finte_memcmp(tmp, "CAP", 3) == 0);

    fhandle_t chc = finte_channel_create("st-close", 4);
    fflow_t *cf = finte_flow_lookup(
        finte_flow_create("st-close-owner", NULL, 1024, FINTE_PERSONA_NATIVE));
    ST(chc != FINTE_HANDLE_NONE);
    ST(cf != NULL);
    if (cf) {
        ST(finte_cap_install(cf, chc, FINTE_R_SEND | FINTE_R_RECV |
                                         FINTE_R_CONTROL) == 0);
        uint32_t cv = 1;
        ST(fx_try_send(cf, 0, 1, &cv, sizeof(cv)).status == FINTE_OK);
        ST(fx_try_send(cf, 0, 2, &cv, sizeof(cv)).status == FINTE_OK);
        fchan_info_t ci;
        ST(fx_chan_info(cf, 0, &ci).status == FINTE_OK);
        ST(ci.closed == 0);
        ST(fx_chan_close(cf, 0).status == FINTE_OK);
        ST(fx_chan_info(cf, 0, &ci).status == FINTE_OK);
        ST(ci.closed == 1);
        fmsg_t cm2;
        ST(fx_try_recv(cf, 0, &cm2).status == FINTE_OK);
        ST(fx_try_recv(cf, 0, &cm2).status == FINTE_OK);
        ST(fx_try_recv(cf, 0, &cm2).status == FINTE_ERR_EOF);
        ST(fx_try_send(cf, 0, 3, &cv, sizeof(cv)).status == FINTE_ERR_STATE);
    }

    fhandle_t ch2 = finte_channel_create("st-block", 2);
    fflow_t *p = finte_flow_lookup(
        finte_flow_create("st-p", &st_p_produce, 4096, FINTE_PERSONA_NATIVE));
    fflow_t *c = finte_flow_lookup(
        finte_flow_create("st-c", &st_p_consume, 4096, FINTE_PERSONA_NATIVE));
    ST(ch2 != FINTE_HANDLE_NONE);
    ST(p != NULL);
    ST(c != NULL);
    if (p && c) {
        ST(finte_cap_install(p, ch2, FINTE_R_SEND) == 0);
        ST(finte_cap_install(c, ch2, FINTE_R_RECV) == 0);
        st_sum = 0;
        ST(finte_flow_start(finte_flow_handle(p)).status == FINTE_OK);
        ST(finte_flow_start(finte_flow_handle(c)).status == FINTE_OK);
        finte_run();
        ST(finte_run_status() == FINTE_OK);
        ST(st_sum == 10);
    }
}

static void st_arena(void)
{
    fhandle_t ar = finte_arena_create("st-arena2", 4096);
    fflow_t *f = finte_flow_lookup(
        finte_flow_create("st-arena-user", NULL, 1024, FINTE_PERSONA_NATIVE));
    ST(ar != FINTE_HANDLE_NONE);
    ST(f != NULL);
    if (!ar || !f) {
        return;
    }
    int cap = finte_cap_install(f, ar, FINTE_R_WRITE);
    ST(cap == 0);

    uint64_t o1 = 0;
    uint64_t o2 = 0;
    uint64_t o3 = 0;
    ST(fx_arena_alloc(f, 0, 100, 16, &o1).status == FINTE_OK);
    ST(fx_arena_alloc(f, 0, 200, 16, &o2).status == FINTE_OK);
    ST(o2 > o1);
    fnode_t *n = finte_node_get(ar);
    ST(n != NULL);
    if (n) {
        farena_t *a = &finte_core.arenas[n->index];
        ST(a->live_bytes >= 300);
        *(uint64_t *)(a->base + o1) = 0x12345678u;
        ST(*(uint64_t *)(a->base + o1) == 0x12345678u);
        ST(fx_arena_free(f, 0, o1).status == FINTE_OK);
        ST(fx_arena_free(f, 0, o2).status == FINTE_OK);
        ST(a->live_bytes == 0);
        ST(fx_arena_free(f, 0, o1).status == FINTE_ERR_INVAL);
        ST(fx_arena_alloc(f, 0, 4000, 16, &o3).status == FINTE_OK);
        ST(fx_arena_free(f, 0, o3).status == FINTE_OK);
    }
    ST(fx_arena_alloc(f, 0, 5000, 16, &o3).status == FINTE_ERR_NOMEM);
    ST(fx_arena_alloc(f, 0, 64, 32, &o3).status == FINTE_ERR_INVAL);
}

static void st_scheduler(void)
{
    st_noop_count = 0;
    for (int i = 0; i < 4; i++) {
        fhandle_t h = finte_flow_create("st-run", &st_p_noop, 4096,
                                        FINTE_PERSONA_NATIVE);
        ST(h != FINTE_HANDLE_NONE);
        ST(finte_flow_start(h).status == FINTE_OK);
    }
    finte_run();
    ST(finte_run_status() == FINTE_OK);
    ST(st_noop_count == 4);

    st_yields = 0;
    fhandle_t yh = finte_flow_create("st-yield", &st_p_yielder, 24,
                                     FINTE_PERSONA_NATIVE);
    ST(yh != FINTE_HANDLE_NONE);
    ST(finte_flow_start(yh).status == FINTE_OK);
    finte_run();
    ST(finte_run_status() == FINTE_OK);
    ST(st_yields == 3);
    ST(finte_get_stats().epochs >= 1);

    st_target_done = 0;
    st_wait_done = 0;
    fhandle_t th = finte_flow_create("st-target", &st_p_target, 4096,
                                     FINTE_PERSONA_NATIVE);
    fflow_t *target = finte_flow_lookup(th);
    fhandle_t wh = finte_flow_create("st-waiter", &st_p_waiter, 4096,
                                     FINTE_PERSONA_NATIVE);
    fflow_t *waiter = finte_flow_lookup(wh);
    ST(target != NULL);
    ST(waiter != NULL);
    if (target && waiter) {
        ST(finte_cap_install(waiter, target->self, FINTE_R_CONTROL) == 0);
        ST(finte_flow_start(th).status == FINTE_OK);
        ST(finte_flow_start(wh).status == FINTE_OK);
        finte_run();
        ST(finte_run_status() == FINTE_OK);
        ST(st_target_done == 1);
        ST(st_wait_done == 1);
    }

    fhandle_t ch = finte_channel_create("st-empty", 2);
    fhandle_t bh = finte_flow_create("st-blocked", &st_p_blocked, 1024,
                                     FINTE_PERSONA_NATIVE);
    fflow_t *b = finte_flow_lookup(bh);
    ST(ch != FINTE_HANDLE_NONE);
    ST(b != NULL);
    if (b) {
        ST(finte_cap_install(b, ch, FINTE_R_RECV) == 0);
        ST(finte_flow_start(bh).status == FINTE_OK);
        finte_run();
        ST(finte_run_status() == FINTE_ERR_DEADLOCK);
        ST(finte_get_stats().deadlocks >= 1);
    }
}

static void st_persona(void)
{
    static const char text[] = "hello";
    fhandle_t console = finte_console_create("st-console");
    fhandle_t dir = finte_dir_create("/");
    fhandle_t file = finte_file_create("hello.txt", text, sizeof(text) - 1);
    fhandle_t arena = finte_arena_create("st-heap", 2 * 1024 * 1024);
    ST(console != FINTE_HANDLE_NONE);
    ST(dir != FINTE_HANDLE_NONE);
    ST(file != FINTE_HANDLE_NONE);
    ST(arena != FINTE_HANDLE_NONE);
    ST(finte_dir_add(dir, "hello.txt", file).status == FINTE_OK);

    fflow_t *g = finte_flow_lookup(
        finte_flow_create("st-guest", NULL, 4096, FINTE_PERSONA_LINUX));
    ST(g != NULL);
    if (!g) {
        return;
    }
    int cc = finte_cap_install(g, console, FINTE_R_WRITE | FINTE_R_READ);
    int dc = finte_cap_install(g, dir, FINTE_R_READ);
    int ac = finte_cap_install(g, arena, FINTE_R_WRITE);
    ST(cc == 0);
    ST(dc == 1);
    ST(ac == 2);
    ST(finte_linux_bootstrap(g, (uint16_t)cc, (uint16_t)dc,
                             (uint16_t)ac).status == FINTE_OK);

    uint64_t args[6] = { (uint64_t)(int64_t)FINTE_AT_FDCWD,
                         (uint64_t)(uintptr_t)"/hello.txt",
                         FINTE_O_RDONLY, 0, 0, 0 };
    int64_t fd = finte_linux_call(g, FINTE_NR_openat, args);
    ST(fd == 3);

    char buf[16];
    finte_memset(buf, 0, sizeof(buf));
    uint64_t rargs[6] = { (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                          sizeof(buf) - 1, 0, 0, 0 };
    int64_t n = finte_linux_call(g, FINTE_NR_read, rargs);
    ST(n == (int64_t)(sizeof(text) - 1));
    ST(finte_memcmp(buf, text, sizeof(text) - 1) == 0);

    flinux_utsname_t u;
    uint64_t uargs[6] = { (uint64_t)(uintptr_t)&u, 0, 0, 0, 0, 0 };
    ST(finte_linux_call(g, FINTE_NR_uname, uargs) == 0);
    ST(finte_strcmp(u.sysname, "Linux") == 0);
    ST(finte_strcmp(u.machine, FINTE_ARCH_NAME) == 0);

    uint64_t margs[6] = { 0, 4096, FINTE_PROT_READ | FINTE_PROT_WRITE,
                          FINTE_MAP_PRIVATE | FINTE_MAP_ANONYMOUS,
                          (uint64_t)-1, 0 };
    int64_t addr = finte_linux_call(g, FINTE_NR_mmap, margs);
    ST(addr > 0);
    if (addr > 0) {
        char *mem = (char *)(uintptr_t)addr;
        finte_memcpy(mem, "ok", 2);
        ST(mem[0] == 'o' && mem[1] == 'k');
    }
    ST(finte_linux_call(g, FINTE_NR_getpid, NULL) == (int64_t)g->slot + 1);
}

int finte_selftest_run(int verbose, fselftest_report_t *out)
{
    fselftest_report_t local;
    if (!out) {
        out = &local;
    }
    finte_memset(out, 0, sizeof(*out));
    hal_watchdog_suspend();
    if (!finte_is_initialized()) {
        finte_init();
    }

    st_verbose = verbose;
    st_checks = 0;
    st_fails = 0;
    st_failed_section[0] = '\0';
    if (st_verbose >= 1) {
        st_print("[post] core self-check\n");
    }

    st_run("objects", st_objects);
    st_run("capabilities", st_caps);
    st_run("channels", st_channels);
    st_run("arenas", st_arena);
    st_run("scheduler", st_scheduler);
    st_run("linux persona", st_persona);

    out->sections = 6;
    out->checks = (uint32_t)st_checks;
    out->failures = (uint32_t)st_fails;
    finte_strlcpy(out->failed_section, st_failed_section,
                  sizeof(out->failed_section));

    if (st_verbose >= 1) {
        if (st_fails == 0) {
            st_print("[post] self-check PASS: 6 sections, %d checks\n", st_checks);
        } else {
            st_print("[post] self-check FAIL: %d/%d checks, section=%s\n",
                     st_fails, st_checks,
                     st_failed_section[0] ? st_failed_section : "unknown");
        }
    }

    finte_reset();
    hal_watchdog_resume();
    return st_fails == 0 ? 0 : -1;
}
