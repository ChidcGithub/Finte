/* Multi-core flow scheduler.
 *
 * Each flow is assigned to a CPU when it is created. Run queues are per-CPU;
 * a global scheduler spinlock protects queue and state transitions. The BSP
 * owns epoch refills and sleeper wakeups. AP workers park in HLT when there is
 * no batch and are woken by a reschedule IPI.
 */
#include "finte/object.h"

static volatile int g_sched_lock;
static volatile int g_batch;

static int band_of(const fflow_t *f)
{
    return f->priority == 0 ? 0 : 1;
}

static uint64_t sched_lock(void)
{
    uint64_t flags = hal_irq_save();
    hal_spin_lock(&g_sched_lock);
    return flags;
}

static void sched_unlock(uint64_t flags)
{
    hal_spin_unlock(&g_sched_lock);
    hal_irq_restore(flags);
}

static void wake_queued(void)
{
    uint32_t n = hal_cpu_count();
    uint32_t mask = 0;
    uint64_t flags = sched_lock();
    for (uint32_t i = 0; i < n && i < FINTE_MAX_CPUS; i++) {
        fcpu_t *cpu = hal_cpu_slot(i);
        if (cpu && (cpu->runq[0] || cpu->runq[1])) {
            mask |= 1u << i;
        }
    }
    sched_unlock(flags);
    for (uint32_t i = 0; i < n && i < FINTE_MAX_CPUS; i++) {
        if (mask & (1u << i)) {
            hal_cpu_wake(i);
        }
    }
}

static fcpu_t *flow_cpu(fflow_t *f)
{
    fcpu_t *cpu = hal_cpu_slot(f->cpu);
    if (!cpu || !cpu->online) {
        cpu = hal_cpu_slot(0);
    }
    return cpu;
}

static void enqueue_locked(fflow_t *f)
{
    if (f->queued) {
        return;
    }
    int band = band_of(f);
    fcpu_t *cpu = flow_cpu(f);
    f->cpu = cpu->id;
    f->state = FINTE_FLOW_READY;
    f->queued = 1;
    f->next_run = NULL;
    if (!cpu->runq[band]) {
        cpu->runq[band] = f;
        cpu->runq_tail[band] = f;
    } else {
        cpu->runq_tail[band]->next_run = f;
        cpu->runq_tail[band] = f;
    }
}

void finte_sched_enqueue(fflow_t *f)
{
    if (!f || !f->in_use || f->state == FINTE_FLOW_DEAD ||
        f->state == FINTE_FLOW_FREE) {
        return;
    }
    uint64_t flags = sched_lock();
    enqueue_locked(f);
    uint32_t target = f->cpu;
    sched_unlock(flags);
    if (g_batch) {
        hal_cpu_wake(target);
    }
}

void finte_sched_throttle(fflow_t *f)
{
    if (!f) {
        return;
    }
    uint64_t flags = sched_lock();
    f->state = FINTE_FLOW_THROTTLED;
    f->next_throttle = finte_core.throttle;
    finte_core.throttle = f;
    sched_unlock(flags);
}

void finte_sched_sleep(fflow_t *f, uint64_t wake_ns)
{
    if (!f) {
        return;
    }
    uint64_t flags = sched_lock();
    f->wake_ns = wake_ns;
    f->state = FINTE_FLOW_SLEEPING;
    f->next_sleep = finte_core.sleepers;
    finte_core.sleepers = f;
    sched_unlock(flags);
}

static fflow_t *runq_pop_cpu(fcpu_t *cpu)
{
    uint64_t flags = sched_lock();
    fflow_t *f = NULL;
    for (int band = 0; band < 2; band++) {
        f = cpu->runq[band];
        if (f) {
            cpu->runq[band] = f->next_run;
            if (!cpu->runq[band]) {
                cpu->runq_tail[band] = NULL;
            }
            f->next_run = NULL;
            f->queued = 0;
            f->state = FINTE_FLOW_RUNNING;
            break;
        }
    }
    sched_unlock(flags);
    return f;
}

static void wait_list_remove_locked(fflow_t *f)
{
    if (!f || !f->wait_chan) {
        return;
    }
    fchan_t *ch = f->wait_chan;
    fflow_t **head = f->wait_send ? &ch->wait_send : &ch->wait_recv;
    fflow_t **pp = head;
    while (*pp && *pp != f) {
        pp = &(*pp)->wait_next;
    }
    if (*pp == f) {
        *pp = f->wait_next;
    }
    f->wait_next = NULL;
    f->wait_chan = NULL;
    f->wait_send = 0;
}

static void sleep_list_remove_locked(fflow_t *f)
{
    fflow_t **pp = &finte_core.sleepers;
    while (*pp) {
        if (*pp == f) {
            *pp = f->next_sleep;
            f->next_sleep = NULL;
            return;
        }
        pp = &(*pp)->next_sleep;
    }
}

void finte_sched_wake(fflow_t *f)
{
    if (!f || !f->in_use) {
        return;
    }
    uint64_t flags = sched_lock();
    if (f->state == FINTE_FLOW_RUNNING || f->state == FINTE_FLOW_READY ||
        f->state == FINTE_FLOW_DEAD || f->state == FINTE_FLOW_FREE) {
        sched_unlock(flags);
        return;
    }
    if (f->state == FINTE_FLOW_BLOCKED) {
        wait_list_remove_locked(f);
    } else if (f->state == FINTE_FLOW_SLEEPING) {
        sleep_list_remove_locked(f);
    }
    enqueue_locked(f);
    uint32_t target = f->cpu;
    sched_unlock(flags);
    if (g_batch) {
        hal_cpu_wake(target);
    }
}

static int throttle_refill(void)
{
    uint64_t flags = sched_lock();
    fflow_t *f = finte_core.throttle;
    finte_core.throttle = NULL;
    int n = 0;
    while (f) {
        fflow_t *next = f->next_throttle;
        f->next_throttle = NULL;
        f->fuel = f->fuel_max;
        enqueue_locked(f);
        n++;
        f = next;
    }
    if (n) {
        finte_core.epochs++;
        finte_core.stats.epochs = finte_core.epochs;
    }
    sched_unlock(flags);
    if (n) {
        wake_queued();
    }
    return n;
}

static int wake_sleepers(void)
{
    uint64_t now = hal_now_ns();
    uint64_t flags = sched_lock();
    int n = 0;
    fflow_t **pp = &finte_core.sleepers;
    while (*pp) {
        fflow_t *f = *pp;
        if (now >= f->wake_ns) {
            *pp = f->next_sleep;
            f->next_sleep = NULL;
            enqueue_locked(f);
            n++;
        } else {
            pp = &f->next_sleep;
        }
    }
    sched_unlock(flags);
    if (n) {
        wake_queued();
    }
    return n;
}

/* Timer hook: sleepers must expire even while a CPU-bound flow never yields.
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
    uint64_t flags = sched_lock();
    uint64_t best = 0;
    for (fflow_t *f = finte_core.sleepers; f; f = f->next_sleep) {
        uint64_t now = hal_now_ns();
        uint64_t delta = f->wake_ns > now ? f->wake_ns - now : 0;
        if (best == 0 || delta < best) {
            best = delta;
        }
    }
    sched_unlock(flags);
    return best;
}

static int any_running_or_queued(void)
{
    uint64_t flags = sched_lock();
    int found = 0;
    for (uint32_t i = 0; i < hal_cpu_count(); i++) {
        fcpu_t *cpu = hal_cpu_slot(i);
        if (!cpu) {
            continue;
        }
        if (cpu->runq[0] || cpu->runq[1]) {
            found = 1;
            break;
        }
    }
    if (!found) {
        for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
            fflow_t *f = &finte_core.flows[i];
            if (f->in_use && (f->state == FINTE_FLOW_RUNNING ||
                              f->state == FINTE_FLOW_READY)) {
                found = 1;
                break;
            }
        }
    }
    sched_unlock(flags);
    return found;
}

static int any_blocked(void)
{
    uint64_t flags = sched_lock();
    int found = 0;
    for (uint32_t i = 0; i < FINTE_MAX_FLOWS; i++) {
        fflow_t *f = &finte_core.flows[i];
        if (f->in_use && f->state == FINTE_FLOW_BLOCKED) {
            found = 1;
            break;
        }
    }
    sched_unlock(flags);
    return found;
}

void finte_sched_switch_out(fflow_t *f)
{
    fcpu_t *cpu = hal_cpu_self();
    if (f && cpu->sched_ctx) {
        hal_ctx_switch(f->ctx, cpu->sched_ctx);
    }
}

/* Called from the timer IRQ while f is executing. The scheduler resumes on
 * this CPU's stack, sees READY, and enqueues the flow. */
void finte_sched_preempt(fflow_t *f)
{
    if (!f) {
        return;
    }
    uint64_t flags = sched_lock();
    if (f->state != FINTE_FLOW_RUNNING) {
        sched_unlock(flags);
        return;
    }
    f->state = FINTE_FLOW_READY;
    sched_unlock(flags);
    finte_sched_switch_out(f);
}

int finte_sched_active(void)
{
    return g_batch;
}

int finte_sched_cpu_has_work(void)
{
    fcpu_t *cpu = hal_cpu_self();
    return cpu->runq[0] != NULL || cpu->runq[1] != NULL;
}

void finte_sched_reset(void)
{
    g_batch = 0;
    for (uint32_t i = 0; i < FINTE_MAX_CPUS; i++) {
        fcpu_t *cpu = hal_cpu_slot(i);
        if (!cpu) {
            continue;
        }
        cpu->runq[0] = NULL;
        cpu->runq[1] = NULL;
        cpu->runq_tail[0] = NULL;
        cpu->runq_tail[1] = NULL;
        cpu->current = NULL;
        cpu->need_resched = 0;
    }
}

int finte_run_status(void)
{
    return finte_core.status;
}

static void finish_ready(fflow_t *f)
{
    if (f->fuel == 0) {
        finte_sched_throttle(f);
    } else {
        finte_sched_enqueue(f);
    }
}

static void finish_dead(fflow_t *f)
{
    finte_core.stats.flows_finished++;
    if (f->ctx) {
        hal_ctx_destroy(f->ctx);
        f->ctx = NULL;
    }
    fhandle_t self = f->self;
    finte_node_unref(self);
}

/* Indexed by FINTE_FLOW_*; blocked/sleeping flows stay where they are. */
static void (*const finish_ops[])(fflow_t *) = {
    [FINTE_FLOW_READY] = finish_ready,
    [FINTE_FLOW_DEAD] = finish_dead,
};

static void run_flow(fcpu_t *cpu, fflow_t *f)
{
    cpu->current = f;
    cpu->switches++;
    f->state = FINTE_FLOW_RUNNING;

    uint32_t q = f->quantum ? f->quantum : FINTE_FUEL_QUANTUM;
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
    hal_ctx_switch(cpu->sched_ctx, f->ctx);
    hal_timer_stop_cpu();
    f->cycles += hal_cycles() - c0;

    if (f->state < FINTE_ARRAY_SIZE(finish_ops) && finish_ops[f->state]) {
        finish_ops[f->state](f);
    }
    cpu->current = NULL;
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

    for (;;) {
        fflow_t *f = runq_pop_cpu(cpu);
        if (f) {
            run_flow(cpu, f);
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
            finte_core.stats.deadlocks++;
            finte_core.status = FINTE_ERR_DEADLOCK;
            hal_watchdog_strike("scheduler deadlock");
        }
        break;
    }

    g_batch = 0;
    hal_irq_enable();
}

void finte_sched_ap_run(void)
{
    fcpu_t *cpu = hal_cpu_self();
    cpu->sched_ctx = hal_ctx_self();
    for (;;) {
        if (!g_batch) {
            cpu->idle = 1;
            hal_cpu_idle();
            cpu->idle = 0;
            continue;
        }
        fflow_t *f = runq_pop_cpu(cpu);
        if (!f) {
            cpu->idle = 1;
            hal_cpu_idle();
            cpu->idle = 0;
            continue;
        }
        run_flow(cpu, f);
    }
}
