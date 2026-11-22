# FNKA contracts

This directory is the normative contract set for **Finte Native Kernel
Architecture (FNKA)**. It describes both the behavior the current kernel
provides and the target behavior the project intends to provide.

The project roadmap is planning context. This directory is the source of truth
for terms, invariants, ownership rules, and support claims. If a document here
disagrees with the code, the code is the current behavior and the document must
be corrected or marked as a target.

## Status labels

Every contract uses one of four labels:

| Label | Meaning |
| --- | --- |
| **Implemented** | The behavior exists in the kernel and is covered by a repeatable test. |
| **Partial** | The behavior exists, but has known gaps, limited coverage, or a different shape than the target contract. |
| **Proposed** | The direction is accepted, but no implementation exists yet. |
| **Open** | No normative decision has been made. The document records the questions. |

A feature is supported only to the extent demonstrated by a repeatable test on
a named platform. "Boots in QEMU AArch64 `virt`" is a narrower claim than
"supports AArch64 hardware."

## Reading order

1. [Concepts overview](concepts/overview.md)
2. [Flow](concepts/flow.md)
3. [Thread](concepts/thread.md)
4. [Capability](concepts/capability.md)
5. [Fuel](concepts/fuel.md)
6. [Channel](concepts/channel.md)
7. [Arena](concepts/arena.md)
8. [Kernel boundary](kernel/kernel-boundary.md)
9. [Scheduler](kernel/scheduler.md)
10. [Memory model](kernel/memory-model.md)
11. [Interrupt model](kernel/interrupt-model.md)
12. [Native syscall ABI](kernel/syscall-abi.md)
13. [Boot protocol](kernel/boot-protocol.md)
14. [Testing contract](kernel/testing.md)
15. [Driver model](drivers/driver-model.md)
16. [Device model](drivers/device-model.md)
17. [DMA model](drivers/dma-model.md)
18. [x86_64 port](architecture/x86_64.md)
19. [arm64 port](architecture/arm64.md)
20. [Security model](security/security-model.md)
21. [Threat model](security/threat-model.md)

Flow Group is an optional organization layer:
[concepts/flow-group.md](concepts/flow-group.md).

## Current baseline

- Version: `0.0.1`, built as `0.0.1-<short commit>`.
- Architectures: x86_64 and AArch64.
- Primary validation: QEMU x86_64 (SeaBIOS and OVMF) and QEMU AArch64
  `virt` (direct ELF and AAVMF).
- Core tests: 20 host tests; 147 core checks on x86_64 and arm64.
- User mode: x86_64 ring 3 only.
- Release media: x86_64 BIOS/UEFI IMG and ISO; arm64 UEFI IMG and ISO.

## Status matrix

| Area | Current | Target | Evidence |
| --- | --- | --- | --- |
| Node table and handles | Implemented | Keep generation-checked handles | `src/core/object.c`, host tests |
| Capability authority | Partial | Default deny, derivation, transfer, revocation | `src/core/cap.c`, `src/tests/test_main.c` |
| Flow lifecycle | Partial | Multi-Thread, suspend/resume, termination | `src/core/flow.c`, core checks |
| Thread execution entity | Partial | Handles, multiple Threads, parallel execution | `fthread_t`, one primary Thread per Flow |
| Flow concurrency | Partial | Serial and Parallel modes | one Thread per Flow; Serial only |
| Fuel accounting | Partial | Flow-level CPU budget shared by Threads | `src/core/sched.c` |
| Channel transport | Partial | Separate endpoint authority; explicit ownership | `src/core/channel.c` |
| Arena mapping | Partial | Explicit lifetime, revocation, cross-arch user maps | `src/core/arena.c`, x86 process maps |
| Scheduler | Partial | Multi-Thread dispatch, work stealing, policies | `src/core/sched.c` |
| Physical/virtual memory | Partial | Both architectures own MMU and allocator | x86 only for MMU/user |
| Interrupts and timers | Partial | IDT/GIC, timer IRQ, IPI on both | x86 only for interrupts |
| SMP | Partial | Same semantics on both | x86_64 only |
| Boot handoff | Partial | One versioned protocol with owned memory | `src/include/finte/boot.h`, two loaders |
| Kernel/user boundary | Partial | Small kernel, isolated drivers/services | `src/arch/x86_64/process.c` |
| Driver framework | Partial | Hybrid kernel/user drivers | in-kernel block/filesystem code |
| Native ABI | Partial | Stable Finte ABI and native libc | verb ABI and x86 demo syscalls |
| Security model | Partial | Capability revocation, DMA isolation, measured TCB | this directory |
| Tests | Partial | Cross-architecture semantic matrix | host, x86 BIOS/UEFI/disk, arm64 core |

## P0 decisions

The six P0 concept contracts now contain a **Resolved P0 decisions
(2026-11-22)** section:

- [Flow](concepts/flow.md) — task/Thread ownership, Serial-only v0.1, one-shot
  start, exit and deferred suspend.
- [Thread](concepts/thread.md) — internal one-per-Flow Thread, scheduler
  dispatch, fault and cancellation policy, no per-Thread caps.
- [Capability](concepts/capability.md) — frozen v0.1 rights, per-object masks,
  copy-only grant and individual revocation.
- [Fuel](concepts/fuel.md) — abstract quantum unit, Flow ownership, event-driven
  refill and strict bands.
- [Channel](concepts/channel.md) — one object with directional rights, copy/share
  attachments, full close and fixed capacity.
- [Arena](concepts/arena.md) — writable scoped x86 mappings, no unmap/revocation
  in v0.1, deferred read-only and cross-arch mappings.

The first-stage Flow/Thread refactor is implemented: `fthread_t` owns execution
state and the scheduler dispatches Threads. See [Thread](concepts/thread.md).

## Known gaps and documentation debt

These are deliberately called out so the docs do not overstate the current
system:

- **Work stealing is currently disabled.** `steal_one()` exists in
  `src/core/sched.c`, but the scheduler guards the call with `if (!t && 0)`.
  Current `smp` output therefore reports `steals=0`. Migration works.
- **The first-stage Flow/Thread split is in place.** `fthread_t` owns the
  context, stack, CPU affinity, scheduler links and blocking state; `fflow_t`
  owns caps, Fuel, persona and task state. The Flow state still mirrors its
  primary Thread, and each Flow has exactly one Thread.
- **The x86_64 UEFI loader does not call `ExitBootServices`.** It passes
  firmware pointers into the kernel and keeps firmware Block I/O and ConIn
  available. The arm64 UEFI loader does exit boot services.
- **arm64 is not a peer port yet.** It has a working UEFI boot path, PL011,
  generic counter, PSCI shutdown and the 147 core checks, but no MMU,
  GIC, timer interrupts, SMP or user mode.
- **The Linux persona is experimental.** It demonstrates syscall translation
  and a handful of POSIX-shaped operations. It is not Linux compatibility.
- **The native ABI is not frozen.** The verb table is the current internal
  ABI; ring-3 uses a small demo syscall set.

## How to change a contract

1. Decide whether the statement is a decision, a proposal, or an open question.
2. Update the status label and the status matrix in this README.
3. Add or update a test reference. If no test exists, do not label it
   Implemented.
4. Record ownership and failure semantics, not only the happy path.
5. Keep architecture-specific behavior in the architecture documents; keep
   shared semantics in the concept and kernel documents.

## Non-goals of this directory

- It is not a full formal specification or a security proof.
- It is not a hardware compatibility list.
- It does not replace the roadmap or release notes.
- It does not claim that every proposed item will ship in a particular
  release.
