# Thread contract

> **Status:** Partial — an internal one-per-Flow Thread object now exists and
> the scheduler dispatches Threads. User-visible Thread handles, multiple
> Threads and Parallel Flows are still deferred.
>
> **Normative for:** the target CPU execution entity, its relationship to a
> Flow, scheduling state and lifetime.
>
> **Related:** [Flow](flow.md), [Fuel](fuel.md), [Scheduler](../kernel/scheduler.md),
> [Capability](capability.md).

## Resolved P0 decisions (2026-11-22)

1. **The first Thread is implicit and internal.** `finte_flow_create()` allocates
   one `fthread_t`; `finte_flow_start()` creates its context and makes it
   runnable. No user-visible Thread handle or verb exists in v0.1.
2. **A Thread belongs to exactly one Flow.** The Flow owns its Thread set. A
   Thread cannot outlive its Flow.
3. **The scheduler dispatches Threads.** Run queues, sleep and throttle lists,
   blocking state and migration operate on `fthread_t`. The Flow state mirrors
   the primary Thread state in v0.1 for compatibility with existing verbs.
4. **Serial-only.** A Flow has at most one runnable Thread in v0.1. The Thread
   state machine is `CREATED → READY → RUNNING → BLOCKED/SLEEPING → TERMINATED`;
   `READY` and `RUNNING` may alternate until termination.
5. **Fault policy.** An unexpected user Thread fault terminates the owning Flow
   (current behavior). A kernel fault panics. Per-Flow fault policies are
   post-v0.1.
6. **Cancellation is cooperative and not exposed.** The kernel stops dispatch
   and waits for the Thread to leave kernel/scheduler state; it does not destroy
   a running stack asynchronously. User cancellation verbs are deferred.
7. **No per-Thread capabilities.** A Thread executes with its Flow's capability
   table. Per-Thread restrictions are deferred.

## 1. Definition

A **Thread** is the entity the scheduler dispatches onto a CPU. It owns the
architecture-specific execution state:

- saved register context;
- kernel stack and, where applicable, user stack reference;
- scheduling state, priority and runtime accounting;
- current CPU and affinity;
- blocked/wake state.

A Thread does **not** own the task's capability set, Fuel budget, Channel set
or Arena set. Those belong to the owning Flow. A Thread executes within the
authority context of its Flow, subject to explicitly represented per-Thread
restrictions.

## 2. Relationship to a Flow

```text
Flow
 ├── capability set
 ├── Fuel budget
 ├── Channels
 ├── Arenas
 └── Threads
      ├── Thread 0: context, stack, CPU, scheduling state
      └── Thread 1: ...
```

Rules:

- A Flow may have zero or more Threads.
- A Serial Flow allows at most one Thread to execute at a time.
- A Parallel Flow allows more than one Thread to execute on different CPUs.
- Creating a Thread does not create additional authority or Fuel.
- A Thread cannot outlive its owning Flow.
- A Thread's fault or exit must have a defined effect on the Flow.

**Current implementation:** `finte_flow_create()` allocates one internal
`fthread_t` from `finte_core.threads[]`. `finte_flow_start()` creates its
`fhal_ctx_t` with a 32 KiB stack and enqueues the Thread. The scheduler queues
Threads; `finte_sched_migrate()` still takes a Flow and moves its primary
Thread. There is no way to create a second Thread, no Thread handle, and no
user-visible Thread verb.

## 3. Current mapping (first stage)

The first-stage split is now real:

| Target Thread field | Current location |
| --- | --- |
| Identity | `fthread_t.slot` (internal; no handle yet) |
| Owning Flow | `fthread_t.flow` |
| Architecture context | `fthread_t.ctx` (`fhal_ctx_t`) |
| Kernel stack | `ctx->stack`, allocated by `hal_ctx_create()` |
| Address space | `ctx->addr_space` |
| Current CPU | `fthread_t.cpu` |
| Scheduler queue link | `fthread_t.next_run` |
| Queue membership | `fthread_t.queued` |
| Blocking handshake | `fthread_t.blocking`, `wake_pending` |
| Sleep link | `fthread_t.next_sleep` |
| Throttle link | `fthread_t.next_throttle` |
| Channel wait link | `fthread_t.wait_next`, `wait_chan`, `wait_send` |
| Priority | `fthread_t.priority` |
| Quantum | `fthread_t.quantum` |
| Runtime counter | `fthread_t.cycles`; Flow `stats.slices` remains aggregate |
| Fuel | `fflow_t.fuel`, `fuel_max` |
| Flow waiters | `fflow_t.waiters` (a Thread list) |

The Flow state mirrors its primary Thread state for compatibility with existing
verbs and tests. That mirroring is the main remaining coupling to remove when
multi-Thread Flows arrive.

## 4. Target object model

Proposed conceptual fields:

| Field | Meaning |
| --- | --- |
| `thread_id` / handle | Stable generation-checked identity |
| `owner_flow` | Flow that owns this Thread |
| `arch_context` | HAL-defined register context |
| `state` | Thread lifecycle state |
| `priority` | Scheduling priority or class input |
| `runtime` | CPU time consumed by this Thread |
| `quantum` | Dispatch quantum |
| `current_cpu` | CPU currently executing or last executing |
| `affinity` | Allowed CPU set or assigned CPU |
| `kernel_stack` | Kernel execution stack |
| `user_context` | User stack/program reference where applicable |
| `blocked_on` | Channel, Flow, timer or other wait object |
| `wake_pending` | Wake raced with switch-out |
| `sched_link` | Run-queue linkage |
| `flags` | Detached/joinable and policy flags |

The architecture context remains HAL-defined. Do not assume x86_64 and AArch64
have identical register layouts.

## 5. Target lifecycle

```text
CREATED
   |
   v
READY <-------------------+
   |                      |
   v                      |
RUNNING                   |
   |                      |
   +----> BLOCKED --------+
   |
   +----> SLEEPING -------+
   |
   +----> TERMINATED
```

Transitions:

| From | To | Trigger |
| --- | --- | --- |
| CREATED | READY | Thread creation completes |
| READY | RUNNING | Scheduler dispatch |
| RUNNING | READY | Quantum expiry, preemption, yield |
| RUNNING | BLOCKED | Channel, Flow wait or other wait object |
| RUNNING | SLEEPING | Timed sleep |
| BLOCKED/SLEEPING | READY | Wake or timer expiry |
| RUNNING/READY | TERMINATED | Exit, fault policy or cancellation |
| BLOCKED/SLEEPING | TERMINATED | Cancellation after quiescence |

A Flow-level suspend is not the same as a Thread block. Suspension policy and
its interaction with Thread states remain open.

## 6. Scheduling

The scheduler dispatches Threads, not Flows.

- A ready Thread is placed on a CPU run queue.
- A Flow's Fuel is charged when one of its Threads runs.
- A Serial Flow must not have two runnable Threads at once. The Flow layer
  enforces this before enqueue, not the low-level dispatch loop.
- A Parallel Flow may place multiple Threads on different CPUs.
- Migration moves a Thread and updates its affinity; it does not move the
  Flow's capabilities or memory.

**Current implementation:** the scheduler queues the primary Thread in two
per-CPU bands and uses Thread affinity. `finte_sched_migrate()` still takes a
Flow and moves its primary Thread. Work stealing exists in code but is disabled;
see [Scheduler](../kernel/scheduler.md).

## 7. Creation and authority

- Thread creation requires authority over the owning Flow.
- A new Thread executes with the Flow's capability context.
- A per-Thread restriction, if introduced, may only reduce authority.
- A Thread cannot be created with a Fuel budget independent of the Flow unless
  the Flow explicitly allocates a sub-budget.
- Creating a Thread must not create a new capability table, Channel endpoint
  or Arena mapping by implication.

## 8. Blocking, wake and cancellation

### Blocking

A Thread blocks on a wait object. The wait object records the Thread, not the
Flow. The Flow's state is derived: it is blocked only when all of its Threads
are blocked and it has no runnable work, subject to the final state machine.

### Wake races

A wake that arrives while a Thread is switching out must not cause the same
context to run on two CPUs. The current kernel uses `blocking` and
`wake_pending` for this handshake; the Thread model must preserve the
invariant.

### Cancellation

Cancellation is a request, not an asynchronous context destruction:

1. Mark the Thread terminating.
2. Prevent new dispatch.
3. Wake it from waits if necessary.
4. Wait until it is quiescent.
5. Release its stack, context and references.
6. Update the Flow state and any joiners.

The exact cancellation points and whether kernel operations are interruptible
remain open.

### Faults

A Thread fault must have a defined policy:

- kill only the Thread and continue the Flow; or
- terminate the whole Flow; or
- deliver a fault event to a supervisor.

The current x86 user-fault path kills the user process (Flow) and records a
watchdog strike for unexpected faults. Kernel faults panic. The target policy
must be explicit per fault class.

## 9. Invariants

1. A Thread belongs to exactly one Flow for its lifetime.
2. A Thread never has broader capability authority than its Flow.
3. A Thread is never simultaneously running on two CPUs.
4. A Thread is not freed while it is running, blocked or queued.
5. A Flow does not disappear while one of its Threads is still executing.
6. Fuel is charged to the Flow, not created per Thread.
7. A Serial Flow never has two executing Threads.
8. A terminated Thread is never scheduled again.

## 10. Migration status

Completed in the first stage:

1. Added `fthread_t` and a fixed Thread pool.
2. Allocated one implicit Thread per Flow at `finte_flow_create()`.
3. Moved context, CPU, priority, quantum, queue links, blocking state, sleep
   and throttle links and runtime cycles into `fthread_t`.
4. Changed the per-CPU run queues, sleep/throttle lists and channel wait lists
   to Threads.
5. Kept Fuel on `fflow_t`; dispatch charges the owning Flow.
6. Kept Flow handles and existing verbs unchanged.
7. Added a Flow/Thread split test to the host suite.

Next stages:

1. Add Thread identity/handles and lifecycle verbs once the native ABI is
   defined.
2. Add Serial multi-Thread behavior.
3. Add Parallel mode after shared Fuel, synchronization and cancellation
   contracts exist.
4. Remove the Flow-state mirror by deriving Flow state from its Thread set.

This order preserves the 147 core checks while making the central distinction
real.

## 11. Remaining open questions (post-v0.1)

- Whether Threads are joinable, detached, or both.
- Which Thread exit/fault policies are available to a Flow.
- Whether per-Thread capability restrictions are needed in the first ABI.
- How cancellation interacts with kernel operations and IPC.
- Whether a Thread can migrate while holding a kernel lock.
- How user and kernel stacks are represented across architectures.
- How Thread handles are exposed without making Threads a second authority
  root.
- Whether a Flow can have a suspended state independent of its Threads.

## 12. Test evidence

- `src/tests/test_main.c`: `flow/thread split` verifies that a Flow owns one
  `fthread_t`, that the Thread points back to the Flow, that its state is
  `CREATED` before start and `READY` after start, and that
  `finte_thread_current()` identifies the running Thread.
- `make test`: 22 host tests pass.
- `make selfcheck`: 147 core checks pass.
- `make qemu-test` / `make qemu-uefi-test`: Thread dispatch, blocking and
  preemption under BIOS and UEFI.
- `make qemu-disk-test`: `procs`/`services` run with migrated Threads.
- `make arm64-test`: the same core checks on arm64.
