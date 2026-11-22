#include "finte/verb.h"

#define PTR(x) ((void *)(uintptr_t)(x))

static fret_t arena_alloc(fflow_t *f, uint16_t cap, size_t size, size_t align,
                          uint64_t *offset)
{
    fcap_t c;
    int rc = finte_cap_check(f, cap, FINTE_R_WRITE, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_ARENA) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    farena_t *a = &finte_core.arenas[n->index];
    if (c.flags & FINTE_CAP_SCOPED) {
        return finte_arena_alloc_scoped(a, c.offset, c.length, size, align,
                                        offset);
    }
    return finte_arena_alloc(a, size, align, offset);
}

typedef fret_t (*verb_handler_t)(fflow_t *f, const uint64_t args[6]);

static fret_t verb_nop(fflow_t *f, const uint64_t args[6])
{
    (void)f;
    (void)args;
    return finte_ret_ok(0);
}

static fret_t verb_sys_info(fflow_t *f, const uint64_t args[6])
{
    (void)f;
    (void)args;
    return finte_ret_ok(((uint64_t)FINTE_VERSION_MAJOR << 16) |
                        ((uint64_t)FINTE_VERSION_MINOR << 8) |
                        FINTE_VERSION_PATCH);
}

static fret_t verb_cap_info(fflow_t *f, const uint64_t args[6])
{
    fcap_info_t *out = (fcap_info_t *)PTR(args[1]);
    if (!out) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], 0, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    out->type = n ? n->type : FINTE_NODE_FREE;
    out->rights = c.rights;
    out->handle = c.handle;
    out->aux = n ? n->aux : 0;
    out->offset = c.offset;
    out->length = c.length;
    out->flags = c.flags;
    out->pad = 0;
    return finte_ret_ok(0);
}

static fret_t verb_cap_dup(fflow_t *f, const uint64_t args[6])
{
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], 0, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    uint32_t rights = (uint32_t)args[1] ? (uint32_t)args[1] : c.rights;
    if ((rights & ~c.rights) != 0) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    c.rights = rights;
    int slot = finte_cap_install_cap(f, &c);
    return slot < 0 ? finte_ret(slot, 0) : finte_ret_ok((uint64_t)slot);
}

static fret_t verb_cap_drop(fflow_t *f, const uint64_t args[6])
{
    int rc = finte_cap_drop_index(f, (uint16_t)args[0]);
    return rc < 0 ? finte_ret(rc, 0) : finte_ret_ok(0);
}

static fret_t verb_cap_grant(fflow_t *f, const uint64_t args[6])
{
    fcap_t target;
    int rc = finte_cap_check(f, (uint16_t)args[0], FINTE_R_GRANT, &target);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *tn = finte_node_get(target.handle);
    if (!tn || tn->type != FINTE_NODE_FLOW) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    fflow_t *to = finte_flow_lookup(target.handle);
    if (!to || !to->in_use || to->state == FINTE_FLOW_DEAD) {
        return finte_ret(FINTE_ERR_NOENT, 0);
    }
    fcap_t src;
    rc = finte_cap_check(f, (uint16_t)args[1], 0, &src);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    uint32_t rights = (uint32_t)args[2] ? (uint32_t)args[2] : src.rights;
    if ((rights & ~src.rights) != 0) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    src.rights = rights;
    int slot = finte_cap_install_cap(to, &src);
    return slot < 0 ? finte_ret(slot, 0) : finte_ret_ok((uint64_t)slot);
}

static fret_t verb_chan_send(fflow_t *f, const uint64_t args[6])
{
    fmsg_t *msg = (fmsg_t *)PTR(args[1]);
    if (!msg) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    return finte_chan_send(f, (uint16_t)args[0], msg, (uint32_t)args[2]);
}

static fret_t verb_chan_recv(fflow_t *f, const uint64_t args[6])
{
    fmsg_t *msg = (fmsg_t *)PTR(args[1]);
    if (!msg) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    return finte_chan_recv(f, (uint16_t)args[0], msg, (uint32_t)args[2]);
}

static fret_t verb_chan_info(fflow_t *f, const uint64_t args[6])
{
    fchan_info_t *out = (fchan_info_t *)PTR(args[1]);
    if (!out) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], 0, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_CHANNEL) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    fchan_t *ch = &finte_core.channels[n->index];
    finte_memset(out, 0, sizeof(*out));
    out->handle = c.handle;
    out->capacity = ch->capacity;
    out->count = ch->count;
    out->closed = ch->closed;
    out->sent = ch->sent;
    out->received = ch->received;
    for (fthread_t *w = ch->wait_send; w; w = w->wait_next) {
        out->waiting_send++;
    }
    for (fthread_t *w = ch->wait_recv; w; w = w->wait_next) {
        out->waiting_recv++;
    }
    finte_strlcpy(out->name, ch->name, sizeof(out->name));
    return finte_ret_ok(0);
}

static fret_t verb_chan_close(fflow_t *f, const uint64_t args[6])
{
    return finte_chan_close(f, (uint16_t)args[0]);
}

static fret_t verb_flow_yield(fflow_t *f, const uint64_t args[6])
{
    (void)args;
    finte_flow_yield(f);
    return finte_ret_ok(0);
}

static fret_t verb_flow_exit(fflow_t *f, const uint64_t args[6])
{
    finte_flow_exit(f, (int)args[0]);
    return finte_ret(FINTE_ERR_STATE, 0);
}

static fret_t verb_flow_info(fflow_t *f, const uint64_t args[6])
{
    fflow_info_t *out = (fflow_info_t *)PTR(args[0]);
    if (!out) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    finte_memset(out, 0, sizeof(*out));
    out->handle = f->self;
    out->state = f->state;
    out->persona = f->persona;
    out->fuel = f->fuel;
    out->fuel_max = f->fuel_max;
    out->caps = f->cap_count;
    finte_strlcpy(out->name, f->name, sizeof(out->name));
    return finte_ret_ok(0);
}

static fret_t verb_flow_fuel(fflow_t *f, const uint64_t args[6])
{
    uint32_t *out = (uint32_t *)PTR(args[0]);
    if (!out) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    *out = f->fuel;
    return finte_ret_ok(0);
}

static fret_t verb_arena_alloc(fflow_t *f, const uint64_t args[6])
{
    uint64_t *out = (uint64_t *)PTR(args[3]);
    if (!out) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    return arena_alloc(f, (uint16_t)args[0], (size_t)args[1],
                       (size_t)args[2], out);
}

static fret_t verb_arena_free(fflow_t *f, const uint64_t args[6])
{
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], FINTE_R_WRITE, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_ARENA) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    if ((c.flags & FINTE_CAP_SCOPED) &&
        (args[1] < c.offset || args[1] >= c.offset + c.length)) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    if (c.flags & FINTE_CAP_SCOPED) {
        return finte_arena_free_scoped(&finte_core.arenas[n->index], args[1], 1);
    }
    return finte_arena_free(&finte_core.arenas[n->index], args[1]);
}

static fret_t verb_console_write(fflow_t *f, const uint64_t args[6])
{
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], FINTE_R_WRITE, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_CONSOLE) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    const char *buf = (const char *)PTR(args[1]);
    size_t len = (size_t)args[2];
    if (!buf && len) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    hal_console_write(buf, len);
    finte_core.stats.console_bytes += len;
    return finte_ret_ok(len);
}

static fret_t verb_file_read(fflow_t *f, const uint64_t args[6])
{
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], FINTE_R_READ, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_FILE) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    ffile_t *file = &finte_core.files[n->index];
    uint64_t off = args[1];
    void *buf = PTR(args[2]);
    size_t len = (size_t)args[3];
    if (!buf && len) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    if (file->backend) {
        int64_t got = finte_disk_read(file->path, off, buf, len);
        if (got < 0) {
            return finte_ret((int32_t)got, 0);
        }
        return finte_ret_ok((uint64_t)got);
    }
    if (off >= file->size) {
        return finte_ret_ok(0);
    }
    if (len > file->size - off) {
        len = file->size - (size_t)off;
    }
    finte_memcpy(buf, file->data + off, len);
    return finte_ret_ok(len);
}

static fret_t verb_file_write(fflow_t *f, const uint64_t args[6])
{
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], FINTE_R_WRITE, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_FILE) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    ffile_t *file = &finte_core.files[n->index];
    if (!file->backend) {
        return finte_ret(FINTE_ERR_PERM, 0);
    }
    const void *buf = PTR(args[2]);
    size_t len = (size_t)args[3];
    if (!buf && len) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    int64_t put = finte_disk_write(file->path, args[1], buf, len);
    if (put < 0) {
        return finte_ret((int32_t)put, 0);
    }
    return finte_ret_ok((uint64_t)put);
}

static fret_t verb_clock_now(fflow_t *f, const uint64_t args[6])
{
    (void)f;
    uint64_t *out = (uint64_t *)PTR(args[0]);
    if (!out) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    *out = hal_now_ns();
    return finte_ret_ok(*out);
}

static fret_t verb_sys_stats(fflow_t *f, const uint64_t args[6])
{
    (void)f;
    fstats_t *out = (fstats_t *)PTR(args[0]);
    if (!out) {
        return finte_ret(FINTE_ERR_FAULT, 0);
    }
    *out = finte_get_stats();
    return finte_ret_ok(0);
}

static fret_t verb_sys_dump(fflow_t *f, const uint64_t args[6])
{
    (void)args;
    finte_dump(f->name);
    return finte_ret_ok(0);
}

static fret_t verb_flow_wait(fflow_t *f, const uint64_t args[6])
{
    fcap_t c;
    int rc = finte_cap_check(f, (uint16_t)args[0], FINTE_R_CONTROL, &c);
    if (rc < 0) {
        return finte_ret(rc, 0);
    }
    fnode_t *n = finte_node_get(c.handle);
    if (!n || n->type != FINTE_NODE_FLOW) {
        return finte_ret(FINTE_ERR_TYPE, 0);
    }
    fflow_t *target = finte_flow_lookup(c.handle);
    if (!target || target->state == FINTE_FLOW_DEAD) {
        return finte_ret_ok(0);
    }
    if (target == f || !target->started) {
        return finte_ret(FINTE_ERR_STATE, 0);
    }
    finte_flow_wait_for(f, target);
    return finte_ret_ok(0);
}

/* Indexed by FINTE_VERB_*; sparse slots are rejected. */
static const verb_handler_t verb_table[FINTE_VERB__COUNT] = {
    [FINTE_VERB_NOP] = verb_nop,
    [FINTE_VERB_SYS_INFO] = verb_sys_info,
    [FINTE_VERB_CAP_INFO] = verb_cap_info,
    [FINTE_VERB_CAP_DUP] = verb_cap_dup,
    [FINTE_VERB_CAP_DROP] = verb_cap_drop,
    [FINTE_VERB_CAP_GRANT] = verb_cap_grant,
    [FINTE_VERB_CHAN_SEND] = verb_chan_send,
    [FINTE_VERB_CHAN_RECV] = verb_chan_recv,
    [FINTE_VERB_CHAN_INFO] = verb_chan_info,
    [FINTE_VERB_FLOW_YIELD] = verb_flow_yield,
    [FINTE_VERB_FLOW_EXIT] = verb_flow_exit,
    [FINTE_VERB_FLOW_INFO] = verb_flow_info,
    [FINTE_VERB_FLOW_FUEL] = verb_flow_fuel,
    [FINTE_VERB_ARENA_ALLOC] = verb_arena_alloc,
    [FINTE_VERB_ARENA_FREE] = verb_arena_free,
    [FINTE_VERB_CONSOLE_WRITE] = verb_console_write,
    [FINTE_VERB_FILE_READ] = verb_file_read,
    [FINTE_VERB_CLOCK_NOW] = verb_clock_now,
    [FINTE_VERB_SYS_STATS] = verb_sys_stats,
    [FINTE_VERB_SYS_DUMP] = verb_sys_dump,
    [FINTE_VERB_FLOW_WAIT] = verb_flow_wait,
    [FINTE_VERB_CHAN_CLOSE] = verb_chan_close,
    [FINTE_VERB_FILE_WRITE] = verb_file_write,
};

static fret_t syscall_dispatch(fflow_t *f, uint32_t nr,
                               const uint64_t args[6])
{
    if (!f) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    finte_core.stats.syscalls++;
    f->stats.syscalls++;

    if (nr >= FINTE_VERB__COUNT || !verb_table[nr]) {
        return finte_ret(FINTE_ERR_NOSYS, 0);
    }
    return verb_table[nr](f, args);
}

/* A verb may manipulate shared queues, so keep timer preemption out of the
 * dispatcher. Flows are still preempted between verbs, while they compute. */
fret_t finte_syscall(fflow_t *f, uint32_t nr, const uint64_t args[6])
{
    finte_core_lock();
    fret_t r = syscall_dispatch(f, nr, args);
    finte_core_unlock();
    return r;
}

fret_t fx_send_msg(fflow_t *f, uint16_t cap, fmsg_t *msg, uint32_t flags)
{
    uint64_t args[6] = { cap, (uint64_t)(uintptr_t)msg, flags, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CHAN_SEND, args);
}

fret_t fx_recv_msg(fflow_t *f, uint16_t cap, fmsg_t *out, uint32_t flags)
{
    uint64_t args[6] = { cap, (uint64_t)(uintptr_t)out, flags, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CHAN_RECV, args);
}

fret_t fx_send(fflow_t *f, uint16_t cap, uint32_t tag,
               const void *data, uint16_t len)
{
    if (len > FINTE_MSG_DATA) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    fmsg_t msg;
    finte_memset(&msg, 0, sizeof(msg));
    msg.tag = tag;
    msg.len = len;
    if (data && len) {
        finte_memcpy(msg.data, data, len);
    }
    return fx_send_msg(f, cap, &msg, 0);
}

fret_t fx_try_send(fflow_t *f, uint16_t cap, uint32_t tag,
                   const void *data, uint16_t len)
{
    if (len > FINTE_MSG_DATA) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    fmsg_t msg;
    finte_memset(&msg, 0, sizeof(msg));
    msg.tag = tag;
    msg.len = len;
    if (data && len) {
        finte_memcpy(msg.data, data, len);
    }
    return fx_send_msg(f, cap, &msg, FINTE_MSG_NONBLOCK);
}

fret_t fx_recv(fflow_t *f, uint16_t cap, fmsg_t *out)
{
    return fx_recv_msg(f, cap, out, 0);
}

fret_t fx_try_recv(fflow_t *f, uint16_t cap, fmsg_t *out)
{
    return fx_recv_msg(f, cap, out, FINTE_MSG_NONBLOCK);
}

fret_t fx_console_write(fflow_t *f, uint16_t cap, const void *buf, size_t len)
{
    uint64_t args[6] = { cap, (uint64_t)(uintptr_t)buf, len, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CONSOLE_WRITE, args);
}

fret_t fx_cap_info(fflow_t *f, uint16_t cap, fcap_info_t *out)
{
    uint64_t args[6] = { cap, (uint64_t)(uintptr_t)out, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CAP_INFO, args);
}

fret_t fx_cap_dup(fflow_t *f, uint16_t cap, uint32_t rights)
{
    uint64_t args[6] = { cap, rights, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CAP_DUP, args);
}

fret_t fx_cap_drop(fflow_t *f, uint16_t cap)
{
    uint64_t args[6] = { cap, 0, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CAP_DROP, args);
}

fret_t fx_cap_grant(fflow_t *f, uint16_t flow_cap, uint16_t src_cap,
                    uint32_t rights)
{
    uint64_t args[6] = { flow_cap, src_cap, rights, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CAP_GRANT, args);
}

fret_t fx_flow_info(fflow_t *f, fflow_info_t *out)
{
    uint64_t args[6] = { (uint64_t)(uintptr_t)out, 0, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_FLOW_INFO, args);
}

fret_t fx_flow_fuel(fflow_t *f, uint32_t *out)
{
    uint64_t args[6] = { (uint64_t)(uintptr_t)out, 0, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_FLOW_FUEL, args);
}

fret_t fx_flow_wait(fflow_t *f, uint16_t flow_cap)
{
    uint64_t args[6] = { flow_cap, 0, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_FLOW_WAIT, args);
}

fret_t fx_chan_info(fflow_t *f, uint16_t cap, fchan_info_t *out)
{
    uint64_t args[6] = { cap, (uint64_t)(uintptr_t)out, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CHAN_INFO, args);
}

fret_t fx_chan_close(fflow_t *f, uint16_t cap)
{
    uint64_t args[6] = { cap, 0, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CHAN_CLOSE, args);
}

fret_t fx_arena_alloc(fflow_t *f, uint16_t cap, size_t size, size_t align,
                      uint64_t *offset)
{
    uint64_t args[6] = { cap, size, align, (uint64_t)(uintptr_t)offset, 0, 0 };
    return finte_syscall(f, FINTE_VERB_ARENA_ALLOC, args);
}

fret_t fx_arena_free(fflow_t *f, uint16_t cap, uint64_t offset)
{
    uint64_t args[6] = { cap, offset, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_ARENA_FREE, args);
}

fret_t fx_file_read(fflow_t *f, uint16_t cap, uint64_t offset,
                    void *buf, size_t len)
{
    uint64_t args[6] = { cap, offset, (uint64_t)(uintptr_t)buf, len, 0, 0 };
    return finte_syscall(f, FINTE_VERB_FILE_READ, args);
}

fret_t fx_file_write(fflow_t *f, uint16_t cap, uint64_t offset,
                     const void *buf, size_t len)
{
    uint64_t args[6] = { cap, offset, (uint64_t)(uintptr_t)buf, len, 0, 0 };
    return finte_syscall(f, FINTE_VERB_FILE_WRITE, args);
}

fret_t fx_clock_now(fflow_t *f, uint64_t *ns)
{
    uint64_t args[6] = { (uint64_t)(uintptr_t)ns, 0, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_CLOCK_NOW, args);
}

fret_t fx_sys_stats(fflow_t *f, fstats_t *out)
{
    uint64_t args[6] = { (uint64_t)(uintptr_t)out, 0, 0, 0, 0, 0 };
    return finte_syscall(f, FINTE_VERB_SYS_STATS, args);
}

void fx_yield(fflow_t *f)
{
    (void)finte_syscall(f, FINTE_VERB_FLOW_YIELD, NULL);
}

void fx_sleep_ns(fflow_t *f, uint64_t ns)
{
    finte_flow_sleep(f, ns);
}

void fx_print(fflow_t *f, uint16_t console_cap, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        fx_console_write(f, console_cap, buf, (size_t)n);
    }
}

void fx_println(fflow_t *f, uint16_t console_cap, const char *fmt, ...)
{
    char buf[260];
    va_list ap;
    va_start(ap, fmt);
    int n = finte_vformat(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 2) {
        n = (int)sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    fx_console_write(f, console_cap, buf, (size_t)n);
}
