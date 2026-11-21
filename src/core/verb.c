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
    fret_t r = finte_arena_alloc(a, size, align, offset);
    return r;
}

static fret_t syscall_dispatch(fflow_t *f, uint32_t nr,
                               const uint64_t args[6])
{
    if (!f) {
        return finte_ret(FINTE_ERR_INVAL, 0);
    }
    finte_core.stats.syscalls++;
    f->stats.syscalls++;

    switch (nr) {
    case FINTE_VERB_NOP:
        return finte_ret_ok(0);

    case FINTE_VERB_SYS_INFO:
        return finte_ret_ok(((uint64_t)FINTE_VERSION_MAJOR << 16) |
                            ((uint64_t)FINTE_VERSION_MINOR << 8) |
                            FINTE_VERSION_PATCH);

    case FINTE_VERB_CAP_INFO: {
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
        return finte_ret_ok(0);
    }

    case FINTE_VERB_CAP_DUP: {
        fcap_t c;
        int rc = finte_cap_check(f, (uint16_t)args[0], 0, &c);
        if (rc < 0) {
            return finte_ret(rc, 0);
        }
        uint32_t rights = (uint32_t)args[1] ? (uint32_t)args[1] : c.rights;
        if ((rights & ~c.rights) != 0) {
            return finte_ret(FINTE_ERR_PERM, 0);
        }
        int slot = finte_cap_install(f, c.handle, rights);
        return slot < 0 ? finte_ret(slot, 0) : finte_ret_ok((uint64_t)slot);
    }

    case FINTE_VERB_CAP_DROP: {
        int rc = finte_cap_drop_index(f, (uint16_t)args[0]);
        return rc < 0 ? finte_ret(rc, 0) : finte_ret_ok(0);
    }

    case FINTE_VERB_CAP_GRANT: {
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
        int slot = finte_cap_install(to, src.handle, rights);
        return slot < 0 ? finte_ret(slot, 0) : finte_ret_ok((uint64_t)slot);
    }

    case FINTE_VERB_CHAN_SEND: {
        fmsg_t *msg = (fmsg_t *)PTR(args[1]);
        if (!msg) {
            return finte_ret(FINTE_ERR_FAULT, 0);
        }
        return finte_chan_send(f, (uint16_t)args[0], msg, (uint32_t)args[2]);
    }

    case FINTE_VERB_CHAN_RECV: {
        fmsg_t *msg = (fmsg_t *)PTR(args[1]);
        if (!msg) {
            return finte_ret(FINTE_ERR_FAULT, 0);
        }
        return finte_chan_recv(f, (uint16_t)args[0], msg, (uint32_t)args[2]);
    }

    case FINTE_VERB_CHAN_INFO: {
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
        for (fflow_t *w = ch->wait_send; w; w = w->wait_next) {
            out->waiting_send++;
        }
        for (fflow_t *w = ch->wait_recv; w; w = w->wait_next) {
            out->waiting_recv++;
        }
        finte_strlcpy(out->name, ch->name, sizeof(out->name));
        return finte_ret_ok(0);
    }

    case FINTE_VERB_CHAN_CLOSE:
        return finte_chan_close(f, (uint16_t)args[0]);

    case FINTE_VERB_FLOW_YIELD:
        finte_flow_yield(f);
        return finte_ret_ok(0);

    case FINTE_VERB_FLOW_EXIT:
        finte_flow_exit(f, (int)args[0]);
        return finte_ret(FINTE_ERR_STATE, 0);

    case FINTE_VERB_FLOW_INFO: {
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

    case FINTE_VERB_FLOW_FUEL: {
        uint32_t *out = (uint32_t *)PTR(args[0]);
        if (!out) {
            return finte_ret(FINTE_ERR_FAULT, 0);
        }
        *out = f->fuel;
        return finte_ret_ok(0);
    }

    case FINTE_VERB_ARENA_ALLOC: {
        uint64_t *out = (uint64_t *)PTR(args[3]);
        if (!out) {
            return finte_ret(FINTE_ERR_FAULT, 0);
        }
        return arena_alloc(f, (uint16_t)args[0], (size_t)args[1],
                           (size_t)args[2], out);
    }

    case FINTE_VERB_ARENA_FREE: {
        fcap_t c;
        int rc = finte_cap_check(f, (uint16_t)args[0], FINTE_R_WRITE, &c);
        if (rc < 0) {
            return finte_ret(rc, 0);
        }
        fnode_t *n = finte_node_get(c.handle);
        if (!n || n->type != FINTE_NODE_ARENA) {
            return finte_ret(FINTE_ERR_TYPE, 0);
        }
        return finte_arena_free(&finte_core.arenas[n->index], args[1]);
    }

    case FINTE_VERB_CONSOLE_WRITE: {
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

    case FINTE_VERB_FILE_READ: {
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

    case FINTE_VERB_FILE_WRITE: {
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

    case FINTE_VERB_CLOCK_NOW: {
        uint64_t *out = (uint64_t *)PTR(args[0]);
        if (!out) {
            return finte_ret(FINTE_ERR_FAULT, 0);
        }
        *out = hal_now_ns();
        return finte_ret_ok(*out);
    }

    case FINTE_VERB_SYS_STATS: {
        fstats_t *out = (fstats_t *)PTR(args[0]);
        if (!out) {
            return finte_ret(FINTE_ERR_FAULT, 0);
        }
        *out = finte_get_stats();
        return finte_ret_ok(0);
    }

    case FINTE_VERB_SYS_DUMP:
        finte_dump(f->name);
        return finte_ret_ok(0);

    case FINTE_VERB_FLOW_WAIT: {
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

    default:
        return finte_ret(FINTE_ERR_NOSYS, 0);
    }
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
