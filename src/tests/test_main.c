#include "finte/finte.h"
#include "finte/ansi.h"
#include "finte/boot.h"
#include "finte/demo.h"
#include "finte/guest.h"

#include <stdio.h>

static int g_pass;
static int g_fail;
static const char *g_test;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    [fail] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define CHECK_OK(expr) CHECK((expr).status == FINTE_OK)
#define CHECK_ERR(expr, err) CHECK((expr).status == (err))

static fflow_t *make_flow(const char *name, const fprogram_t *prog,
                          uint32_t fuel, fhandle_t *out)
{
    fhandle_t h = finte_flow_create(name, prog, fuel, FINTE_PERSONA_NATIVE);
    if (out) {
        *out = h;
    }
    return finte_flow_lookup(h);
}

static fflow_t *flow_with_handle(const char *name, const fprogram_t *prog,
                                 uint32_t fuel)
{
    return make_flow(name, prog, fuel, NULL);
}

static void test_handles(void)
{
    fhandle_t ch = finte_channel_create("ch", 4);
    CHECK(ch != FINTE_HANDLE_NONE);
    CHECK(finte_node_type(ch) == FINTE_NODE_CHANNEL);

    fflow_t *holder = flow_with_handle("holder", NULL, 1024);
    CHECK(holder != NULL);
    int cap = finte_cap_install(holder, ch, FINTE_R_SEND | FINTE_R_RECV);
    CHECK(cap == 0);
    CHECK(finte_node_type(ch) == FINTE_NODE_CHANNEL);
    CHECK(finte_cap_drop_index(holder, (uint16_t)cap) == 0);
    CHECK(finte_node_type(ch) == FINTE_NODE_FREE);

    fhandle_t ch2 = finte_channel_create("ch2", 4);
    CHECK(ch2 != FINTE_HANDLE_NONE);
    CHECK(ch2 != ch);
    CHECK(finte_node_type(ch2) == FINTE_NODE_CHANNEL);
    CHECK(finte_node_type(ch) == FINTE_NODE_FREE);
}

static void test_cap_rights(void)
{
    fhandle_t console = finte_console_create("console");
    CHECK(console != FINTE_HANDLE_NONE);
    fflow_t *f = flow_with_handle("reader", NULL, 1024);
    CHECK(f != NULL);

    int rd = finte_cap_install(f, console, FINTE_R_READ);
    int wr = finte_cap_install(f, console, FINTE_R_WRITE);
    CHECK(rd == 0);
    CHECK(wr == 1);
    CHECK_ERR(fx_console_write(f, (uint16_t)rd, "", 0), FINTE_ERR_PERM);
    CHECK_OK(fx_console_write(f, (uint16_t)wr, "", 0));

    fcap_info_t info;
    CHECK_OK(fx_cap_info(f, (uint16_t)wr, &info));
    CHECK(info.type == FINTE_NODE_CONSOLE);
    CHECK(info.rights == FINTE_R_WRITE);
    CHECK(info.handle == console);

    CHECK_OK(fx_cap_drop(f, (uint16_t)rd));
    CHECK_ERR(fx_cap_drop(f, (uint16_t)rd), FINTE_ERR_NOENT);
    CHECK_OK(fx_cap_dup(f, (uint16_t)wr, FINTE_R_WRITE));
}

static void test_cap_grant(void)
{
    fhandle_t console = finte_console_create("console");
    fflow_t *sender = flow_with_handle("sender", NULL, 1024);
    fflow_t *receiver = flow_with_handle("receiver", NULL, 1024);
    CHECK(sender && receiver);

    int console_cap = finte_cap_install(sender, console, FINTE_R_READ | FINTE_R_WRITE);
    int target_cap = finte_cap_install(sender, receiver->self,
                                       FINTE_R_GRANT | FINTE_R_CONTROL);
    CHECK(console_cap == 0);
    CHECK(target_cap == 1);

    fret_t r = fx_cap_grant(sender, (uint16_t)target_cap, (uint16_t)console_cap,
                            FINTE_R_READ);
    CHECK_OK(r);
    CHECK(r.value == 0);
    CHECK(receiver->caps[0].handle == console);
    CHECK(receiver->caps[0].rights == FINTE_R_READ);
    CHECK_ERR(fx_console_write(receiver, 0, "", 0), FINTE_ERR_PERM);

    r = fx_cap_grant(sender, (uint16_t)target_cap, (uint16_t)console_cap,
                     FINTE_R_WRITE | FINTE_R_READ | FINTE_R_GRANT);
    CHECK_ERR(r, FINTE_ERR_PERM);

    fcap_info_t info;
    CHECK_OK(fx_cap_info(receiver, 0, &info));
    CHECK(info.rights == FINTE_R_READ);
}

static void test_channel_fifo(void)
{
    fhandle_t ch = finte_channel_create("fifo", 4);
    fflow_t *tx = flow_with_handle("tx", NULL, 1024);
    fflow_t *rx = flow_with_handle("rx", NULL, 1024);
    CHECK(ch && tx && rx);
    CHECK(finte_cap_install(tx, ch, FINTE_R_SEND) == 0);
    CHECK(finte_cap_install(rx, ch, FINTE_R_RECV) == 0);

    for (uint32_t i = 0; i < 3; i++) {
        CHECK_OK(fx_try_send(tx, 0, i, &i, sizeof(i)));
    }
    fchan_info_t ci;
    CHECK_OK(fx_chan_info(tx, 0, &ci));
    CHECK(ci.count == 3);
    CHECK(ci.capacity == 4);

    for (uint32_t i = 0; i < 3; i++) {
        fmsg_t m;
        CHECK_OK(fx_try_recv(rx, 0, &m));
        CHECK(m.tag == i);
        CHECK(m.len == sizeof(uint32_t));
        uint32_t v = 0;
        finte_memcpy(&v, m.data, sizeof(v));
        CHECK(v == i);
    }
    fmsg_t empty;
    CHECK_ERR(fx_try_recv(rx, 0, &empty), FINTE_ERR_AGAIN);

    for (uint32_t i = 0; i < 4; i++) {
        CHECK_OK(fx_try_send(tx, 0, 100 + i, &i, sizeof(i)));
    }
    CHECK_ERR(fx_try_send(tx, 0, 999, &ch, sizeof(ch)), FINTE_ERR_AGAIN);
}

static void test_channel_close(void)
{
    fhandle_t ch = finte_channel_create("close", 4);
    fflow_t *owner = flow_with_handle("owner", NULL, 1024);
    fflow_t *writer = flow_with_handle("writer", NULL, 1024);
    CHECK(ch && owner && writer);
    CHECK(finte_cap_install(owner, ch, FINTE_R_SEND | FINTE_R_RECV |
                                          FINTE_R_CONTROL) == 0);
    CHECK(finte_cap_install(writer, ch, FINTE_R_SEND) == 0);

    uint32_t v = 1;
    CHECK_OK(fx_try_send(owner, 0, 1, &v, sizeof(v)));
    CHECK_OK(fx_try_send(owner, 0, 2, &v, sizeof(v)));
    fchan_info_t ci;
    CHECK_OK(fx_chan_info(owner, 0, &ci));
    CHECK(ci.closed == 0);
    CHECK_ERR(fx_chan_close(writer, 0), FINTE_ERR_PERM);
    CHECK_OK(fx_chan_close(owner, 0));
    CHECK_OK(fx_chan_info(owner, 0, &ci));
    CHECK(ci.closed == 1);

    fmsg_t m;
    CHECK_OK(fx_try_recv(owner, 0, &m));
    CHECK_OK(fx_try_recv(owner, 0, &m));
    CHECK_ERR(fx_try_recv(owner, 0, &m), FINTE_ERR_EOF);
    CHECK_ERR(fx_try_send(owner, 0, 3, &v, sizeof(v)), FINTE_ERR_STATE);
    CHECK_OK(fx_chan_close(owner, 0));
}

static void test_channel_caps(void)
{
    static const char payload[] = "CAPDATA";
    fhandle_t ch = finte_channel_create("caps", 4);
    fhandle_t file = finte_file_create("cap.txt", payload, sizeof(payload) - 1);
    fflow_t *tx = flow_with_handle("tx", NULL, 1024);
    fflow_t *rx = flow_with_handle("rx", NULL, 1024);
    CHECK(ch && file && tx && rx);
    CHECK(finte_cap_install(tx, ch, FINTE_R_SEND) == 0);
    CHECK(finte_cap_install(rx, ch, FINTE_R_RECV) == 0);
    int file_cap = finte_cap_install(tx, file, FINTE_R_READ);
    CHECK(file_cap == 1);

    fmsg_t msg;
    finte_memset(&msg, 0, sizeof(msg));
    msg.tag = 0xCAFE;
    msg.cap_count = 1;
    msg.caps[0] = (uint16_t)file_cap;
    msg.len = 2;
    msg.data[0] = 'h';
    msg.data[1] = 'i';
    CHECK_OK(fx_send_msg(tx, 0, &msg, 0));

    fmsg_t out;
    CHECK_OK(fx_recv_msg(rx, 0, &out, 0));
    CHECK(out.cap_count == 1);
    CHECK(out.tag == 0xCAFE);
    char buf[8];
    finte_memset(buf, 0, sizeof(buf));
    CHECK_OK(fx_file_read(rx, out.caps[0], 0, buf, sizeof(payload) - 1));
    CHECK(finte_memcmp(buf, payload, sizeof(payload) - 1) == 0);

    /* The sender keeps its cap: sending duplicates the reference. */
    finte_memset(buf, 0, sizeof(buf));
    CHECK_OK(fx_file_read(tx, (uint16_t)file_cap, 0, buf, sizeof(payload) - 1));
    CHECK(finte_memcmp(buf, payload, sizeof(payload) - 1) == 0);
}

static void test_arena(void)
{
    fhandle_t ar = finte_arena_create("arena", 4096);
    fflow_t *f = flow_with_handle("user", NULL, 1024);
    CHECK(ar && f);
    int cap = finte_cap_install(f, ar, FINTE_R_WRITE);
    CHECK(cap == 0);

    uint64_t o1 = 0;
    uint64_t o2 = 0;
    uint64_t o3 = 0;
    CHECK_OK(fx_arena_alloc(f, 0, 100, 16, &o1));
    CHECK_OK(fx_arena_alloc(f, 0, 200, 16, &o2));
    CHECK(o1 != 0 && o2 != 0 && o2 > o1);

    fnode_t *n = finte_node_get(ar);
    CHECK(n != NULL);
    farena_t *a = &finte_core.arenas[n->index];
    CHECK(a->live_bytes >= 300);
    *(uint64_t *)(a->base + o1) = 0x12345678u;
    CHECK(*(uint64_t *)(a->base + o1) == 0x12345678u);

    CHECK_OK(fx_arena_free(f, 0, o1));
    CHECK_OK(fx_arena_free(f, 0, o2));
    CHECK(a->live_bytes == 0);
    CHECK_ERR(fx_arena_free(f, 0, o1), FINTE_ERR_INVAL);

    CHECK_OK(fx_arena_alloc(f, 0, 4000, 16, &o3));
    CHECK_OK(fx_arena_free(f, 0, o3));
    CHECK_ERR(fx_arena_alloc(f, 0, 5000, 16, &o3), FINTE_ERR_NOMEM);
    CHECK_ERR(fx_arena_alloc(f, 0, 64, 32, &o3), FINTE_ERR_INVAL);

    fhandle_t slice = finte_arena_create("slice", 3 * 4096);
    CHECK(slice != FINTE_HANDLE_NONE);
    int scap = finte_cap_install_arena_pages(f, slice, 1, 1,
                                             FINTE_R_WRITE);
    CHECK(scap >= 0);
    uint64_t so1 = 0;
    CHECK_OK(fx_arena_alloc(f, (uint16_t)scap, 64, 16, &so1));
    CHECK(so1 >= 4096 && so1 < 8192);
    CHECK_ERR(fx_arena_alloc(f, (uint16_t)scap, 5000, 16, &o3),
              FINTE_ERR_NOMEM);
    CHECK_ERR(fx_arena_free(f, (uint16_t)scap, 0), FINTE_ERR_PERM);
    fcap_info_t info;
    CHECK_OK(fx_cap_info(f, (uint16_t)scap, &info));
    CHECK(info.flags & FINTE_CAP_SCOPED);
    CHECK(info.offset == 4096 && info.length == 4096);
    CHECK_OK(fx_arena_free(f, (uint16_t)scap, so1));
}

static uint64_t g_counter;

static void count_program(fflow_t *f)
{
    (void)f;
    g_counter++;
}

static const fprogram_t p_count = { "count", count_program, 4096 };

static void test_spawn(void)
{
    g_counter = 0;
    for (uint32_t i = 0; i < 8; i++) {
        fhandle_t h = finte_flow_create("count", &p_count, 4096,
                                        FINTE_PERSONA_NATIVE);
        CHECK(h != FINTE_HANDLE_NONE);
        CHECK_OK(finte_flow_start(h));
    }
    finte_run();
    CHECK(finte_run_status() == FINTE_OK);
    CHECK(g_counter == 8);
    fstats_t s = finte_get_stats();
    CHECK(s.flows_started == 8);
    CHECK(s.flows_finished == 8);
    CHECK(s.flows_live == 0);
}

static int g_thread_ok;

static void thread_probe(fflow_t *f)
{
    fthread_t *t = finte_thread_current();
    g_thread_ok = t && t->flow == f && f->thread == t &&
                  t->state == FINTE_THREAD_RUNNING &&
                  f->state == FINTE_FLOW_RUNNING;
}

static const fprogram_t p_thread_probe = { "thread-probe", thread_probe, 1024 };

static void test_thread_model(void)
{
    g_thread_ok = 0;
    fhandle_t h = finte_flow_create("thread-test", &p_thread_probe, 1024,
                                    FINTE_PERSONA_NATIVE);
    fflow_t *f = finte_flow_lookup(h);
    CHECK(f != NULL);
    CHECK(f && f->thread != NULL);
    if (!f || !f->thread) {
        return;
    }
    CHECK(f->thread->flow == f);
    CHECK(f->thread->state == FINTE_THREAD_CREATED);
    CHECK(f->state == FINTE_FLOW_FREE);
    CHECK_OK(finte_flow_start(h));
    CHECK(f->thread != NULL);
    CHECK(f->thread->state == FINTE_THREAD_READY);
    finte_run();
    CHECK(g_thread_ok == 1);
    CHECK(finte_get_stats().flows_finished >= 1);
}

static uint64_t g_sum;

static void producer10(fflow_t *f)
{
    for (uint64_t i = 0; i < 10; i++) {
        CHECK_OK(fx_send(f, 0, 1, &i, sizeof(i)));
    }
}

static void consumer10(fflow_t *f)
{
    for (int i = 0; i < 10; i++) {
        fmsg_t m;
        CHECK_OK(fx_recv(f, 0, &m));
        uint64_t v = 0;
        finte_memcpy(&v, m.data, sizeof(v));
        g_sum += v;
    }
}

static const fprogram_t p_prod = { "producer10", producer10, 8192 };
static const fprogram_t p_cons = { "consumer10", consumer10, 8192 };

static void test_blocking(void)
{
    g_sum = 0;
    fhandle_t ch = finte_channel_create("pipe", 2);
    fhandle_t ph = finte_flow_create("prod", &p_prod, 8192, FINTE_PERSONA_NATIVE);
    fhandle_t ch2 = finte_flow_create("cons", &p_cons, 8192, FINTE_PERSONA_NATIVE);
    fflow_t *p = finte_flow_lookup(ph);
    fflow_t *c = finte_flow_lookup(ch2);
    CHECK(ch && p && c);
    CHECK(finte_cap_install(p, ch, FINTE_R_SEND) == 0);
    CHECK(finte_cap_install(c, ch, FINTE_R_RECV) == 0);
    CHECK_OK(finte_flow_start(ph));
    CHECK_OK(finte_flow_start(ch2));
    finte_run();
    CHECK(finte_run_status() == FINTE_OK);
    CHECK(g_sum == 45);
    fstats_t s = finte_get_stats();
    CHECK(s.messages_sent == 10);
    CHECK(s.messages_received == 10);
}

static void blocked_forever(fflow_t *f)
{
    fmsg_t m;
    fx_recv(f, 0, &m);
}

static const fprogram_t p_blocked = { "blocked", blocked_forever, 1024 };

static void test_deadlock(void)
{
    fhandle_t ch = finte_channel_create("empty", 2);
    fhandle_t h = finte_flow_create("blocked", &p_blocked, 1024,
                                    FINTE_PERSONA_NATIVE);
    fflow_t *f = finte_flow_lookup(h);
    CHECK(ch && f);
    CHECK(finte_cap_install(f, ch, FINTE_R_RECV) == 0);
    CHECK_OK(finte_flow_start(h));
    finte_run();
    CHECK(finte_run_status() == FINTE_ERR_DEADLOCK);
    CHECK(finte_get_stats().deadlocks == 1);
    CHECK(f->state == FINTE_FLOW_BLOCKED);
}

static uint64_t g_sleep_elapsed;
static int g_slept;

static void sleeper_program(fflow_t *f)
{
    uint64_t t0 = hal_now_ns();
    fx_sleep_ns(f, 2 * 1000 * 1000);
    g_sleep_elapsed = hal_now_ns() - t0;
    g_slept = 1;
}

static const fprogram_t p_sleeper = { "sleeper", sleeper_program, 4096 };

static void test_sleep(void)
{
    g_slept = 0;
    g_sleep_elapsed = 0;
    fhandle_t h = finte_flow_create("sleeper", &p_sleeper, 4096,
                                    FINTE_PERSONA_NATIVE);
    CHECK(h != FINTE_HANDLE_NONE);
    CHECK_OK(finte_flow_start(h));
    finte_run();
    CHECK(finte_run_status() == FINTE_OK);
    CHECK(g_slept == 1);
    CHECK(g_sleep_elapsed >= 1000000ull);
}

static int g_yields;

static void yielder_program(fflow_t *f)
{
    for (int i = 0; i < 10; i++) {
        g_yields++;
        fx_yield(f);
    }
}

static const fprogram_t p_yielder = { "yielder", yielder_program, 48 };

static void test_fuel(void)
{
    g_yields = 0;
    fhandle_t h = finte_flow_create("yielder", &p_yielder, 48,
                                    FINTE_PERSONA_NATIVE);
    CHECK(h != FINTE_HANDLE_NONE);
    CHECK_OK(finte_flow_start(h));
    finte_run();
    CHECK(finte_run_status() == FINTE_OK);
    CHECK(g_yields == 10);
    fstats_t s = finte_get_stats();
    CHECK(s.epochs >= 1);
    CHECK(s.fuel_burned >= 48);
}

static int g_target_done;
static int g_wait_done;

static void wait_target_program(fflow_t *f)
{
    fx_yield(f);
    fx_yield(f);
    g_target_done = 1;
}

static void wait_waiter_program(fflow_t *f)
{
    CHECK_OK(fx_flow_wait(f, 0));
    g_wait_done = 1;
}

static const fprogram_t p_wait_target = { "wait-target", wait_target_program, 4096 };
static const fprogram_t p_wait_waiter = { "wait-waiter", wait_waiter_program, 4096 };

static void test_flow_wait(void)
{
    g_target_done = 0;
    g_wait_done = 0;
    fhandle_t th = finte_flow_create("target", &p_wait_target, 4096,
                                     FINTE_PERSONA_NATIVE);
    fhandle_t wh = finte_flow_create("waiter", &p_wait_waiter, 4096,
                                     FINTE_PERSONA_NATIVE);
    fflow_t *target = finte_flow_lookup(th);
    fflow_t *waiter = finte_flow_lookup(wh);
    CHECK(target != NULL);
    CHECK(waiter != NULL);
    if (!target || !waiter) {
        return;
    }
    CHECK(finte_cap_install(waiter, target->self, FINTE_R_CONTROL) == 0);
    CHECK_OK(finte_flow_start(th));
    CHECK_OK(finte_flow_start(wh));
    finte_run();
    CHECK(finte_run_status() == FINTE_OK);
    CHECK(g_target_done == 1);
    CHECK(g_wait_done == 1);
}

static void test_persona(void)
{
    static const char hello[] = "compat-data";
    static const char path[] = "/hello.txt";
    fhandle_t console = finte_console_create("console");
    fhandle_t dir = finte_dir_create("/");
    fhandle_t file = finte_file_create("hello.txt", hello, sizeof(hello) - 1);
    fhandle_t arena = finte_arena_create("heap", 2 * 1024 * 1024);
    CHECK(console && dir && file && arena);
    CHECK_OK(finte_dir_add(dir, "hello.txt", file));

    fhandle_t fh = finte_flow_create("guest", &finte_linux_guest_program,
                                     8192, FINTE_PERSONA_LINUX);
    fflow_t *f = finte_flow_lookup(fh);
    CHECK(f != NULL);
    int cc = finte_cap_install(f, console, FINTE_R_WRITE | FINTE_R_READ);
    int dc = finte_cap_install(f, dir, FINTE_R_READ);
    int ac = finte_cap_install(f, arena, FINTE_R_WRITE);
    CHECK(cc == 0 && dc == 1 && ac == 2);
    CHECK_OK(finte_linux_bootstrap(f, (uint16_t)cc, (uint16_t)dc, (uint16_t)ac));

    uint64_t args[6] = { (uint64_t)(int64_t)FINTE_AT_FDCWD,
                         (uint64_t)(uintptr_t)path, FINTE_O_RDONLY, 0, 0, 0 };
    int64_t fd = finte_linux_call(f, FINTE_NR_openat, args);
    CHECK(fd == 3);

    char buf[32];
    finte_memset(buf, 0, sizeof(buf));
    uint64_t rargs[6] = { (uint64_t)fd, (uint64_t)(uintptr_t)buf, sizeof(buf) - 1,
                          0, 0, 0 };
    int64_t n = finte_linux_call(f, FINTE_NR_read, rargs);
    CHECK(n == (int64_t)(sizeof(hello) - 1));
    CHECK(finte_memcmp(buf, hello, sizeof(hello) - 1) == 0);

    flinux_stat_t st;
    finte_memset(&st, 0, sizeof(st));
    uint64_t sargs[6] = { (uint64_t)fd, (uint64_t)(uintptr_t)&st, 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_fstat, sargs) == 0);
    CHECK(st.size == sizeof(hello) - 1);

    uint64_t wargs[6] = { (uint64_t)fd, (uint64_t)(uintptr_t)"x", 1, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_write, wargs) == -FINTE_EROFS);

    uint64_t margs[6] = { 0, 4096, FINTE_PROT_READ | FINTE_PROT_WRITE,
                          FINTE_MAP_PRIVATE | FINTE_MAP_ANONYMOUS,
                          (uint64_t)-1, 0 };
    int64_t addr = finte_linux_call(f, FINTE_NR_mmap, margs);
    CHECK(addr > 0);
    if (addr > 0) {
        char *mem = (char *)(uintptr_t)addr;
        finte_memcpy(mem, "xyz", 3);
        CHECK(mem[0] == 'x' && mem[1] == 'y' && mem[2] == 'z');
    }

    flinux_utsname_t un;
    finte_memset(&un, 0, sizeof(un));
    uint64_t uargs[6] = { (uint64_t)(uintptr_t)&un, 0, 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_uname, uargs) == 0);
    CHECK(finte_strcmp(un.sysname, "Linux") == 0);
    CHECK(finte_strcmp(un.machine, "x86_64") == 0);

    CHECK(finte_linux_call(f, FINTE_NR_getpid, NULL) == (int64_t)f->slot + 1);
    uint8_t rnd[8];
    uint64_t gargs[6] = { (uint64_t)(uintptr_t)rnd, sizeof(rnd), 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_getrandom, gargs) == (int64_t)sizeof(rnd));

    uint64_t fargs[6] = { (uint64_t)(int64_t)FINTE_AT_FDCWD,
                          (uint64_t)(uintptr_t)path,
                          (uint64_t)(uintptr_t)&st, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_newfstatat, fargs) == 0);
    CHECK(st.size == sizeof(hello) - 1);

    CHECK(finte_linux_call(f, 9999, NULL) == -FINTE_ENOSYS);
    uint64_t cargs[6] = { (uint64_t)fd, 0, 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_close, cargs) == 0);
    CHECK(finte_linux_call(f, FINTE_NR_read, rargs) == -FINTE_EBADF);

    /* open/stat/access */
    uint64_t oargs[6] = { (uint64_t)(uintptr_t)path, FINTE_O_RDONLY, 0, 0, 0, 0 };
    int64_t fd2 = finte_linux_call(f, FINTE_NR_open, oargs);
    CHECK(fd2 == 3);
    uint64_t stargs[6] = { (uint64_t)(uintptr_t)path,
                           (uint64_t)(uintptr_t)&st, 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_stat, stargs) == 0);
    CHECK(st.size == sizeof(hello) - 1);
    uint64_t aargs[6] = { (uint64_t)(uintptr_t)path, FINTE_F_OK, 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_access, aargs) == 0);
    aargs[1] = FINTE_W_OK;
    CHECK(finte_linux_call(f, FINTE_NR_access, aargs) == -FINTE_EROFS);

    /* getcwd */
    char cwd[8];
    finte_memset(cwd, 0, sizeof(cwd));
    uint64_t gwargs[6] = { (uint64_t)(uintptr_t)cwd, sizeof(cwd), 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_getcwd, gwargs) == 2);
    CHECK(cwd[0] == '/' && cwd[1] == '\0');

    /* dup/dup2 */
    uint64_t dargs[6] = { (uint64_t)fd2, 0, 0, 0, 0, 0 };
    int64_t fd3 = finte_linux_call(f, FINTE_NR_dup, dargs);
    CHECK(fd3 == 4);
    uint64_t d2args[6] = { (uint64_t)fd2, 7, 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_dup2, d2args) == 7);
    CHECK(finte_linux_call(f, FINTE_NR_close,
                           (uint64_t[]){ (uint64_t)fd2, 0, 0, 0, 0, 0 }) == 0);
    char dbuf[16];
    finte_memset(dbuf, 0, sizeof(dbuf));
    uint64_t drargs[6] = { (uint64_t)fd3, (uint64_t)(uintptr_t)dbuf,
                           sizeof(dbuf) - 1, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_read, drargs) ==
          (int64_t)(sizeof(hello) - 1));
    CHECK(finte_memcmp(dbuf, hello, sizeof(hello) - 1) == 0);
    CHECK(finte_linux_call(f, FINTE_NR_close,
                           (uint64_t[]){ (uint64_t)fd3, 0, 0, 0, 0, 0 }) == 0);
    CHECK(finte_linux_call(f, FINTE_NR_close,
                           (uint64_t[]){ 7, 0, 0, 0, 0, 0 }) == 0);

    /* Small compatibility calls. */
    flinux_timespec_t res;
    finte_memset(&res, 0, sizeof(res));
    uint64_t resargs[6] = { FINTE_CLOCK_MONOTONIC,
                            (uint64_t)(uintptr_t)&res, 0, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_clock_getres, resargs) == 0);
    CHECK(res.tv_sec == 0 && res.tv_nsec == 1);

    uint64_t mask = 0;
    uint64_t affargs[6] = { 0, sizeof(mask), (uint64_t)(uintptr_t)&mask, 0, 0, 0 };
    CHECK(finte_linux_call(f, FINTE_NR_sched_getaffinity, affargs) == 8);
    CHECK((mask & 1u) != 0);
    CHECK(finte_linux_call(f, FINTE_NR_prlimit64, NULL) == 0);
    CHECK(finte_linux_call(f, FINTE_NR_sched_yield, NULL) == 0);
    CHECK(finte_linux_call(f, FINTE_NR_getppid, NULL) == 0);
    CHECK(finte_linux_call(f, FINTE_NR_readlink, NULL) == -FINTE_EINVAL);
}

static void test_no_ambient_authority(void)
{
    fflow_t *f = flow_with_handle("bare", NULL, 1024);
    CHECK(f != NULL);
    CHECK_ERR(fx_console_write(f, 0, "", 0), FINTE_ERR_NOENT);
    CHECK(finte_get_stats().caps_denied > 0);
}

static void test_demo(void)
{
    CHECK_OK(finte_demo_bootstrap());
    finte_run();
    CHECK(finte_run_status() == FINTE_OK);
    const fdemo_result_t *d = finte_demo_result();
    CHECK(d->ok == 1);
    CHECK(d->produced == 12);
    CHECK(d->transformed == 12);
    CHECK(d->consumed == 12);
    CHECK(d->checksum == 650);
    CHECK(d->denied == 2);
}

static void test_ansi(void)
{
    ansi_parser_t p;
    ansi_init(&p, 7);
    CHECK(p.style.fg == 7);
    CHECK(p.style.bg == 0);
    CHECK(p.style.bold == 0);

    CHECK(ansi_feed(&p, 'A') == 0);
    const char *bright_red = "\x1b[1;31m";
    while (*bright_red) {
        CHECK(ansi_feed(&p, (unsigned char)*bright_red++) == 1);
    }
    CHECK(p.state == 0);
    CHECK(p.style.bold == 1);
    CHECK(p.style.fg == 1);
    CHECK(ansi_fg_index(&p.style) == 9);
    CHECK(ansi_vga_color(9) == 12);
    CHECK(ansi_rgb(9) == 0xFF5555u);

    const char *green_bg = "\x1b[42m";
    while (*green_bg) {
        CHECK(ansi_feed(&p, (unsigned char)*green_bg++) == 1);
    }
    CHECK(p.style.bg == 2);

    const char *bright_cyan = "\x1b[96m";
    while (*bright_cyan) {
        (void)ansi_feed(&p, (unsigned char)*bright_cyan++);
    }
    CHECK(p.style.fg == 14);
    CHECK(ansi_vga_color(14) == 11);
    CHECK(ansi_rgb(14) == 0x55FFFFu);

    const char *reset_fg = "\x1b[39m\x1b[49m";
    while (*reset_fg) {
        (void)ansi_feed(&p, (unsigned char)*reset_fg++);
    }
    CHECK(p.style.fg == 7);
    CHECK(p.style.bg == 0);

    const char *no_bold = "\x1b[22m";
    while (*no_bold) {
        (void)ansi_feed(&p, (unsigned char)*no_bold++);
    }
    CHECK(p.style.bold == 0);

    const char *reset = "\x1b[0m";
    while (*reset) {
        (void)ansi_feed(&p, (unsigned char)*reset++);
    }
    CHECK(p.style.fg == 7);
    CHECK(p.style.bg == 0);
    CHECK(p.style.bold == 0);

    ansi_init(&p, 15);
    const char *white_reset = "\x1b[1;97m\x1b[0m";
    while (*white_reset) {
        (void)ansi_feed(&p, (unsigned char)*white_reset++);
    }
    CHECK(p.style.fg == 15);
    CHECK(ansi_rgb(ansi_fg_index(&p.style)) == 0xFFFFFFu);

    /* A non-SGR final byte is consumed without changing the style. */
    const char *clear_screen = "\x1b[2J";
    while (*clear_screen) {
        CHECK(ansi_feed(&p, (unsigned char)*clear_screen++) == 1);
    }
    CHECK(p.state == 0);
    CHECK(p.style.fg == 15);
}

static void test_status_text(void)
{
    CHECK(finte_strcmp(finte_strstatus(FINTE_OK), "ok") == 0);
    CHECK(finte_strcmp(finte_strstatus(FINTE_ERR_NOENT), "no-entry") == 0);
    CHECK(finte_strcmp(finte_strstatus(FINTE_ERR_EOF), "end-of-stream") == 0);
    CHECK(finte_strcmp(finte_strstatus(-99), "unknown") == 0);
    CHECK(finte_strcmp(finte_disk_error(0), "ok") == 0);
    CHECK(finte_strcmp(finte_disk_error(FINTE_ERR_NOENT), "not found") == 0);
    CHECK(finte_strcmp(finte_disk_error(FINTE_ERR_EOF), "end of file") == 0);
    CHECK(finte_strcmp(finte_disk_error(-99), "disk error") == 0);
}

static void test_pmm(void)
{
    (void)pmm_init(NULL, 0, 0);
    CHECK(pmm_active() == 0);

    fboot_mmap_entry_t map[1];
    map[0].base = 0x100000;
    map[0].length = 0x10000;
    map[0].type = FINTE_BOOT_MMAP_USABLE;
    map[0].flags = 0;
    fboot_info_t bi;
    finte_memset(&bi, 0, sizeof(bi));
    bi.flags = FINTE_BOOT_FLAG_MEMMAP;
    bi.memmap_addr = (uint64_t)(uintptr_t)map;
    bi.memmap_count = 1;
    bi.memmap_desc_size = sizeof(map[0]);

    CHECK(pmm_init(&bi, 0x100000, 0x110000) == 0);
    CHECK(pmm_active() == 1);
    CHECK(pmm_total_pages() == 16);
    CHECK(pmm_free_page_count() == 16);
    CHECK(pmm_reserve(0x100000, 0x1000) == 0);
    CHECK(pmm_free_page_count() == 15);
    uint64_t a = pmm_alloc_pages(1);
    CHECK(a == 0x101000);
    uint64_t b = pmm_alloc_pages(2);
    CHECK(b == 0x102000);
    CHECK(pmm_owns(a, 4096) == 1);
    CHECK(pmm_owns(b, 8192) == 1);
    CHECK(pmm_owns(0x200000, 4096) == 0);
    pmm_free_pages(b, 2);
    CHECK(pmm_free_page_count() == 14);
    pmm_free_pages(a, 1);
    CHECK(pmm_free_page_count() == 15);
    CHECK(pmm_init(NULL, 0, 0) != 0);
    CHECK(pmm_active() == 0);
}

static void test_boot_protocol(void)
{
    fboot_info_t bi;
    finte_memset(&bi, 0, sizeof(bi));
    bi.magic = FINTE_BOOT_MAGIC;
    bi.version = FINTE_BOOT_VERSION;
    bi.header_size = FINTE_BOOT_INFO_BYTES;
    bi.total_size = FINTE_BOOT_INFO_BYTES;
    bi.arch = FINTE_BOOT_ARCH_X86_64;
    bi.platform = FINTE_BOOT_PLATFORM_BIOS;
    bi.console_type = FINTE_BOOT_CONSOLE_VGA;
    bi.kernel_base = 0x4000000;
    bi.kernel_size = 4096;
    CHECK(finte_boot_validate(&bi, FINTE_BOOT_ARCH_X86_64) == FINTE_BOOT_OK);

    bi.version = 99;
    CHECK(finte_boot_validate(&bi, FINTE_BOOT_ARCH_X86_64) ==
          FINTE_BOOT_ERR_VERSION);
    bi.version = FINTE_BOOT_VERSION;
    bi.magic = 0;
    CHECK(finte_boot_validate(&bi, FINTE_BOOT_ARCH_X86_64) ==
          FINTE_BOOT_ERR_MAGIC);
    bi.magic = FINTE_BOOT_MAGIC;

    fboot_mmap_entry_t map[1];
    map[0].base = 0x100000;
    map[0].length = 0x1000;
    map[0].type = FINTE_BOOT_MMAP_USABLE;
    map[0].flags = 0;
    bi.flags = FINTE_BOOT_FLAG_MEMMAP;
    bi.memmap_addr = (uint64_t)(uintptr_t)map;
    bi.memmap_count = 1;
    bi.memmap_desc_size = sizeof(map[0]);
    CHECK(finte_boot_validate(&bi, FINTE_BOOT_ARCH_X86_64) == FINTE_BOOT_OK);
    CHECK(finte_boot_mmap_usable(&bi, 0x100000, 0x1000) == 1);
    CHECK(finte_boot_mmap_usable(&bi, 0x101000, 0x1000) == 0);
    map[0].length = 0;
    CHECK(finte_boot_validate(&bi, FINTE_BOOT_ARCH_X86_64) ==
          FINTE_BOOT_ERR_MEMMAP);
}

static void test_format(void)
{
    char buf[128];
    int n = finte_format(buf, sizeof(buf),
                         "%s %c %d %u %x %X %08x %lld %p %%",
                         "ok", 'A', -12, 42u, 0xBEEFu, 0xBEEFu,
                         0xBEEFu, -1234567890123ll, (void *)0x1234);
    CHECK(n == (int)finte_strlen(buf));
    CHECK(finte_strcmp(
              buf,
              "ok A -12 42 beef BEEF 0000beef -1234567890123 0x1234 %") == 0);
}

struct testcase {
    const char *name;
    void (*fn)(void);
};

static const struct testcase g_tests[] = {
    { "handles/generation", test_handles },
    { "capability rights", test_cap_rights },
    { "capability grant", test_cap_grant },
    { "channel FIFO", test_channel_fifo },
    { "channel close/EOF", test_channel_close },
    { "channel cap transfer", test_channel_caps },
    { "arena allocator", test_arena },
    { "flow/thread split", test_thread_model },
    { "flow spawn/join", test_spawn },
    { "blocking channel", test_blocking },
    { "deadlock detection", test_deadlock },
    { "sleep/wake", test_sleep },
    { "fuel/throttle", test_fuel },
    { "flow wait/join", test_flow_wait },
    { "Linux persona", test_persona },
    { "no ambient authority", test_no_ambient_authority },
    { "native demo", test_demo },
    { "ANSI SGR parser", test_ansi },
    { "status text", test_status_text },
    { "boot protocol", test_boot_protocol },
    { "physical memory manager", test_pmm },
    { "formatter", test_format },
};

int finte_tests_run(void)
{
    hal_init();
    finte_init();
    printf("\nFinte %s | self-tests\n\n", FINTE_VERSION_STRING);
    for (size_t i = 0; i < sizeof(g_tests) / sizeof(g_tests[0]); i++) {
        g_test = g_tests[i].name;
        finte_reset();
        int before = g_fail;
        g_tests[i].fn();
        if (g_fail == before) {
            g_pass++;
            printf("  [ ok ] %s\n", g_test);
        } else {
            printf("  [fail] %s\n", g_test);
        }
    }
    printf("\n  %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
