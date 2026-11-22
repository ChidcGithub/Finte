/* Multi-core thread scheduler.
 *
 * Each Flow owns one primary Thread in the serial-only stage. Run queues and
 * their locks are per-CPU; a smaller state lock protects sleep/throttle lists
 * and blocked/ready transitions. An idle CPU can steal one ready Thread from a
 * busier queue (stealing is currently disabled while the wake/switch path is
 * validated). The BSP owns batch finalization, but every CPU can refill
 * sleepers and throttled Threads.
 */
#include "finte/object.h"

static volatile int g_state_lock;
static volatile int g_batch;

static int band_of(const fthread_t *t)
{
    return t->priority == 0 ? 0 : 1;
}

static uint64_t state_lock(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_state_lock);
    return flags;
}

static void state_unlock(uint64_t flags)
{
    hal_spin_unlock(&g_state_lock);
    hal_irq_restore(flags);
}

static uint64_t runq_lock(fcpu_t *cpu)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&cpu->runq_lock);
    return flags;
}

static void runq_unlock(fcpu_t *cpu, uint64_t flags)
{
    hal_spin_unlock(&cpu->runq_lock);
    hal_irq_restore(flags);
}

static void runq_push_locked(fcpu_t *cpu, fthread_t *t)
{
    int band = band_of(t);
    t->next_run = NULL;
    if (!cpu->runq[band]) {
        cpu->runq[band] = t;
        cpu->runq_tail[band] = t;
    } else {
        cpu->runq_tail[band]->next_run = t;
        cpu->runq_tail[band] = t;
    }
}

static fthread_t *runq_take_locked(fcpu_t *cpu)
{
    for (int band = 0; band < 2; band++) {
        fthread_t *t = cpu->runq[band];
        if (!t) {
            continue;
        }
        cpu->runq[band] = t->next_run;
        if (!cpu->runq[band]) {
            cpu->runq_tail[band] = NULL;
        }
        t->next_run = NULL;
        return t;
    }
    return NULL;
}

static int runq_remove_locked(fcpu_t *cpu, fthread_t *t)
{
    for (int band = 0; band < 2; band++) {
        fthread_t **pp = &cpu->runq[band];
        while (*pp) {
            if (*pp == t) {
                *pp = t->next_run;
                if (cpu->runq_tail[band] == t || !cpu->runq_tail[band]) {
                    cpu->runq_tail[band] = cpu->runq[band];
                    while (cpu->runq_tail[band] &&
                           cpu->runq_tail[band]->next_run) {
                        cpu->runq_tail[band] =
                            cpu->runq_tail[band]->next_run;
                    }
                }
                t->next_run = NULL;
                return 1;
            }
            pp = &(*pp)->next_run;
        }
    }
    return 0;
}

static void wake_queued(void)
{
    uint32_t n = hal_cpu_count();
    uint32_t mask = 0;
    for (uint32_t i = 0; i < n && i < FINTE_MAX_CPUS; i++) {
        fcpu_t *cpu = hal_cpu_slot(i);
        if (!cpu) {
            continue;
        }
        uint64_t flags = runq_lock(cpu);
        if (cpu->runq[0] || cpu->runq[1]) {
            mask |= 1u << i;
        }
        runq_unlock(cpu, flags);
    }
    for (uint32_t i = 0; i < n && i < FINTE_MAX_CPUS; i++) {
        if (mask & (1u << i)) {
            hal_cpu_wake(i);
        }
    }
}

static fcpu_t *thread_cpu(fthread_t *t)
{
    fcpu_t *cpu = hal_cpu_slot(t->cpu);
    if (!cpu || !cpu->online) {
        cpu = hal_cpu_slot(0);
    }
    return cpu;
}

/* Called with the state lock held. Takes the target queue lock. */
static void enqueue_locked(fthread_t *t)
{
    if (!t || !t->in_use || !t->flow || t->queued) {
        return;
    }
    fcpu_t *cpu = thread_cpu(t);
    uint64_t flags = runq_lock(cpu);
    t->cpu = cpu->id;
    t->state = FINTE_THREAD_READY;
    t->flow->state = FINTE_FLOW_READY;
    t->queued = 1;
    runq_push_locked(cpu, t);
    runq_unlock(cpu, flags);
}

void finte_sched_enqueue_thread(fthread_t *t)
{
    if (!t || !t->in_use || !t->flow || t->state == FINTE_THREAD_DEAD ||
        t->state == FINTE_THREAD_FREE) {
        return;
    }
    uint64_t flags = state_lock();
    enqueue_locked(t);
    uint32_t target = t->cpu;
    state_unlock(flags);
    if (g_batch) {
        hal_cpu_wake(target);
    }
}

void finte_sched_enqueue(fflow_t *f)
{
    if (f) {
        finte_sched_enqueue_thread(f->thread);
    }
}

void finte_sched_throttle_thread(fthread_t *t)
{
    if (!t || !t->in_use || !t->flow) {
        return;
    }
    uint64_t flags = state_lock();
    t->state = FINTE_THREAD_THROTTLED;
    t->flow->state = FINTE_FLOW_THROTTLED;
    t->next_throttle = finte_core.throttle;
    finte_core.throttle = t;
    state_unlock(flags);
}

void finte_sched_throttle(fflow_t *f)
{
    if (f) {
        finte_sched_throttle_thread(f->thread);
    }
}

void finte_sched_sleep_thread(fthread_t *t, uint64_t wake_ns)
{
    if (!t || !t->in_use || !t->flow) {
        return;
    }
    uint64_t flags = state_lock();
    t->wake_ns = wake_ns;
    t->state = FINTE_THREAD_SLEEPING;
    t->flow->state = FINTE_FLOW_SLEEPING;
    t->next_sleep = finte_core.sleepers;
    finte_core.sleepers = t;
    state_unlock(flags);
}

void finte_sched_sleep(fflow_t *f, uint64_t wake_ns)
{
    if (f) {
        finte_sched_sleep_thread(f->thread, wake_ns);
    }
}

static fthread_t *runq_pop_cpu(fcpu_t *cpu)
{
    uint64_t flags = runq_lock(cpu);
    fthread_t *t = runq_take_locked(cpu);
    if (t) {
        t->queued = 0;
        t->state = FINTE_THREAD_RUNNING;
        if (t->flow) {
            t->flow->state = FINTE_FLOW_RUNNING;
        }
    }
    runq_unlock(cpu, flags);
    return t;
}

static void wait_list_remove_locked(fthread_t *t)
{
    if (!t || !t->wait_chan) {
        return;
    }
    fchan_t *ch = t->wait_chan;
    fthread_t **head = t->wait_send ? &ch->wait_send : &ch->wait_recv;
    fthread_t **pp = head;
    while (*pp && *pp != t) {
        pp = &(*pp)->wait_next;
    }
    if (*pp == t) {
        *pp = t->wait_next;
    }
    t->wait_next = NULL;
    t->wait_chan = NULL;
    t->wait_send = 0;
}

static void sleep_list_remove_locked(fthread_t *t)
{
    fthread_t **pp = &finte_core.sleepers;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next_sleep;
            t->next_sleep = NULL;
            return;
        }
        pp = &(*pp)->next_sleep;
    }
}

void finte_sched_wake_thread(fthread_t *t)
{
    if (!t || !t->in_use || !t->flow) {
        return;
    }
    uint64_t flags = state_lock();
    if (t->blocking) {
        if (t->wait_chan) {
            wait_list_remove_locked(t);
        } else if (t->state == FINTE_THREAD_SLEEPING) {
            sleep_list_remove_locked(t);
        }
        t->wake_pending = 1;
        state_unlock(flags);
        return;
    }
    if (t->state == FINTE_THREAD_CREATED || t->state == FINTE_THREAD_RUNNING ||
        t->state == FINTE_THREAD_READY || t->state == FINTE_THREAD_DEAD ||
        t->state == FINTE_THREAD_FREE) {
        state_unlock(flags);
        return;
    }
    if (t->state == FINTE_THREAD_BLOCKED) {
        wait_list_remove_locked(t);
    } else if (t->state == FINTE_THREAD_SLEEPING) {
        sleep_list_remove_locked(t);
    }
    enqueue_locked(t);
    uint32_t target = t->cpu;
    state_unlock(flags);
    if (g_batch) {
        hal_cpu_wake(target);
    }
}

void finte_sched_wake(fflow_t *f)
{
    if (f) {
        finte_sched_wake_thread(f->thread);
    }
}

static int throttle_refill(void)
{
    uint64_t flags = state_lock();
    fthread_t *t = finte_core.throttle;
    finte_core.throttle = NULL;
    int n = 0;
    while (t) {
        fthread_t *next = t->next_throttle;
        t->next_throttle = NULL;
        if (t->flow) {
            t->flow->fuel = t->flow->fuel_max;
        }
        enqueue_locked(t);
        n++;
        t = next;
    }
    if (n) {
        finte_core.epochs++;
        finte_core.stats.epochs = finte_core.epochs;
    }
    state_unlock(flags);
    if (n) {
        wake_queued();
    }
    return n;
}

static int wake_sleepers(void)
{
    uint64_t now = hal_now_ns();
    uint64_t flags = state_lock();
    int n = 0;
    fthread_t **pp = &finte_core.sleepers;
    while (*pp) {
        fthread_t *t = *pp;
        if (now >= t->wake_ns) {
            *pp = t->next_sleep;
            t->next_sleep = NULL;
            if (t->blocking) {
                t->wake_pending = 1;
            } else {
                enqueue_locked(t);
            }
            n++;
        } else {
            pp = &t->next_sleep;
        }
    }
    state_unlock(flags);
    if (n) {
        wake_queued();
    }
    return n;
}

/* Timer hook: sleepers must expire even while a CPU-bound Flow never yields.
 * Called from the timer IRQ before the preemption decision. */
void finte_sched_tick(void)
{
    if (!g_batch) {
        return;
    }
    (void)wake_sleepers();
}

static uint64_t next_sleep_ns(void)
{
    uint64_t flags = state_lock();
    uint64_t best = 0;
    for (fthread_t *t = finte_core.sleepers; t; t = t->next_sleep) {
        uint64_t now = hal_now_ns();
        uint64_t delta = t->wake_ns > now ? t->wake_ns - now : 0;
        if (best == 0 || delta < best) {
            best = delta;
        }
    }
    state_unlock(flags);
    return best;
}

static int any_running_or_queued(void)
{
    uint64_t flags = state_lock();
    int found = 0;
    for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
        fflow_t *f = &finte_core.flows[i];
        if (f->in_use && (f->state == FINTE_FLOW_RUNNING ||
                          f->state == FINTE_FLOW_READY)) {
            found = 1;
            break;
        }
    }
    state_unlock(flags);
    return found;
}

static int any_blocked(void)
{
    uint64_t flags = state_lock();
    int found = 0;
    for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
        fflow_t *f = &finte_core.flows[i];
        if (f->in_use && f->state == FINTE_FLOW_BLOCKED &&
            f->thread && !f->thread->wake_pending) {
            found = 1;
            break;
        }
    }
    state_unlock(flags);
    return found;
}

void finte_sched_switch_out_thread(fthread_t *t)
{
    fcpu_t *cpu = hal_cpu_self();
    if (t && t->ctx && cpu->sched_ctx) {
        hal_ctx_switch(t->ctx, cpu->sched_ctx);
    }
}

void finte_sched_switch_out(fflow_t *f)
{
    if (f) {
        finte_sched_switch_out_thread(f->thread);
    }
}

/* Called from the timer IRQ while t is executing. The scheduler resumes on
 * this CPU's stack, sees READY, and enqueues the Thread. */
void finte_sched_preempt_thread(fthread_t *t)
{
    if (!t) {
        return;
    }
    uint64_t flags = state_lock();
    if (t->state != FINTE_THREAD_RUNNING) {
        state_unlock(flags);
        return;
    }
    t->state = FINTE_THREAD_READY;
    if (t->flow) {
        t->flow->state = FINTE_FLOW_READY;
    }
    state_unlock(flags);
    finte_sched_switch_out_thread(t);
}

void finte_sched_preempt(fflow_t *f)
{
    if (f) {
        finte_sched_preempt_thread(f->thread);
    }
}

int finte_sched_active(void)
{
    return g_batch;
}

int finte_sched_cpu_has_work(void)
{
    fcpu_t *cpu = hal_cpu_self();
    uint64_t flags = runq_lock(cpu);
    int has = cpu->runq[0] != NULL || cpu->runq[1] != NULL;
    runq_unlock(cpu, flags);
    return has;
}

/* Move a Flow's Thread to another CPU. Ready Threads are removed from one
 * queue and inserted on the target; running/blocked/sleeping Threads change
 * affinity and use it on their next enqueue. */
int finte_sched_migrate(fflow_t *f, uint32_t target)
{
    if (!f || !f->in_use) {
        return FINTE_ERR_INVAL;
    }
    fthread_t *t = f->thread;
    if (!t || !t->in_use) {
        return FINTE_ERR_NOENT;
    }
    fcpu_t *to = hal_cpu_slot(target);
    if (!to || !to->online) {
        return FINTE_ERR_NOENT;
    }
    uint64_t flags = state_lock();
    if (t->cpu == target) {
        state_unlock(flags);
        return 0;
    }
    fcpu_t *from = hal_cpu_slot(t->cpu);
    if (t->queued && from) {
        uint64_t qf = runq_lock(from);
        runq_remove_locked(from, t);
        t->queued = 0;
        runq_unlock(from, qf);
    }
    t->cpu = target;
    if (t->state == FINTE_THREAD_READY) {
        enqueue_locked(t);
    }
    to->migrations++;
    state_unlock(flags);
    if (g_batch) {
        hal_cpu_wake(target);
    }
    return 0;
}

/* Take one ready Thread from another CPU without touching the global state
 * lock. The Thread is handed directly to the caller for execution. */
static fthread_t *steal_one(fcpu_t *self)
{
    uint32_t n = hal_cpu_count();
    uint64_t sf = state_lock();
    for (uint32_t k = 1; k < n; k++) {
        fcpu_t *victim = hal_cpu_slot((self->id + k) % n);
        if (!victim || victim == self || !victim->online) {
            continue;
        }
        uint64_t flags = runq_lock(victim);
        fthread_t *t = runq_take_locked(victim);
        if (t && t->flow && t->flow->stats.slices == 0) {
            runq_push_locked(victim, t);
            t = NULL;
        }
        if (t) {
            t->queued = 0;
            t->cpu = self->id;
            self->steals++;
        }
        runq_unlock(victim, flags);
        if (t) {
            state_unlock(sf);
            return t;
        }
    }
    state_unlock(sf);
    return NULL;
}

void finte_sched_reset(void)
{
    uint64_t flags = state_lock();
    g_batch = 0;
    for (uint32_t i = 0; i < FINTE_MAX_CPUS; i++) {
        fcpu_t *cpu = hal_cpu_slot(i);
        if (!cpu) {
            continue;
        }
        uint64_t qf = runq_lock(cpu);
        cpu->runq[0] = NULL;
        cpu->runq[1] = NULL;
        cpu->runq_tail[0] = NULL;
        cpu->runq_tail[1] = NULL;
        cpu->current = NULL;
        cpu->current_thread = NULL;
        cpu->need_resched = 0;
        runq_unlock(cpu, qf);
    }
    state_unlock(flags);
}

int finte_run_status(void)
{
    return finte_core.status;
}

static void finish_ready(fthread_t *t)
{
    fflow_t *f = t->flow;
    if (!f) {
        return;
    }
    if (f->fuel == 0) {
        finte_sched_throttle_thread(t);
    } else {
        finte_sched_enqueue_thread(t);
    }
}

static void finish_dead(fthread_t *t)
{
    fflow_t *f = t->flow;
    finte_core.stats.flows_finished++;
    if (t->ctx) {
        hal_ctx_destroy(t->ctx);
        t->ctx = NULL;
    }
    fhandle_t self = f ? f->self : FINTE_HANDLE_NONE;
    if (f) {
        f->thread = NULL;
    }
    finte_thread_release(t);
    if (finte_handle_valid(self)) {
        finte_node_unref(self);
    }
}

/* Indexed by FINTE_THREAD_*; blocked/sleeping Threads stay where they are. */
static void (*const finish_ops[])(fthread_t *) = {
    [FINTE_THREAD_READY] = finish_ready,
    [FINTE_THREAD_DEAD] = finish_dead,
};

static void run_thread(fcpu_t *cpu, fthread_t *t)
{
    fflow_t *f = t->flow;
    cpu->current = f;
    cpu->current_thread = t;
    cpu->switches++;
    f->state = FINTE_FLOW_RUNNING;
    t->state = FINTE_THREAD_RUNNING;

    uint32_t q = t->quantum ? t->quantum : FINTE_FUEL_QUANTUM;
    if (q > f->fuel) {
        q = f->fuel;
    }
    f->fuel -= q;
    f->stats.fuel_used += q;
    f->stats.slices++;
    finte_core.stats.fuel_burned += q;

    uint64_t c0 = hal_cycles();
    finte_core.stats.context_switches++;
    hal_timer_start_cpu();
    hal_ctx_switch(cpu->sched_ctx, t->ctx);
    hal_timer_stop_cpu();
    t->cycles += hal_cycles() - c0;

    if (t->blocking) {
        uint64_t sf = state_lock();
        t->blocking = 0;
        if (t->wake_pending) {
            t->wake_pending = 0;
            enqueue_locked(t);
        } else if (t->state == FINTE_THREAD_RUNNING) {
            t->state = FINTE_THREAD_BLOCKED;
            f->state = FINTE_FLOW_BLOCKED;
        }
        state_unlock(sf);
    }
    if (t->state < FINTE_ARRAY_SIZE(finish_ops) && finish_ops[t->state]) {
        finish_ops[t->state](t);
    }
    cpu->current = NULL;
    cpu->current_thread = NULL;
}

static void sched_loop(fcpu_t *cpu, int owner)
{
    for (;;) {
        fthread_t *t = runq_pop_cpu(cpu);
        if (!t && 0) {
            t = steal_one(cpu);
        }
        if (t) {
            run_thread(cpu, t);
            continue;
        }
        if (!owner && !g_batch) {
            cpu->idle = 1;
            hal_cpu_idle();
            cpu->idle = 0;
            continue;
        }
        if (throttle_refill()) {
            continue;
        }
        if (wake_sleepers()) {
            continue;
        }
        if (finte_core.sleepers) {
            uint64_t wait = next_sleep_ns();
            if (wait == 0) {
                wait = 1;
            }
            if (wait > 1000000ull) {
                wait = 1000000ull;
            }
            hal_idle(wait);
            continue;
        }
        if (any_running_or_queued()) {
            hal_idle(50000ull);
            continue;
        }
        if (any_blocked()) {
            if (owner) {
                finte_core.stats.deadlocks++;
                finte_core.status = FINTE_ERR_DEADLOCK;
                hal_watchdog_strike("scheduler deadlock");
            }
        }
        if (owner) {
            break;
        }
        cpu->idle = 1;
        hal_cpu_idle();
        cpu->idle = 0;
    }
}

void finte_run(void)
{
    if (g_batch) {
        return;
    }
    if (hal_cpu_self()->id != 0) {
        return;
    }
    g_batch = 1;
    finte_core.status = FINTE_OK;
    fcpu_t *cpu = hal_cpu_self();
    cpu->sched_ctx = hal_ctx_self();
    hal_irq_disable();
    wake_queued();
    hal_cpu_wake_all();
    sched_loop(cpu, 1);
    g_batch = 0;
    hal_irq_enable();
}

void finte_sched_ap_run(void)
{
    fcpu_t *cpu = hal_cpu_self();
    cpu->sched_ctx = hal_ctx_self();
    hal_irq_disable();
    for (;;) {
        sched_loop(cpu, 0);
    }
}
