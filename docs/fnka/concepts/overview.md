# FNKA concepts overview

> **Status:** Partial — the five mechanisms exist and the first-stage Flow/Thread
> split is in place. Multi-Thread Flows and several ownership rules are still
> targets.
>
> **Scope:** Shared vocabulary and relationships. Individual contracts are in
> the linked documents.

## Definition

**Finte Native Kernel Architecture (FNKA)** is Finte's native kernel
organization and execution model. Its distinguishing traits are:

- Capability-centric authority.
- Flow-based task identity.
- Thread-based CPU execution.
- Explicit resource budgeting through Fuel.
- Message-oriented communication through Channels.
- Explicit memory sharing through Arenas.
- A hybrid kernel/user driver model.
- A shared core across x86_64 and AArch64.

The short form is:

> **Capability-centric · Flow-based · Resource-aware · Hybrid-isolation**

FNKA is not defined by mixing monolithic and microkernel features. It is
defined by contracts: what each abstraction means, who owns each resource, how
authority is granted, and what happens when a component fails.

## The five mechanisms

| Mechanism | Primary responsibility | Key invariant |
| --- | --- | --- |
| Capability | Authority to access an object or operation | No valid authority, no access |
| Flow | Task identity and task-level state | Independent of a particular CPU execution context |
| Fuel | Resource budget | Creating more Threads must not create free budget |
| Channel | Communication between components | Endpoint authority and message ownership are explicit |
| Arena | Memory region and sharing domain | Mapping, ownership and lifetime are explicit |

They are related but must not be collapsed:

- A **Flow** holds or references capabilities, Fuel, Channels and Arenas.
- A **Thread** executes a Flow; its CPU context is not the Flow identity.
- A **Capability** is authority, not a name or a raw object ID.
- A **Channel** carries messages; bulk data belongs in an Arena.
- An **Arena** is memory; mapping it does not grant unrelated authority.

## Target relationship model

```text
                 +----------------------+
                 |        Flow          |
                 |  caps, fuel, state   |
                 |  channels, arenas    |
                 +----------+-----------+
                            |
              +-------------+-------------+
              |                           |
        +-----v-----+               +-----v-----+
        |  Thread A |               |  Thread B |
        | CPU ctx   |               | CPU ctx   |
        +-----------+               +-----------+
```

A Flow may have zero, one, or many Threads. A Serial Flow permits at most one
Thread to execute at a time. A Parallel Flow permits more than one, with
explicit synchronization and shared-budget rules.

**Current implementation:** the first-stage split is in place. `fflow_t` owns
caps, Fuel, Channel/Arena references, persona and task state; `fthread_t` owns
the context, stack, CPU affinity, scheduler links and blocking state. Each Flow
has exactly one primary Thread in this serial-only stage, and the Flow state
mirrors it.

## Principles

### 1. Mechanisms and policies are separable

The kernel provides mechanisms such as address-space management, scheduling
primitives, capability validation and IPC transport. Policies such as
scheduling classes, service restart behavior and device recovery should live
outside the smallest trusted mechanism where practical.

### 2. Authority is explicit

A component must not gain access because it knows an address, a numeric ID or
another component's name. Access requires a valid capability checked at the
kernel boundary.

### 3. Execution is separate from task identity

A Flow describes a task. A Thread provides an execution context. Task
suspension, migration or temporary absence of an executing Thread should not
change the meaning of the task.

### 4. Ownership and lifetime are explicit

Memory regions, messages, device resources, capabilities and execution objects
need defined owners and destruction rules. Ambiguous ownership causes leaks,
use-after-free and authority leaks.

### 5. Failure boundaries are deliberate

A failure in an ordinary user-space driver or service should not automatically
compromise the kernel or unrelated services. Kernel-resident components must
stay small and reviewable.

## Trust levels

These are authority and isolation levels, not necessarily three process types
in the first implementation.

| Level | Contents | Current status |
| --- | --- | --- |
| 0 — Kernel Core | CPU, scheduler, Flow/Thread primitives, capabilities, memory, Channels, Arenas, interrupts | Implemented in one kernel image |
| 1 — Trusted components | Essential early drivers, selected performance-critical drivers, justified privileged services | Proposed; in-kernel drivers are currently part of the core |
| 2 — Isolated components | Ordinary drivers, filesystem/network/logging/configuration services | Partial; x86 ring-3 demos exist, no user driver framework |

## Shared core, separate HALs

The target structure is:

> **One Kernel Core + Two Architecture HALs + Multiple Platform Backends.**

The core owns shared semantics. The HAL owns CPU mechanisms: context switch,
interrupt masking, address-space activation, atomics, per-CPU state, time and
cache maintenance. The platform layer owns firmware and machine description:
UEFI, ACPI, Device Tree, PCIe, interrupt controllers, timers and CPU startup.

**Current implementation:** `src/include/finte/hal.h` is the common HAL.
`src/arch/x86_64/` and `src/arch/arm64/` provide the two ports. Platform and
architecture code are still mixed, and the arm64 HAL hard-codes QEMU `virt`
devices. The scheduler now dispatches `fthread_t` objects; user-visible Thread
handles and parallel Flows are not implemented.

## Contract vocabulary

- **Decision:** A direction selected for the project.
- **Proposal:** A recommended design that still needs detailed specification
  and implementation review.
- **Open question:** A topic that must be resolved before the related
  subsystem is stable.
- **Implemented / Partial / Proposed / Open:** Support labels defined in the
  [README](../README.md).

## Current reading of the baseline

The current kernel already has the vocabulary and much of the mechanism:

- capability tables with per-Flow rights;
- a Flow object with lifecycle, Fuel, caps, Channels and Arenas;
- per-CPU run queues, affinity and migration;
- ring-3 user processes on x86_64;
- a common HAL and two architecture ports.

What it does not yet have is the separation that makes the model durable:
Threads, a stable native ABI, a unified boot handoff, cross-architecture
interrupt/SMP parity, and a driver framework outside the kernel.

The next architectural milestone is therefore **contracts and separation**, not
more features. See the [Flow](flow.md) and [Thread](thread.md) documents for the
central refactor.
