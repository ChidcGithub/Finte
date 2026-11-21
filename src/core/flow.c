#include "finte/object.h"
#include "internal.h"

#define FINTE_FLOW_STACK (32u * 1024u)

static uint32_t g_next_cpu;

static void flow_entry_trampoline(void *arg)
{
    fflow_t *f = (fflow_t *)arg;
    f->state = FINTE_FLOW_RUNNING;
    if (f->prog && f->prog->entry) {
        f->prog->entry(f);
    }
    finte_flow_exit(f, 0);
}

fhandle_t finte_flow_create(const char *name, const fprogram_t *prog,
                            uint32_t fuel, uint8_t persona)
{
    for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
        fflow_t *f = &finte_core.flows[i];
        if (f->in_use) {
            continue;
        }
        fhandle_t h = finte_node_alloc(FINTE_NODE_FLOW, i, 0);
        if (!h) {
            return FINTE_HANDLE_NONE;
        }
        finte_memset(f, 0, sizeof(*f));
        f->in_use = 1;
        f->state = FINTE_FLOW_FREE;
        f->slot = (uint16_t)i;
        f->self = h;
        f->prog = prog;
        f->persona = persona;
        f->fuel_max = fuel ? fuel : 1024;
        f->fuel = f->fuel_max;
        f->quantum = FINTE_FUEL_QUANTUM;
        uint32_t ncpu = hal_cpu_count();
        if (ncpu == 0) {
            ncpu = 1;
        }
        f->cpu = (g_next_cpu++) % ncpu;
        finte_strlcpy(f->name, name ? name : (prog && prog->name ? prog->name : "flow"),
                      sizeof(f->name));
        for (int c = 0; c < FINTE_MAX_CAPS; c++) {
            f->linux.fd_cap[c] = -1;
        }
        return h;
    }
    return FINTE_HANDLE_NONE;
}

fret_t finte_flow_start(fhandle_t flow)
{
    fflow_t *f = finte_flow_lookup(flow);
    if (!f) {
        return finte_ret(FINTE_ERR_NOENT, 0);
    }
    if (f->started) {
        return finte_ret(FINTE_ERR_STATE, 0);
    }
    finte_core_lock();
    f->ctx = hal_ctx_create(flow_entry_trampoline, f, FINTE_FLOW_STACK);
    if (!f->ctx) {
        finte_core_unlock();
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }
    finte_node_ref(f->self); /* scheduler ownership */
    f->started = 1;
    f->state = FINTE_FLOW_READY;
    finte_core.stats.flows_started++;
    finte_sched_enqueue(f);
    finte_core_unlock();
    return finte_ret_ok(0);
}

fhandle_t finte_flow_handle(fflow_t *f)
{
    return f ? f->self : FINTE_HANDLE_NONE;
}

fflow_t *finte_flow_lookup(fhandle_t flow)
{
    fnode_t *n = finte_node_get(flow);
    if (!n || n->type != FINTE_NODE_FLOW) {
        return NULL;
    }
    fflow_t *f = &finte_core.flows[n->index];
    return f->in_use ? f : NULL;
}

int finte_flow_alive(fhandle_t flow)
{
    fflow_t *f = finte_flow_lookup(flow);
    return f && f->state != FINTE_FLOW_DEAD && f->state != FINTE_FLOW_FREE;
}

fflow_t *finte_flow_current(void)
{
    return hal_cpu_self()->current;
}

void finte_flow_yield(fflow_t *f)
{
    if (!f || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    f->state = FINTE_FLOW_READY;
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(f->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    f->state = FINTE_FLOW_RUNNING;
    finte_core_unlock();
}

void finte_flow_sleep(fflow_t *f, uint64_t ns)
{
    if (!f || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    finte_sched_sleep(f, hal_now_ns() + ns);
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(f->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    f->state = FINTE_FLOW_RUNNING;
    finte_core_unlock();
}

void finte_flow_block(fflow_t *f, fchan_t *ch, int is_send)
{
    if (!f || !ch || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    f->state = FINTE_FLOW_BLOCKED;
    f->wait_chan = ch;
    f->wait_send = (uint8_t)(is_send ? 1 : 0);
    f->wait_next = NULL;
    fflow_t **head = is_send ? &ch->wait_send : &ch->wait_recv;
    if (!*head) {
        *head = f;
    } else {
        fflow_t *it = *head;
        while (it->wait_next) {
            it = it->wait_next;
        }
        it->wait_next = f;
    }
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(f->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    f->state = FINTE_FLOW_RUNNING;
    finte_core_unlock();
}

void finte_flow_wait_for(fflow_t *f, fflow_t *target)
{
    if (!f || !target || target == f || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    if (target->state == FINTE_FLOW_DEAD || target->state == FINTE_FLOW_FREE) {
        finte_core_unlock();
        return;
    }
    f->state = FINTE_FLOW_BLOCKED;
    f->wait_chan = NULL;
    f->wait_send = 0;
    f->wait_next = target->waiters;
    target->waiters = f;
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(f->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    f->state = FINTE_FLOW_RUNNING;
    finte_core_unlock();
}

void finte_flow_exit(fflow_t *f, int code)
{
    if (!f) {
        hal_panic("flow exit without flow");
    }
    finte_core_lock();
    f->state = FINTE_FLOW_DEAD;
    f->exit_code = (uint32_t)code;

    /* Wake flows that are waiting for this one to finish. */
    fflow_t *w = f->waiters;
    f->waiters = NULL;
    while (w) {
        fflow_t *next = w->wait_next;
        w->wait_next = NULL;
        w->wait_chan = NULL;
        w->wait_send = 0;
        finte_sched_wake(w);
        w = next;
    }

    finte_cap_drop_all(f);
    (void)finte_core_lock_suspend();
    if (hal_cpu_self()->sched_ctx) {
        hal_ctx_switch(f->ctx, hal_cpu_self()->sched_ctx);
    }
    /* A finished flow must never be resumed. */
    hal_panic("dead flow resumed");
}
