# Scheduler contract

> **Status:** Partial — per-CPU Thread run queues, affinity, migration and timer
> preemption exist. Work stealing is present in code but currently disabled.
> Each Flow has exactly one primary Thread; multi-Thread and Parallel Flows are
> deferred.
>
> **Normative for:** run queues, dispatch, affinity, migration, preemption,
> sleep/throttle handling and idle behavior.
>
> **Related:** [Flow](../concepts/flow.md), [Thread](../concepts/thread.md),
> [Fuel](../concepts/fuel.md), [Interrupt model](interrupt-model.md).

## 1. Current model

The scheduler runs one Thread per CPU at a time. Each CPU owns:

- a two-band Thread run queue (`runq[0]`, `runq[1]`);
- a run-queue spinlock;
- a current-Flow pointer and a current-Thread pointer;
- timer, IPI, switch, steal and migration counters;
- page and stack caches.

A smaller global state lock protects:

- sleep and throttle lists;
- blocked/ready transitions;
- queue insertion and wake races;
- the `queued` flag.

Run-queue pops do not take the global state lock.

## 2. Run-queue bands

`band_of()` maps:

- `priority == 0` to band 0;
- every other priority to band 1.

`runq_take_locked()` always drains band 0 before band 1. There is no aging,
fair share or priority inheritance. A continuous band-0 workload can starve
band 1.

Each primary Thread is assigned a CPU at Flow creation by a round-robin
`g_next_cpu` hint. `thread_cpu()` falls back to CPU 0 if the assigned CPU is
offline.

## 3. Enqueue and dispatch

`finte_sched_enqueue_thread()`:

1. rejects dead/free Threads;
2. takes the state lock;
3. calls `enqueue_locked()`, which takes the target CPU's run-queue lock;
4. sets the Thread `READY`, the Flow `READY`, `queued = 1`, and appends to the
   band tail;
5. wakes the target CPU only when the scheduler batch is active.

`run_thread()`:

1. sets `cpu->current = f` and `cpu->current_thread = t`, increments switches,
   sets both Thread and Flow `RUNNING`;
2. clamps the Thread quantum to the Flow's remaining Fuel and charges it;
3. starts the CPU timer;
4. switches from the scheduler context to the Thread context;
5. stops the timer and records Thread cycles;
6. resolves the blocking handshake;
7. runs the Thread-state finish operation.

`finish_ready()` throttles a Thread at zero Flow Fuel, otherwise re-enqueues it.
`finish_dead()` destroys the Thread context and drops the scheduler's Flow node
reference.

## 4. Preemption

The timer gate calls `finte_sched_preempt(f)` while its Thread is running:

- if the Thread is still `RUNNING`, mark Thread and Flow `READY`;
- switch to the CPU scheduler context.

After the switch returns, the scheduler sees `READY` and re-enqueues the Thread.
The timer tick also services sleepers when a scheduler batch is active.

Threads run with interrupts enabled; scheduler contexts run with interrupts
disabled.

## 5. Sleep, throttle and deadlock detection

- `finte_sched_sleep_thread()` puts a Thread on the sleep list with `wake_ns`.
- `wake_sleepers()` moves expired sleepers to the ready queues, or sets
  `wake_pending` if the Thread is still switching out.
- `finte_sched_throttle_thread()` puts a Thread with zero Flow Fuel on the
  throttle list.
- `throttle_refill()` restores the Flow Fuel and re-enqueues the whole throttle
  list when the scheduler has no runnable work.
- If the owner CPU has no runnable work, no sleepers, and blocked Flows exist,
  it increments `stats.deadlocks`, sets `FINTE_ERR_DEADLOCK`, and records a
  watchdog strike.
- The owner CPU exits the batch loop; APs park until the next batch.

## 6. Affinity and migration

`finte_sched_migrate(f, target)`:

- validates the target CPU;
- takes the state lock;
- resolves the Flow's primary Thread;
- removes a queued Thread from the old run queue;
- sets `t->cpu = target` and increments the target's migration counter;
- re-enqueues the Thread if it is `READY`;
- for running, blocked or sleeping Threads, changes affinity for the next
  enqueue.

Migration does not move memory, capabilities, Channels or Arenas. It only
changes where the Flow's Thread will run.

## 7. Work stealing — currently disabled

`steal_one()` exists and can take one ready Thread from another CPU's queue
without taking the global state lock. It:

- scans other online CPUs;
- takes the victim's run-queue lock;
- removes one Thread;
- skips Threads whose Flow has never run (`flow->stats.slices == 0`);
- updates `queued`, `cpu` and the thief's `steals` counter.

However, `sched_loop()` currently contains:

```c
if (!t && 0) {
    t = steal_one(cpu);
}
```

The `&& 0` disables stealing. Current `smp` output reports `steals=0`. Re-enable
only after a stress run covering wake/switch races on multiple CPUs, and update
the docs with the result.

## 8. Idle and IPI behavior

- `wake_queued()` scans CPUs with queued work and sends a wake IPI when a
  scheduler batch is active.
- A CPU with no work and no active batch parks in `hal_cpu_idle()`.
- With sleepers, the owner waits for the next sleep deadline (capped at 1 ms).
- With runnable or queued Threads, it waits briefly and retries.
- IPIs target CPUs parked with queued work. A wake targeting a running CPU
  waits for its next yield or timer tick.

## 9. Target model

The first stage now dispatches **Threads**. The target adds:

- Multiple Threads per Flow, with Serial and Parallel semantics.
- A Flow's Fuel charged when any of its Threads runs.
- A Serial Flow ensuring at most one of its Threads is runnable.
- A Parallel Flow placing multiple Threads on different CPUs.
- Work stealing moving Threads.
- Migration moving a Thread, not the Flow's authority or memory.
- Scheduling policy separated from dispatch mechanism.
- Fair, priority, fuel-aware and deadline classes sharing the same run-queue
  and accounting foundations.

## 10. Invariants

1. One CPU runs at most one Thread context at a time.
2. A queued Thread is on exactly one run queue.
3. `queued == 1` means the Thread is in a run queue.
4. A Thread is not enqueued while it is switching out without the
   `wake_pending` handshake.
5. A dead Thread is never dispatched.
6. Migration does not change authority or memory ownership.
7. Fuel is charged to the Flow before its Thread runs, not after it exits.
8. A CPU does not steal from an offline CPU.
9. The owner CPU is the only one that declares a scheduler deadlock.
10. APs do not dispatch work until `g_batch` is active.

## 11. Current limitations

- Work stealing is disabled.
- Static affinity; no load balancing.
- Two strict bands; no aging or fair share.
- Only one Thread per Flow; no Parallel Flows.
- No deadline or priority-inheritance policy.
- Global state lock for sleep/throttle/blocked transitions.
- Global core lock serializes verbs across CPUs.
- A busy system can delay throttle refill.
- No CPU hotplug or offline path beyond boot.

## 12. Open questions

- When to re-enable work stealing and with what stress coverage.
- How multiple Threads of one Flow should be placed across run queues.
- How Parallel Flow Threads share Fuel and cancellation.
- Whether priority is per-Flow, per-Thread or both.
- How to prevent band starvation without adding unbounded complexity.
- What load-balancing policy is appropriate for the first release.
- How scheduler policy is configured by userland, if at all.

## 13. Test evidence

- `src/tests/test_main.c`: Flow/Thread split, queue, migration, Fuel and
  throttle behavior (23 host tests).
- `src/core/selftest.c`: scheduler checks.
- `preempt`: timer preemption against a spinning Flow.
- `smp`: per-CPU ticks, IPIs, switches, steals and migrations.
- `services`: multi-CPU service run and migration exercise.
- `make qemu-test` / `make qemu-uefi-test`: shell and demo under SMP.
- No stress test yet for re-enabled work stealing.
