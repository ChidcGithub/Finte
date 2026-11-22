#include "finte/object.h"
#include "finte/process.h"
#include "internal.h"

#define FINTE_FLOW_STACK (32u * 1024u)

static uint32_t g_next_cpu;

static fthread_t *thread_alloc(fflow_t *f)
{
    uint32_t start = f->slot % FINTE_MAX_THREADS;
    for (uint32_t k = 0; k < FINTE_MAX_THREADS; k++) {
        uint32_t i = (start + k) % FINTE_MAX_THREADS;
        fthread_t *t = &finte_core.threads[i];
        if (t->in_use) {
            continue;
        }
        finte_memset(t, 0, sizeof(*t));
        t->in_use = 1;
        t->state = FINTE_THREAD_CREATED;
        t->slot = (uint16_t)i;
        t->flow = f;
        t->priority = 0;
        t->quantum = FINTE_FUEL_QUANTUM;
        uint32_t ncpu = hal_cpu_count();
        if (ncpu == 0) {
            ncpu = 1;
        }
        t->cpu = (g_next_cpu++) % ncpu;
        return t;
    }
    return NULL;
}

void finte_thread_release(fthread_t *t)
{
    if (!t || !t->in_use) {
        return;
    }
    if (t->release_space) {
        finte_as_unref(t->release_space);
        t->release_space = NULL;
    }
    if (t->ctx) {
        hal_ctx_destroy(t->ctx);
        t->ctx = NULL;
    }
    finte_memset(t, 0, sizeof(*t));
    t->state = FINTE_THREAD_FREE;
}

static void flow_entry_trampoline(void *arg)
{
    fflow_t *f = (fflow_t *)arg;
    if (f->thread) {
        f->thread->state = FINTE_THREAD_RUNNING;
    }
    f->state = FINTE_FLOW_RUNNING;
    if (f->prog && f->prog->entry) {
        f->prog->entry(f);
    }
    finte_flow_exit(f, 0);
}

fhandle_t finte_flow_create(const char *name, const fprogram_t *prog,
                            uint32_t fuel, uint8_t persona)
{
    fcpu_t *cpu0 = hal_cpu_self();
    uint32_t start = cpu0->flow_hint % FINTE_MAX_FLOWS;
    for (uint32_t k = 0; k < FINTE_MAX_FLOWS; k++) {
        uint32_t i = (start + k) % FINTE_MAX_FLOWS;
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
        f->thread = thread_alloc(f);
        if (!f->thread) {
            /* refs==0, so this releases the flow node as well. */
            finte_node_unref(h);
            return FINTE_HANDLE_NONE;
        }
        finte_strlcpy(f->name, name ? name : (prog && prog->name ? prog->name : "flow"),
                      sizeof(f->name));
        for (int c = 0; c < FINTE_MAX_CAPS; c++) {
            f->linux.fd_cap[c] = -1;
        }
        cpu0->flow_hint = (i + 1u) % FINTE_MAX_FLOWS;
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
    fthread_t *t = f->thread;
    if (!t || t->state != FINTE_THREAD_CREATED) {
        return finte_ret(FINTE_ERR_STATE, 0);
    }
    finte_core_lock();
    if (f->started) {
        finte_core_unlock();
        return finte_ret(FINTE_ERR_STATE, 0);
    }
    if (!t->ctx) {
        t->ctx = hal_ctx_create(flow_entry_trampoline, f, FINTE_FLOW_STACK);
    }
    if (!t->ctx) {
        finte_core_unlock();
        return finte_ret(FINTE_ERR_NOMEM, 0);
    }
    /* Bind the user root before the first activation. The flow entry also
     * sets this, but hal_ctx_switch needs it to bind the TSS stack and CR3
     * on the very first switch into the flow. */
    if (f->space) {
        t->ctx->addr_space = finte_as_root(f->space);
    }
    finte_node_ref(f->self); /* scheduler ownership */
    f->started = 1;
    f->state = FINTE_FLOW_READY;
    t->state = FINTE_THREAD_READY;
    finte_core.stats.flows_started++;
    finte_sched_enqueue_thread(t);
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

fthread_t *finte_thread_current(void)
{
    return hal_cpu_self()->current_thread;
}

void finte_flow_yield(fflow_t *f)
{
    fthread_t *t = f ? f->thread : NULL;
    if (!f || !t || !t->ctx || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    f->state = FINTE_FLOW_READY;
    t->state = FINTE_THREAD_READY;
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(t->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;
    finte_core_unlock();
}

void finte_flow_sleep(fflow_t *f, uint64_t ns)
{
    fthread_t *t = f ? f->thread : NULL;
    if (!f || !t || !t->ctx || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    t->blocking = 1;
    finte_sched_sleep_thread(t, hal_now_ns() + ns);
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(t->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    t->blocking = 0;
    t->wake_pending = 0;
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;
    finte_core_unlock();
}

void finte_flow_block(fflow_t *f, fchan_t *ch, int is_send)
{
    fthread_t *t = f ? f->thread : NULL;
    if (!f || !t || !ch || !t->ctx || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;
    t->blocking = 1;
    t->wait_chan = ch;
    t->wait_send = (uint8_t)(is_send ? 1 : 0);
    t->wait_next = NULL;
    fthread_t **head = is_send ? &ch->wait_send : &ch->wait_recv;
    if (!*head) {
        *head = t;
    } else {
        fthread_t *it = *head;
        while (it->wait_next) {
            it = it->wait_next;
        }
        it->wait_next = t;
    }
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(t->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    t->blocking = 0;
    t->wake_pending = 0;
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;
    finte_core_unlock();
}

void finte_flow_wait_for(fflow_t *f, fflow_t *target)
{
    fthread_t *t = f ? f->thread : NULL;
    if (!f || !t || !target || target == f || !t->ctx ||
        !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    if (target->state == FINTE_FLOW_DEAD || target->state == FINTE_FLOW_FREE) {
        finte_core_unlock();
        return;
    }
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;
    t->blocking = 1;
    t->wait_chan = NULL;
    t->wait_send = 0;
    t->wait_next = target->waiters;
    target->waiters = t;
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(t->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    t->blocking = 0;
    t->wake_pending = 0;
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;
    finte_core_unlock();
}

void finte_flow_stop(fflow_t *f)
{
    fthread_t *t = f ? f->thread : NULL;
    if (!f || !t || !t->ctx || !hal_cpu_self()->sched_ctx) {
        return;
    }
    finte_core_lock();
    if (!f->user || !((fproc_t *)f->user)->stopped) {
        finte_core_unlock();
        return;
    }
    ((fproc_t *)f->user)->stop_acked = 1;
    t->blocking = 1;
    finte_sched_sleep_thread(t, hal_now_ns() + (1ull << 62));
    uint32_t depth = finte_core_lock_suspend();
    hal_ctx_switch(t->ctx, hal_cpu_self()->sched_ctx);
    finte_core_lock_resume(depth);
    t->blocking = 0;
    t->wake_pending = 0;
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;
    finte_core_unlock();
}

void finte_flow_exit(fflow_t *f, int code)
{
    if (!f || !f->thread) {
        hal_panic("flow exit without thread");
    }
    fthread_t *t = f->thread;
    finte_core_lock();
    f->state = FINTE_FLOW_DEAD;
    t->state = FINTE_THREAD_DEAD;
    f->exit_code = (uint32_t)code;
    /* The scheduler releases the address space after switching to the
     * scheduler context, so the page tables are never freed while active. */
    if (f->space) {
        t->release_space = f->space;
        f->space = NULL;
    }
    if (f->user) {
        fproc_t *p = (fproc_t *)f->user;
        p->space = NULL;
        p->flow = NULL;
        p->exit_code = (uint32_t)code;
        f->user = NULL;
        if (p->parent && p->parent->flow) {
            p->parent->sigs[FPROC_SIGCHLD].pending = 1;
            if (p->parent->flow->thread) {
                finte_sched_wake_thread(p->parent->flow->thread);
            }
        }
    }

    /* Wake flows that are waiting for this one to finish. */
    fthread_t *w = f->waiters;
    f->waiters = NULL;
    while (w) {
        fthread_t *next = w->wait_next;
        w->wait_next = NULL;
        w->wait_chan = NULL;
        w->wait_send = 0;
        finte_sched_wake_thread(w);
        w = next;
    }

    finte_cap_drop_all(f);
    (void)finte_core_lock_suspend();
    if (hal_cpu_self()->sched_ctx && t->ctx) {
        hal_ctx_switch(t->ctx, hal_cpu_self()->sched_ctx);
    }
    /* A finished flow must never be resumed. */
    hal_panic("dead flow resumed");
}
