# Fuel contract

> **Status:** Partial — Fuel is an abstract per-Flow quantum counter with
> epoch refill. It is not yet CPU-time accounting and cannot be shared by
> multiple Threads.
>
> **Normative for:** resource budgeting, exhaustion behavior, replenishment
> and the relation between Fuel and scheduling policy.
>
> **Related:** [Flow](flow.md), [Thread](thread.md), [Scheduler](../kernel/scheduler.md).

## Resolved P0 decisions (2026-11-22)

1. **v0.1 Fuel is an abstract dispatch-quantum budget, not measured CPU time.**
   The unit is one scheduler quantum. Defaults remain `fuel_max = 1024` and
   `quantum = FINTE_FUEL_QUANTUM` (24).
2. **Fuel belongs to the Flow.** One Thread charges the owning Flow. There are
   no per-Thread sub-budgets in v0.1.
3. **Parallel sharing is deferred.** Since v0.1 is Serial-only, the shared-budget
   question is not exercised yet. The target rule remains: creating Threads
   must not create free Fuel.
4. **Exhaustion throttles, never kills.** A Flow at zero Fuel enters
   `THROTTLED`; a later epoch restores `fuel_max` and re-enqueues it.
5. **Replenishment is event-driven.** The scheduler refills the throttle list
   when the CPU has no runnable work, before servicing sleepers. There is no
   periodic per-Flow refill timer in v0.1.
6. **Scheduling bands remain two strict bands.** Band 0 (`priority == 0`) is
   drained before band 1. Aging, fair share and deadline scheduling are
   post-v0.1. The starvation risk is documented and accepted for v0.1.
7. **Measured CPU-time accounting is Phase 1/2 work.** It requires a defined
   clock source, accounting overhead measurement and tests on both
   architectures.

## 1. Definition

**Fuel** is a resource budget associated primarily with a Flow. It is a
mechanism for bounding resource consumption, not a synonym for a time slice and
not a scheduling policy by itself.

The initial implementation focuses on CPU consumption. Memory commitment, I/O
throughput and device usage are future resource dimensions.

```text
Flow
 ├── Fuel budget
 │    └── CPU budget
 └── Threads
      ├── Thread A
      └── Thread B
```

## 2. Current implementation

### Units and defaults

- `fuel` and `fuel_max` are `uint32_t` abstract units.
- `fflow_t.quantum` defaults to `FINTE_FUEL_QUANTUM` (24).
- `finte_flow_create()` sets `fuel_max = fuel ? fuel : 1024` and
  `fuel = fuel_max`.
- The scheduler subtracts `q = min(quantum, fuel)` on each dispatch.
- `fflow_stats.fuel_used` and `fflow_stats.slices` record consumption.
- `finte_core.stats.fuel_burned` records the global total.

Fuel units are **not** nanoseconds, cycles or a fixed wall-clock duration.
They are dispatch quanta. A Flow with a larger quantum can consume more Fuel
per dispatch.

### Dispatch and exhaustion

In `src/core/sched.c`:

1. `run_flow()` chooses the quantum, clamps it to remaining Fuel, subtracts it
   and increments the slice counters.
2. When the Flow returns to the scheduler, `finish_ready()` checks Fuel.
3. If Fuel is zero, the Flow is placed on the throttle list and marked
   `FINTE_FLOW_THROTTLED`.
4. Otherwise it is re-enqueued.

Fuel exhaustion does **not** destroy the Flow. It removes the Flow from the
ready queues until replenishment.

### Replenishment

`throttle_refill()`:

- detaches the entire throttle list;
- restores each Flow's `fuel = fuel_max`;
- re-enqueues each Flow;
- increments the epoch counter and `stats.epochs`.

The scheduler calls `throttle_refill()` when the CPU has no runnable work,
before it services sleepers. Replenishment is therefore **not time-based**: a
busy system can keep a throttled Flow waiting while other Flows run. This is a
global epoch, not a per-Flow period.

### Scheduling bands

The current run queues have two bands:

- band 0: `priority == 0`;
- band 1: every other priority.

`runq_take_locked()` always drains band 0 first. There is no aging or priority
inversion handling. A continuous band-0 workload can starve band 1.

## 3. Target model

Fuel is a Flow-level budget shared by all Threads of the Flow.

Rules:

- Threads of one Flow must not each receive a full independent copy of the
  Flow budget by default.
- Parallel Threads share the Flow budget or receive explicitly accounted
  sub-budgets.
- Creating a Thread must not create free budget.
- Fuel exhaustion throttles, suspends or replenishes according to policy; it
  does not automatically destroy the Flow.
- Accounting precision and overhead must be measured.
- Fuel must remain separable from scheduling policy: fair, priority, deadline
  and fuel-aware classes may all use the same accounting mechanism.

## 4. Policy separation

Potential scheduling classes:

| Policy | Intended workload |
| --- | --- |
| Fair scheduling | Interactive and background tasks |
| Priority scheduling | Explicit relative importance |
| Fuel-aware scheduling | Resource-constrained tasks |
| Deadline scheduling | Explicit timing requirements |

The first release does not need every class. The design must avoid making
future classes impossible.

## 5. Invariants

1. Fuel is charged to the owning Flow, not manufactured per Thread.
2. A Flow cannot run after its budget is exhausted without replenishment.
3. Fuel exhaustion is not silent destruction.
4. Replenishment restores at most `fuel_max`.
5. A Flow cannot gain Fuel by creating another Thread.
6. Fuel accounting must not overflow silently.
7. A throttled Flow remains a live object with its capabilities intact.

## 6. Current limitations

- Fuel is an abstract quantum count, not measured CPU time.
- Replenishment is event-driven, not periodic.
- A busy system can delay refill of throttled Flows indefinitely.
- There is no per-Thread sub-budget.
- There is no deadline or fair-share policy.
- Band 0 can starve band 1.
- The kernel does not expose a policy for choosing how much Fuel a new Flow
  receives; callers pass a raw value.

## 7. Remaining open questions (post-v0.1)

- Exact CPU-time unit, resolution and accounting overhead.
- Per-Thread sub-budget rules for Parallel Flows.
- Fairness and maximum wait for throttled Flows.
- Interaction between Fuel and priorities.
- Starvation prevention and deadline scheduling.
- Behavior when a Flow has multiple runnable Threads but insufficient budget.
- Whether Fuel can be donated, borrowed or transferred between Flows.
- Whether Fuel dimensions beyond CPU are needed before v0.2.

## 8. Test evidence

- `src/tests/test_main.c`: Fuel accounting and throttling paths.
- `src/core/selftest.c`: scheduler and Fuel checks.
- `preempt` shell command: measures preemption while a Flow spins.
- `smp` command: reports per-CPU switches, steals and migrations.
- No test yet for parallel Thread budgets or measured CPU-time accounting.
