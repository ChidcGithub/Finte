#include "finte/finte.h"

#include <stdio.h>

#define BENCH_ITER 200000u
#define BENCH_ARENA 100000u
#define BENCH_CHAN 20000u
#define BENCH_FLOWS 32u

static uint64_t g_cycles;
static uint64_t g_ops;

static void bench_syscalls(fflow_t *f)
{
    uint64_t ns = 0;
    uint64_t c0 = hal_cycles();
    for (uint64_t i = 0; i < BENCH_ITER; i++) {
        fx_clock_now(f, &ns);
    }
    g_cycles = hal_cycles() - c0;
    g_ops = BENCH_ITER;
    (void)ns;
}

static void bench_caps(fflow_t *f)
{
    fcap_info_t info;
    uint64_t c0 = hal_cycles();
    for (uint64_t i = 0; i < BENCH_ITER; i++) {
        fx_cap_info(f, 0, &info);
    }
    g_cycles = hal_cycles() - c0;
    g_ops = BENCH_ITER;
}

static void bench_arena(fflow_t *f)
{
    uint64_t c0 = hal_cycles();
    for (uint64_t i = 0; i < BENCH_ARENA; i++) {
        uint64_t off = 0;
        if (finte_failed(fx_arena_alloc(f, 0, 64, 16, &off))) {
            break;
        }
        fx_arena_free(f, 0, off);
    }
    g_cycles = hal_cycles() - c0;
    g_ops = BENCH_ARENA;
}

static void bench_ping(fflow_t *f)
{
    uint64_t v = 0;
    for (uint64_t i = 0; i < BENCH_CHAN; i++) {
        v = i;
        fx_send(f, 0, 1, &v, sizeof(v));
        fmsg_t msg;
        fx_recv(f, 1, &msg);
    }
}

static void bench_pong(fflow_t *f)
{
    for (uint64_t i = 0; i < BENCH_CHAN; i++) {
        fmsg_t msg;
        fx_recv(f, 0, &msg);
        fx_send(f, 1, 2, msg.data, msg.len);
    }
}

static void bench_noop(fflow_t *f)
{
    (void)f;
}

static const fprogram_t p_syscalls = { "bench-syscall", bench_syscalls, 1000000 };
static const fprogram_t p_caps = { "bench-caps", bench_caps, 1000000 };
static const fprogram_t p_arena = { "bench-arena", bench_arena, 1000000 };
static const fprogram_t p_ping = { "bench-ping", bench_ping, 100000000 };
static const fprogram_t p_pong = { "bench-pong", bench_pong, 100000000 };
static const fprogram_t p_noop = { "bench-noop", bench_noop, 4096 };

static double ns_per_op(uint64_t cycles, uint64_t ops)
{
    if (!ops) {
        return 0.0;
    }
    long double ns = (long double)cycles * 1000000000.0L /
                     (long double)hal_cycles_per_sec();
    return (double)(ns / (long double)ops);
}

static void report(const char *name, uint64_t cycles, uint64_t ops)
{
    double ns = ns_per_op(cycles, ops);
    double rate = ns > 0 ? 1000.0 / ns : 0.0;
    printf("  %-24s %8.1f ns/op   %8.2f Mops/s\n", name, ns, rate);
}

static int start_one(const fprogram_t *prog, uint32_t fuel,
                     fhandle_t *cap_obj, uint32_t rights,
                     fflow_t **out)
{
    fhandle_t fh = finte_flow_create(prog->name, prog, fuel, FINTE_PERSONA_NATIVE);
    if (!fh) {
        return -1;
    }
    fflow_t *f = finte_flow_lookup(fh);
    if (!f) {
        return -1;
    }
    if (cap_obj) {
        if (finte_cap_install(f, *cap_obj, rights) < 0) {
            return -1;
        }
    }
    if (finte_failed(finte_flow_start(fh))) {
        return -1;
    }
    if (out) {
        *out = f;
    }
    return 0;
}

static int bench_syscall_once(void)
{
    finte_reset();
    fhandle_t con = finte_console_create("bench-console");
    if (!con) {
        return -1;
    }
    if (start_one(&p_syscalls, 1000000, &con, FINTE_R_WRITE, NULL) < 0) {
        return -1;
    }
    finte_run();
    return finte_run_status() == FINTE_OK ? 0 : -1;
}

static int bench_cap_once(void)
{
    finte_reset();
    fhandle_t con = finte_console_create("bench-console");
    if (!con) {
        return -1;
    }
    if (start_one(&p_caps, 1000000, &con, FINTE_R_WRITE, NULL) < 0) {
        return -1;
    }
    finte_run();
    return finte_run_status() == FINTE_OK ? 0 : -1;
}

static int bench_arena_once(void)
{
    finte_reset();
    fhandle_t ar = finte_arena_create("bench-arena", 1024 * 1024);
    if (!ar) {
        return -1;
    }
    if (start_one(&p_arena, 1000000, &ar, FINTE_R_WRITE, NULL) < 0) {
        return -1;
    }
    finte_run();
    return finte_run_status() == FINTE_OK ? 0 : -1;
}

static int bench_chan_once(uint64_t *cycles)
{
    finte_reset();
    fhandle_t a = finte_channel_create("bench-a", 1);
    fhandle_t b = finte_channel_create("bench-b", 1);
    if (!a || !b) {
        return -1;
    }
    fhandle_t ph = finte_flow_create("ping", &p_ping, 100000000, FINTE_PERSONA_NATIVE);
    fhandle_t qh = finte_flow_create("pong", &p_pong, 100000000, FINTE_PERSONA_NATIVE);
    fflow_t *p = finte_flow_lookup(ph);
    fflow_t *q = finte_flow_lookup(qh);
    if (!p || !q) {
        return -1;
    }
    if (finte_cap_install(p, a, FINTE_R_SEND) < 0 ||
        finte_cap_install(p, b, FINTE_R_RECV) < 0 ||
        finte_cap_install(q, a, FINTE_R_RECV) < 0 ||
        finte_cap_install(q, b, FINTE_R_SEND) < 0) {
        return -1;
    }
    if (finte_failed(finte_flow_start(ph)) ||
        finte_failed(finte_flow_start(qh))) {
        return -1;
    }
    uint64_t c0 = hal_cycles();
    finte_run();
    *cycles = hal_cycles() - c0;
    return finte_run_status() == FINTE_OK ? 0 : -1;
}

static int bench_spawn_once(uint64_t *cycles)
{
    finte_reset();
    uint64_t c0 = hal_cycles();
    for (uint32_t i = 0; i < BENCH_FLOWS; i++) {
        fhandle_t fh = finte_flow_create("noop", &p_noop, 4096,
                                         FINTE_PERSONA_NATIVE);
        if (!fh || finte_failed(finte_flow_start(fh))) {
            return -1;
        }
    }
    finte_run();
    *cycles = hal_cycles() - c0;
    return finte_run_status() == FINTE_OK ? 0 : -1;
}

int finte_bench_run(void)
{
    finte_init();
    printf("\nFinte %s | core microbenchmarks\n\n", FINTE_VERSION_STRING);
    printf("  host cycle source: %llu Hz, one core, cooperative flows\n",
           (unsigned long long)hal_cycles_per_sec());
    printf("  methodology: one warm process, single run per row\n\n");

    uint64_t cycles = 0;
    if (bench_syscall_once() == 0) {
        report("verb clock_now", g_cycles, g_ops);
    } else {
        printf("  verb clock_now           FAILED\n");
    }
    if (bench_cap_once() == 0) {
        report("cap validate + info", g_cycles, g_ops);
    } else {
        printf("  cap validate + info      FAILED\n");
    }
    if (bench_arena_once() == 0) {
        report("arena alloc + free", g_cycles, g_ops);
    } else {
        printf("  arena alloc + free       FAILED\n");
    }
    if (bench_chan_once(&cycles) == 0) {
        report("channel roundtrip", cycles, BENCH_CHAN);
    } else {
        printf("  channel roundtrip        FAILED\n");
    }
    if (bench_spawn_once(&cycles) == 0) {
        report("flow spawn + run", cycles, BENCH_FLOWS);
    } else {
        printf("  flow spawn + run         FAILED\n");
    }
    printf("\n  note: a roundtrip includes two blocking channel ops and the\n");
    printf("        scheduler context switches, measured end to end.\n\n");
    finte_reset();
    return 0;
}
