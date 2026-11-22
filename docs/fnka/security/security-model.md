# Security model

> **Status:** Partial — capabilities, handle generations, ring-3 isolation and
> user-pointer validation exist. Revocation, DMA isolation, signed boot,
> user-space drivers and a stable audit model do not.
>
> **Normative for:** assets, trust boundaries, security invariants and the
> current limits of isolation.
>
> **Related:** [Threat model](threat-model.md), [Capability](../concepts/capability.md),
> [Kernel boundary](../kernel/kernel-boundary.md), [Driver model](../drivers/driver-model.md).

## 1. Security goals

1. **Kernel integrity** — a user Flow cannot corrupt kernel memory or control
   CPU state.
2. **Authority confinement** — a Flow can only use objects for which it holds
   a valid capability with the required rights.
3. **Memory isolation** — Flows cannot read or write each other's private
   memory without an explicit shared mapping.
4. **IPC integrity** — a message cannot forge authority, leak a sender's
   capability index, or outlive its backing object.
5. **Boot integrity** — the kernel is loaded from the intended image and does
   not trust unvalidated boot data.
6. **Availability** — a faulty or malicious Flow cannot trivially starve the
   scheduler or exhaust all kernel objects.
7. **Recoverability** — a failing ordinary driver or service should not
   automatically take down the kernel.

## 2. Assets

- Kernel code, data, page tables, interrupt state and scheduler state.
- Capability tables and node reference counts.
- Flow, Thread, Channel, Arena and device objects.
- User address spaces, stacks and shared mappings.
- Boot image, kernel image, initrd and configuration.
- Persistent filesystems and storage.
- Device MMIO, IRQ lines, DMA windows and firmware services.
- Availability of CPU time and object pools.

## 3. Trust boundaries

| Boundary | Current status |
| --- | --- |
| Ring 3 → kernel syscall | x86_64 only; user pointers validated |
| Kernel → firmware | Boot-time only; both UEFI loaders exit Boot Services |
| Kernel → device | In-kernel drivers; no IOMMU |
| Bootloader → kernel | Two handoff formats; boot data only partially validated |
| Flow → Flow | Capability and page-table isolation |
| Service → service | Channels and explicit Arena caps |
| Kernel → userland | Native ABI not stable |

## 4. Principles

### Default deny

A new Flow has no capabilities. Access requires an explicit grant.

### Least authority

Capabilities should carry the minimum rights and, where useful, a range
(`FINTE_CAP_SCOPED`). A send-only endpoint should not carry receive or control
authority.

### Explicit ownership

Every object, message, mapping, DMA buffer and device resource needs an owner
and a destruction rule.

### Fail closed

A missing capability, stale handle, wrong object type or invalid range returns
an explicit error. The kernel does not guess or silently widen authority.

### Small TCB

Keep the kernel core, boot chain and kernel-resident drivers small and
reviewable. Every addition to the TCB needs a security rationale.

### No ambient hardware access

A user-space driver must receive MMIO, IRQ and DMA capabilities. It must not
map arbitrary physical memory or program arbitrary device registers.

## 5. Current mechanisms

- **Capability table per Flow.** Verbs take local table indices, not raw
  handles as authority.
- **Generation-checked handles.** A reused node slot increments the generation,
  so stale handles fail.
- **Node reference counting.** Caps, queued messages and scheduler ownership
  retain objects.
- **Ring-3 isolation on x86_64.** Private PML4, supervisor-only low identity
  map, user window above 4 GiB.
- **User-pointer validation.** The x86 copy path checks the user range, page
  presence and write permission.
- **Per-CPU TSS/GDT.** User processes can run on APs without sharing a single
  TSS.
- **Capability-scoped Arenas.** Page ranges are reserved and mapped only with a
  scoped writable capability.
- **Channel attachment references.** Queued messages retain their attached
  nodes.
- **Watchdog.** Three abnormal events lead to panic and power-off.
- **No ambient path namespace in the core.** Paths belong to the persona.

## 6. Current limitations

- No transitive capability revocation or revocation domains.
- No capability move/transfer semantics.
- No per-object rights mask; rights are enforced per verb.
- No IOMMU or DMA isolation.
- No user-space drivers or IRQ/MMIO capabilities.
- No signed boot or Secure Boot integration; the UEFI loaders are unsigned.
- The boot memory map/ACPI/DTB are validated but not yet consumed by the
  allocator or device model. x86 UEFI has no native USB-HID input.
- The Linux persona runs in the kernel.
- The global core lock serializes verbs; a blocking or long-running verb can
  affect other CPUs.
- No audit log beyond counters.
- Fixed user virtual layout; no ASLR or KASLR.
- No side-channel mitigations (cache, timing, speculation).
- No physical hardware qualification.

## 7. Security invariants

1. A Flow cannot access an object without a valid capability.
2. A capability never grants more rights than its parent.
3. A stale handle never aliases a new object.
4. Ring 3 cannot read or write supervisor memory.
5. A user pointer is validated before the kernel dereferences it.
6. A queued message's attached nodes remain alive until delivery or release.
7. A dead Flow's capabilities are dropped before final release.
8. A device cannot access memory outside its granted DMA window (target; not
   yet enforced).
9. A user-space driver cannot access MMIO or IRQs without a capability
   (target).
10. Boot data is validated before use (target; partially implemented).

## 8. Security work priorities

1. Define the final rights vocabulary and per-object masks.
2. Define revocation semantics and tests.
3. Unify the boot protocol and validate every boot-provided range.
4. Add an IOMMU/DMA isolation plan before user-space drivers.
5. Define MMIO/IRQ/DMA capabilities.
6. Move ordinary drivers and the shell out of the kernel incrementally.
7. Add an audit/telemetry surface for authority grants and denials.
8. Add signed boot or document the unsigned-boot risk explicitly.

## 9. Out of scope for the current baseline

- Formal verification.
- A complete side-channel-resistant implementation.
- A certified secure boot chain.
- Multi-tenant isolation guarantees on physical hardware.
- Network-facing security (no network stack yet).
