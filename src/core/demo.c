#include "finte/demo.h"
#include "finte/compat_linux.h"
#include "finte/guest.h"
#include "finte/verb.h"

#define DEMO_N 12u
#define DEMO_TAG 0x46494E54u /* "FINT" */
#define DEMO_RAW_CAP 0
#define DEMO_OUT_CAP 1

typedef struct demo_msg {
    uint64_t value;
    uint64_t square;
} demo_msg_t;

static fdemo_result_t g_result;

static void demo_producer(fflow_t *f)
{
    for (uint64_t i = 1; i <= DEMO_N; i++) {
        demo_msg_t m;
        m.value = i;
        m.square = 0;
        fret_t r = fx_send(f, DEMO_RAW_CAP, DEMO_TAG, &m, sizeof(m));
        if (finte_failed(r)) {
            fx_println(f, 1, "[producer] send failed: %s", finte_strstatus(r.status));
            break;
        }
        g_result.produced++;
    }
    /* This flow holds a console cap with READ but no WRITE. */
    fret_t r = fx_console_write(f, 1, "x", 1);
    g_result.denied_status[0] = r.status;
    if (r.status == FINTE_ERR_PERM) {
        g_result.denied++;
    }
    fx_yield(f);
}

static void demo_transformer(fflow_t *f)
{
    for (uint64_t i = 0; i < DEMO_N; i++) {
        fmsg_t msg;
        fret_t r = fx_recv(f, 0, &msg);
        if (finte_failed(r)) {
            fx_println(f, 2, "[transform] recv failed: %s", finte_strstatus(r.status));
            break;
        }
        demo_msg_t in;
        finte_memcpy(&in, msg.data, sizeof(in));
        in.square = in.value * in.value;
        r = fx_send(f, DEMO_OUT_CAP, DEMO_TAG, &in, sizeof(in));
        if (finte_failed(r)) {
            fx_println(f, 2, "[transform] send failed: %s", finte_strstatus(r.status));
            break;
        }
        g_result.transformed++;
    }
    fret_t r = fx_console_write(f, 2, "x", 1);
    g_result.denied_status[1] = r.status;
    if (r.status == FINTE_ERR_PERM) {
        g_result.denied++;
    }
    fx_yield(f);
}

static void demo_sink(fflow_t *f)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < DEMO_N; i++) {
        fmsg_t msg;
        fret_t r = fx_recv(f, 0, &msg);
        if (finte_failed(r)) {
            fx_println(f, 1, "[sink] recv failed: %s", finte_strstatus(r.status));
            break;
        }
        demo_msg_t m;
        finte_memcpy(&m, msg.data, sizeof(m));
        sum += m.square;
        fx_println(f, 1, "[sink] %llu^2 = %llu", m.value, m.square);
        g_result.consumed++;
    }
    g_result.checksum = sum;
    g_result.expected_checksum = (DEMO_N * (DEMO_N + 1) * (2 * DEMO_N + 1)) / 6;
    g_result.ok = g_result.consumed == DEMO_N && sum == g_result.expected_checksum;
    g_result.done = 1;
    fx_println(f, 1, "[sink] checksum=%llu expected=%llu", sum,
               g_result.expected_checksum);
    fx_println(f, 1, "[sink] authority: %llu write attempts denied (EPERM)",
               g_result.denied);
    fx_println(f, 1, "[sink] pipeline %s", g_result.ok ? "OK" : "FAILED");
}

static const fprogram_t g_producer = { "producer", demo_producer, 1200 };
static const fprogram_t g_transformer = { "transform", demo_transformer, 1600 };
static const fprogram_t g_sink = { "sink", demo_sink, 1600 };

static fret_t install(fflow_t *f, fhandle_t h, uint32_t rights, int *index)
{
    int cap = finte_cap_install(f, h, rights);
    if (cap < 0) {
        return finte_ret(cap, 0);
    }
    if (index) {
        *index = cap;
    }
    return finte_ret_ok((uint64_t)cap);
}

fret_t finte_demo_bootstrap(void)
{
    finte_memset(&g_result, 0, sizeof(g_result));
    if (!finte_is_initialized()) {
        return finte_ret(FINTE_ERR_STATE, 0);
    }

    fhandle_t console = finte_console_create("console");
    fhandle_t raw = finte_channel_create("raw", 4);
    fhandle_t out = finte_channel_create("out", 4);
    if (!console || !raw || !out) {
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }

    fhandle_t ph = finte_flow_create("producer", &g_producer, 1200,
                                     FINTE_PERSONA_NATIVE);
    fhandle_t th = finte_flow_create("transform", &g_transformer, 1600,
                                     FINTE_PERSONA_NATIVE);
    fhandle_t sh = finte_flow_create("sink", &g_sink, 1600,
                                     FINTE_PERSONA_NATIVE);
    if (!ph || !th || !sh) {
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }
    fflow_t *p = finte_flow_lookup(ph);
    fflow_t *t = finte_flow_lookup(th);
    fflow_t *s = finte_flow_lookup(sh);
    if (!p || !t || !s) {
        return finte_ret(FINTE_ERR_NOENT, 0);
    }

    /* Producer: raw[SEND], console[READ only]. */
    if (finte_failed(install(p, raw, FINTE_R_SEND, NULL)) ||
        finte_failed(install(p, console, FINTE_R_READ, NULL))) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    /* Transformer: raw[RECV], out[SEND], console[READ only]. */
    if (finte_failed(install(t, raw, FINTE_R_RECV, NULL)) ||
        finte_failed(install(t, out, FINTE_R_SEND, NULL)) ||
        finte_failed(install(t, console, FINTE_R_READ, NULL))) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    /* Sink: out[RECV], console[WRITE]. */
    if (finte_failed(install(s, out, FINTE_R_RECV, NULL)) ||
        finte_failed(install(s, console, FINTE_R_WRITE, NULL))) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }

    if (finte_failed(finte_flow_start(ph)) ||
        finte_failed(finte_flow_start(th)) ||
        finte_failed(finte_flow_start(sh))) {
        return finte_ret(FINTE_ERR_STATE, 0);
    }
    return finte_ret_ok(0);
}

const fdemo_result_t *finte_demo_result(void)
{
    return &g_result;
}

fret_t finte_compat_bootstrap(void)
{
    static const char hello[] = "hello from the Finte ramfs\n";
    fhandle_t console = finte_console_create("console");
    fhandle_t dir = finte_dir_create("/");
    fhandle_t file = finte_file_create("hello.txt", hello, sizeof(hello) - 1);
    fhandle_t arena = finte_arena_create("linux-heap", 2 * 1024 * 1024);
    if (!console || !dir || !file || !arena) {
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }
    if (finte_failed(finte_dir_add(dir, "hello.txt", file))) {
        return finte_ret(FINTE_ERR_STATE, 0);
    }

    fhandle_t gh = finte_flow_create("linux-guest", &finte_linux_guest_program,
                                     8192, FINTE_PERSONA_LINUX);
    fflow_t *g = finte_flow_lookup(gh);
    if (!g) {
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }
    int cc = finte_cap_install(g, console, FINTE_R_WRITE | FINTE_R_READ);
    int dc = finte_cap_install(g, dir, FINTE_R_READ);
    int ac = finte_cap_install(g, arena, FINTE_R_WRITE);
    if (cc < 0 || dc < 0 || ac < 0) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    fret_t r = finte_linux_bootstrap(g, (uint16_t)cc, (uint16_t)dc, (uint16_t)ac);
    if (finte_failed(r)) {
        return r;
    }
    return finte_flow_start(gh);
}
