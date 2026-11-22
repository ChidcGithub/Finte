# Flow contract

> **Status:** Partial — Flow task state is separated from one primary Thread.
> Suspend/resume, multiple Threads and Parallel Flows remain deferred.
>
> **Normative for:** task identity, Flow lifecycle, ownership of caps, Fuel,
> Channels and Arenas, and Flow-level concurrency policy.
>
> **Related:** [Thread](thread.md), [Fuel](fuel.md), [Capability](capability.md),
> [Channel](channel.md), [Arena](arena.md), [Scheduler](../kernel/scheduler.md).

## Resolved P0 decisions (2026-11-22)

1. **Flow owns task state; Thread owns execution state.** Capabilities, Fuel,
   Channels, Arenas, persona and task lifecycle belong to the Flow. Register
   context, kernel stack, CPU affinity and scheduler linkage belong to the
   Thread.
2. **v0.1 creates exactly one Thread per Flow.** The Thread is implicit and is
   created with the Flow, started by `finte_flow_start()`, and terminated when
   the Flow exits. Multiple Threads per Flow are deferred.
3. **Concurrency mode is immutable and Serial-only in v0.1.** No public mode
   selector is added yet. Parallel Flow creation is rejected or deferred until
   Thread creation, shared Fuel and cancellation are implemented and tested.
4. **Flow start is one-shot.** A Flow is created, may receive capabilities and
   affinity changes, is started once, and then runs until exit. Restart is a
   new Flow.
5. **Flow exit terminates its Thread and drops all Flow capabilities.** Waiting
   Flows are woken; the Thread is never resumed. A dead Flow is not restarted.
6. **Suspend/resume is defined but deferred.** The target state machine reserves
   `SUSPENDED`; v0.1 does not expose a suspend operation.
7. **Flow identity remains the public handle.** Thread handles are internal in
   v0.1; user-visible Thread creation and join are deferred to the native ABI.

These decisions resolve the P0 Flow questions. Remaining questions are listed
at the end of the document and are explicitly post-v0.1.

## 1. Definition

A **Flow** is a unit of work and task-level identity. It owns or references:

- a capability set;
- a Fuel budget;
- a set of Channels;
- a set of Arenas;
- task-level state and a persona;
- zero or more Threads (target model).

A Flow is not a CPU register context. The CPU context belongs to a Thread.
A Flow may exist without a running Thread, for example while blocked, sleeping,
suspended, or waiting to be created.

**Current implementation:** `fflow_t` owns task state and a pointer to one
primary `fthread_t`. The Thread owns the execution context, stack, CPU affinity
and scheduler links. Each Flow has exactly one Thread in the serial-only stage;
the Flow state mirrors it for compatibility. See [Thread](thread.md).

## 2. Current object model

`fflow_t` in `src/include/finte/object.h` now contains task state only:

| Field group | Current fields | Owner |
| --- | --- | --- |
| Identity | `self`, `slot`, `name`, `prog`, `persona` | Flow |
| Lifecycle | `state`, `started`, `exit_code` | Flow |
| Authority | `caps[]`, `cap_count` | Flow |
| Budget | `fuel`, `fuel_max` | Flow |
| Thread linkage | `thread` | Flow |
| Waiter list | `waiters` (Thread list) | Flow |
| Accounting | `stats` | Flow aggregate |
| Persona | `linux` | Flow |
| User binding | `user` | Flow |

`fthread_t` now holds the primary Thread:

| Field group | Current fields |
| --- | --- |
| Identity | `slot`, `flow` |
| State | `state`, `queued`, `blocking`, `wake_pending`, `wait_send` |
| Execution | `ctx`, `cycles`, `cpu`, `priority`, `quantum` |
| Scheduler links | `next_run`, `next_sleep`, `next_throttle`, `wait_next`, `wait_chan` |

The Thread context is created by `finte_flow_start()` with a 32 KiB stack and a
trampoline that calls `prog->entry(f)` and then `finte_flow_exit(f, 0)`. The
Flow state mirrors the primary Thread state in this serial-only stage.

## 3. Current lifecycle

The implemented states are:

```text
FREE
  |
  | finte_flow_start()
  v
READY <-------------------------------+
  |                                   |
  | dispatch                          | wake / enqueue
  v                                   |
RUNNING --- yield --------------------+
  |                                   |
  +--- block on Channel -------------> BLOCKED
  |                                   |
  +--- sleep ------------------------> SLEEPING
  |                                   |
  +--- fuel exhausted ---------------> THROTTLED
  |                                   |
  +--- exit -------------------------> DEAD
```

| State | Meaning | Exit paths |
| --- | --- | --- |
| `FREE` | Slot unused | `finte_flow_create()` |
| `READY` | Runnable, queued on a CPU | dispatch, migration |
| `RUNNING` | Currently executing on one CPU | yield, block, sleep, preempt, exit |
| `BLOCKED` | Waiting on a Channel or another Flow | wake, peer exit |
| `SLEEPING` | Waiting for `wake_ns` | timer wake |
| `THROTTLED` | Fuel exhausted | epoch refill |
| `DEAD` | Exited; scheduler owns final release | never resumed |

Current transition rules:

- `finte_flow_start()` is one-shot. A second start returns `FINTE_ERR_STATE`.
- `finte_flow_yield()` sets `READY` and switches to the CPU scheduler context.
- `finte_flow_block()` records the Channel and direction, then switches out.
  A wake that races the switch sets `wake_pending`; the scheduler re-enqueues
  the Flow when the switch returns.
- `finte_flow_wait_for()` puts the Flow on the target Flow's `waiters` list.
- `finte_flow_exit()` marks the Flow `DEAD`, wakes waiters, drops all caps and
  switches out permanently. A resumed dead Flow is a kernel panic.
- The scheduler owns one node reference while the Flow is started. Final
  release happens in `finish_dead()`.

There is no `SUSPENDED` state in the current kernel. Suspension and resume are
target features.

## 4. Target lifecycle

The target contract adds explicit suspended and terminating states:

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
   +----> SUSPENDED ------+
   |
   +----> TERMINATING
              |
              v
          TERMINATED
```

The final state machine must specify:

- who may request each transition;
- which transitions are legal from `BLOCKED` and `SUSPENDED`;
- what happens to associated Threads;
- what happens to queued Channel messages and in-flight operations;
- how Fuel is handled while suspended;
- how termination waits for active Threads.

## 5. Concurrency mode

A Flow selects its concurrency mode at creation. The mode is intended to be
immutable after creation until quiescence and synchronization semantics are
well defined.

### Serial Flow

- At most one Thread of the Flow executes at a time.
- Multiple Threads may exist, but execution is serialized.
- Task-level mutable state needs no additional kernel-imposed locking.
- This is the first implementation target.

### Parallel Flow

- Multiple Threads may execute simultaneously on different CPUs.
- Shared state requires explicit synchronization.
- Fuel is shared by the Flow or divided into explicitly accounted sub-budgets.
- Cancellation must coordinate all executing Threads.
- A Thread fault must have a defined effect on the Flow.
- Capability access is governed by the Flow's authority context, with any
  per-Thread restrictions represented explicitly.

**Current implementation:** one primary Thread per Flow. The Thread object and
serial scheduling exist; there is no parallel mode and no user-visible
concurrency selector. Parallel Flow is **Proposed**, not implemented.

## 6. Creation and authority

A Flow is created by an authorized component. Creation must not grant ambient
authority:

- the new Flow starts with no capabilities unless explicitly installed;
- capabilities are installed from the creator's table with rights at most
  equal to the source;
- the Flow receives a Fuel budget at creation;
- Channels and Arenas must be installed explicitly;
- the Flow's persona is selected at creation and does not grant authority.

Current `finte_flow_create()` takes `name`, `prog`, `fuel` and `persona`. It
allocates a node, assigns a per-CPU round-robin affinity, and initializes the
Fuel budget to `fuel ? fuel : 1024`. It does not take a capability list; the
creator installs caps afterwards.

## 7. Blocking, waiting and exit

### Channel blocking

A Flow may block for send or receive on a Channel. The wait list is per
Channel and per direction. Waking one waiter is the current policy; fairness
across waiters is FIFO insertion order.

### Waiting for another Flow

`finte_flow_wait_for()` attaches the caller to the target's `waiters` list.
When the target exits, all waiters are woken. A target that is already dead
returns immediately.

### Exit

On exit:

1. The Flow becomes `DEAD`.
2. All waiters are woken.
3. All capabilities are dropped.
4. The scheduler switches away and never resumes the Flow.
5. The scheduler finalizes the context and drops its node reference.

Queued Channel messages and their attached capabilities are owned by the
Channel, not the exiting sender, and are released when the Channel is
destroyed.

## 8. Invariants

1. A Flow has exactly one persona for its lifetime.
2. A Flow's capability rights never exceed the rights it was granted.
3. A Flow cannot be started twice.
4. A dead Flow is never scheduled again.
5. Fuel exhaustion throttles a Flow; it does not silently destroy it.
6. A Flow's capabilities are dropped before final release.
7. Wake-after-block races must not allow two CPUs to run the same Flow
   context concurrently. The current `blocking` / `wake_pending` handshake
   exists for this reason.

## 9. Remaining open questions (post-v0.1)

- How a Parallel Flow cancels and waits for active Threads.
- How a Thread fault affects the Flow's other Threads.
- Whether individual Threads may hold narrower capabilities.
- How Flow Groups interact with Flow lifecycle.
- How a Flow's Fuel is replenished while suspended.
- Whether Flow creation can atomically install an initial capability set.
- When and how a user-visible Thread handle and join operation are exposed.

## 10. Test evidence

- `src/tests/test_main.c`: Flow lifecycle, the Flow/Thread split test,
  capability installation, Channel and Arena integration.
- `src/core/selftest.c`: 147 core checks across six sections.
- `make qemu-test` / `make qemu-uefi-test`: shell and `demo` pipeline.
- `make qemu-disk-test`: `procs` and `services` flows.
- `make arm64-test`: the same core checks on arm64.

There is no test yet for suspend/resume, parallel Flows, or thread-level
capability restrictions.
