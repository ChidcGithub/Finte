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

- Version: `0.0.2`, built as `0.0.2-<short commit>`.
- Architectures: x86_64 and AArch64.
- Primary validation: QEMU x86_64 (SeaBIOS and OVMF) and QEMU AArch64
  `virt` (direct ELF and AAVMF).
- Core tests: 24 host tests; 147 core checks on x86_64 and arm64; x86_64
  1-CPU process ABI suite; arm64 platform probes for MMU, GIC/timer, 4-CPU
  SMP, EL0 user mode, the process ABI suite and virtio block storage.
- User mode: x86_64 ring 3 and arm64 EL0 share one process core with a fd
  table, `mmap`, `fork`, `exec`, SIGUSR1 delivery and cap transfer.
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
| Arena mapping | Partial | Explicit lifetime, revocation, cross-arch user maps | `src/core/arena.c`, both process backends |
| Scheduler | Partial | Multi-Thread dispatch, work stealing, policies | `src/core/sched.c` |
| Physical/virtual memory | Partial | Both architectures own MMU and allocator | PMM; shared process core; Flow-owned refcounted spaces; x86 CR3 + arm64 ASID roots |
| Interrupts and timers | Partial | IDT/GIC, timer IRQ, IPI on both | x86 IDT/APIC/PIT; arm64 GICv2/generic timer/PPI 30 |
| SMP | Partial | Same semantics on both | 4-CPU x86 and 4-CPU arm64 PSCI; stealing disabled |
| Boot handoff | Implemented | Parse ACPI/DTB in platform services | `boot.h`, `bootinfo.c`, three producers; PMM consumes the map |
| Kernel/user boundary | Partial | Small kernel, isolated drivers/services | `src/core/process.c` + x86/arm64 arch backends |
| Driver framework | Partial | Hybrid kernel/user drivers | in-kernel block/filesystem code |
| Native ABI | Partial | Stable Finte ABI and native libc | verb ABI; fd table, mmap, COW fork, waitpid, exec, signals, cap transfer, service registry and ET_DYN load on both arches |
| Security model | Partial | Capability revocation, DMA isolation, measured TCB | this directory |
| Tests | Partial | Full cross-architecture semantic matrix | 24 host, x86 BIOS/UEFI/disk + 1-CPU ABI, arm64 MMU/timer/SMP/user/ABI/disk direct + UEFI |

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
- **Boot Protocol v1 is produced, validated and consumed by the PMM.** ACPI/DTB
  discovery is passed through but not parsed by the kernel. Both UEFI loaders
  call `ExitBootServices`; x86 UEFI input is serial/PS/2 until a native USB-HID
  driver exists.
- **Both architectures share one process core.** ELF loading, user copy,
  syscall dispatch, fd tables, `mmap`, `fork`, `exec`, signals and cap transfer
  live in `src/core/process.c`; arch files only own page tables, roots, trap
  frames and EL0/ring-3 entry. Address spaces are refcounted and Flow-owned, so
  flow death frees page tables even without `proc_destroy()`.
- **The process ABI is still a demo subset.** Fork is copy-on-write,
  anonymous mappings are capped, signals cover SIGUSR1/SIGKILL only, and there
  is no job control or general POSIX fd layer. arm64 still lacks ACPI/DTB
  parsing and GICv3; static-heap fallback remains for direct `-kernel` boot.
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
