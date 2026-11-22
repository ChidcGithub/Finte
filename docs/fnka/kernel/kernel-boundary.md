# Kernel/user protection boundary

> **Status:** Partial — x86_64 has ring-3 user processes and a syscall gate.
> The kernel still contains all drivers and filesystems, and arm64 has no user
> mode.
>
> **Normative for:** trust levels, what belongs in the kernel, how components
> cross the boundary, and what the TCB currently contains.
>
> **Related:** [Capability](../concepts/capability.md), [Security model](../security/security-model.md),
> [Threat model](../security/threat-model.md), [Driver model](../drivers/driver-model.md).

## 1. Trust levels

The architecture uses three logical levels. They describe authority and
isolation, not necessarily three process types in the first implementation.

### Level 0 — Kernel Core

Responsibilities:

- CPU and per-CPU management;
- scheduler and dispatch;
- Flow and Thread lifecycle primitives;
- capability validation;
- physical and virtual memory management;
- address-space switching;
- Channel transport primitives;
- Arena mapping primitives;
- interrupt core;
- fundamental resource management.

The Kernel Core should remain small enough to reason about, test and audit.

### Level 1 — Trusted components

Potential contents:

- essential early drivers;
- selected performance-critical drivers;
- services that require privileged execution for a justified reason.

“Trusted” does not mean unrestricted access to every kernel object. Each
component needs an explicit authority boundary.

### Level 2 — Isolated components

Potential contents:

- ordinary device drivers;
- filesystem services;
- network services;
- logging and diagnostics;
- configuration and other system services.

These should run in user space where practical, communicating through Channels
and receiving resources through capabilities.

## 2. Current boundary

| Component | Current level | Notes |
| --- | --- | --- |
| Scheduler, memory, caps, Channels, Arenas | Level 0 | In-kernel |
| x86_64 ATA/AHCI block drivers | Level 0 | In-kernel |
| UEFI firmware Block I/O and ConIn | Not used by current loaders | Only available when a producer sets `fw_services` |
| Disk manager and lfc1/ext4/NTFS | Level 0 | In-kernel |
| Shell | Level 0 | In-kernel shell |
| `init.elf` and `ipc.elf` | Level 2 | x86 ring-3 demo processes |
| Linux persona | Level 0 | In-kernel translation layer |
| arm64 drivers/services | Level 0 | No user mode yet |

The current x86_64 and arm64 UEFI loaders call `ExitBootServices` and clear
the firmware pointers. The kernel no longer relies on UEFI Block I/O or ConIn.
Firmware-service support remains in the code path only for a producer that
advertises `fw_services`. See [Boot protocol](boot-protocol.md).

## 3. Cross-boundary operations

### System calls

On x86_64, ring-3 enters through `int 0x80` and reaches
`finte_process_syscall()`. The current demo syscall set covers write, getpid,
exit, time, sleep, yield, IPC send/receive, shared-memory address and Arena
mapping. It is not the final native ABI.

Kernel-internal verbs use `finte_syscall(f, nr, args[6])` and a fixed verb
table. Verbs are serialized by the global core lock.

### Capability validation

Every cross-boundary operation that touches an object must validate a
capability in the calling Flow's table. The kernel never trusts a raw object
handle supplied by userland as authority.

### User memory access

The x86 user-copy path walks the process page tables and validates:

- the virtual range is inside the user window;
- the page is present and user-accessible;
- writes require writable mappings.

Kernel pointers are never copied directly from user-controlled addresses.

### Faults

A user fault kills the user process and records a watchdog strike. A kernel
fault panics. The target contract must define per-fault policy for Threads and
Flows; see [Thread](../concepts/thread.md).

## 4. What belongs in the kernel

Keep in Level 0:

- CPU, interrupt and memory mechanisms;
- scheduler and context switch;
- capability and handle validation;
- Channel transport primitives;
- Arena mapping primitives;
- minimal early console;
- boot-time CPU and platform initialization.

Do not add without review:

- filesystem implementations;
- network protocols;
- ordinary device drivers;
- policy-heavy device discovery;
- logging and configuration services;
- desktop or application services.

Every kernel-resident driver increases the TCB and the impact of a defect.

## 5. Migration strategy

The move from the current kernel to the target boundary should be incremental:

1. Define stable interfaces for memory, IRQ, DMA and device resources.
2. Move one ordinary driver or service out of the kernel at a time.
3. Preserve a working boot and test path at every step.
4. Keep essential boot drivers in the kernel until a user-space replacement
   has a recovery path.
5. Do not move a component merely to match a diagram.

The first candidates are ordinary, recoverable devices and filesystem
services, not the interrupt controller, timer or boot console.

## 6. Invariants

1. A Level 2 component cannot access kernel memory or arbitrary physical
   memory.
2. A Level 2 component can only use capabilities granted to it.
3. A Level 1 component has an explicit, reviewed authority boundary.
4. Crossing the boundary validates capabilities and user pointers.
5. A user fault does not corrupt kernel state.
6. A kernel fault is not silently converted into a user-visible success.
7. The TCB is documented and does not grow by accident.

## 7. Current limitations

- All drivers and filesystems are in the kernel.
- The shell is in the kernel.
- x86 UEFI has no native USB-HID keyboard driver; USB-only systems need one
  for a non-serial console.
- arm64 has no user mode or user/kernel boundary.
- There is no user-space driver framework or device manager.
- The native syscall ABI is not stable.
- The Linux persona executes in the kernel.

## 8. Open questions

- Which drivers must remain in the kernel for boot and recovery?
- What is the first user-space driver target?
- How are MMIO, IRQ and DMA capabilities represented?
- How does a user-space driver recover from a device fault?
- What is the stable native syscall ABI?
- How does the shell become a user-space service?
- How is the TCB measured and reviewed over time?

## 9. Test evidence

- `src/arch/x86_64/process.c`: ring-3 process creation, page tables, user copy
  validation and user faults.
- `procs` / `services`: isolation, IPC, shared memory, Arena caps and fault
  kill.
- `make qemu-uefi-test`: UEFI boot and shell.
- No test yet for a user-space driver or arm64 user mode.
