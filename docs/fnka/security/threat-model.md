# Threat model

> **Status:** Proposed — an initial working model. It records plausible threats
> and current mitigations; it is not a formal risk assessment or a security
> proof.
>
> **Related:** [Security model](security-model.md), [Capability](../concepts/capability.md),
> [Kernel boundary](../kernel/kernel-boundary.md), [Boot protocol](../kernel/boot-protocol.md).

## 1. Adversaries

| Adversary | Capability |
| --- | --- |
| Malicious or buggy user Flow | Can issue syscalls, pass bad pointers, send messages, allocate Arenas, fault |
| Compromised user service | Same as a Flow, plus whatever capabilities it was granted |
| Buggy kernel-resident driver | Full kernel privilege; can corrupt memory or hang the CPU |
| Malicious device / firmware | Can DMA, raise interrupts, return malformed data |
| Malicious boot media | Can replace the kernel, ramdisk or loader |
| Physical attacker | Can read storage, tamper with the boot chain, probe hardware |
| Network attacker | Future: no network stack yet |
| Side-channel observer | Timing, cache, speculative execution |

## 2. Assets and threats

### Capability and object integrity

| Threat | Current mitigation | Gap |
| --- | --- | --- |
| Forge a capability | Verbs take table indices; handles are not authority | None known for the current verb ABI |
| Reuse a stale handle | Generation checks | Handles are visible; table index reuse is local |
| Use another Flow's cap index | Cap table is per Flow | Must audit every new verb |
| Leak a capability through IPC | Attachments are copied into the receiver's table; sender index is not exposed | No revocation of delivered copies |
| Confused deputy | Explicit target Flow cap and `GRANT` right | `GRANT` semantics need review |
| Capability lifetime bug | Node refcounts; drop on Flow exit | No transitive revocation or revocation domain |

### Memory and isolation

| Threat | Current mitigation | Gap |
| --- | --- | --- |
| User reads kernel memory | Low 4 GiB supervisor-only; user window above 4 GiB | No NX/read-only user mappings |
| User writes another Flow's memory | Private PML4 | Shared mappings must be explicit |
| Kernel dereferences a bad user pointer | User copy validation | Needs a complete audit of every copy path |
| Use-after-free of an Arena page | Node refs and scoped caps | Mapping teardown/revocation incomplete |
| Integer overflow in size/range checks | Range checks in Arena and user copy | Needs systematic review |
| Stack overflow into adjacent memory | Fixed stacks | Guard pages not implemented |

### IPC and shared memory

| Threat | Current mitigation | Gap |
| --- | --- | --- |
| Message outlives sender | Channel owns queued message and cap refs | No move/transfer semantics |
| Receiver gets too many caps | Free-slot check before dequeue | Denial-of-service if receiver holds no slots |
| Sender closes while receiver waits | Close wakes waiters; receive drains then EOF | No half-close |
| Spoofed message tag | Tag is data; authority is the Channel cap | No message authentication |
| Starvation by a full queue | Fixed capacity; non-blocking `AGAIN` | No timeout/fairness policy |

### Scheduling and resource exhaustion

| Threat | Current mitigation | Gap |
| --- | --- | --- |
| One Flow spins forever | Timer preemption; Fuel throttle | Band 0 can starve band 1 |
| Flow creates unbounded objects | Fixed pools and per-Flow cap limit | Pool exhaustion still affects the system |
| Blocked Flow deadlock | Watchdog strike and deadlock status | No recovery policy |
| Wake/switch race runs a context twice | `blocking` / `wake_pending` handshake | Needs stress coverage, especially if stealing is re-enabled |
| Throttle refill starvation | Refill when the scheduler is idle | Busy systems can delay refill |

### Drivers and DMA

| Threat | Current mitigation | Gap |
| --- | --- | --- |
| Buggy driver corrupts kernel | Drivers are kernel code; no isolation | Move ordinary drivers to user space |
| Device DMA reads/writes arbitrary memory | Static kernel buffers; no IOMMU | No IOMMU or DMA window |
| Malicious device raises interrupts forever | In-kernel IRQ handling | No IRQ storm policy or user IRQ caps |
| Driver binds to an unauthorized device | No device manager yet | Resource grants and matching policy missing |
| Driver crash takes down the kernel | None for kernel-resident drivers | No restart/recovery framework |

### Boot and firmware

| Threat | Current mitigation | Gap |
| --- | --- | --- |
| Tampered kernel or ramdisk | Boot image is self-contained | No signature verification |
| Malicious boot info | Magic/version check; some range checks | No memory-map/pointer ownership validation |
| Firmware services compromised | x86 uses firmware Block I/O/ConIn | No native replacement; firmware TCB |
| Unsigned UEFI loader | Documented Secure Boot limitation | No signing path |
| Physical boot tampering | None | No measured boot or TPM integration |

### Side channels and physical access

| Threat | Current mitigation | Gap |
| --- | --- | --- |
| Timing/cache side channels | None | No mitigation |
| Speculative execution | Not addressed | Needs a documented policy |
| Cold-boot or memory extraction | None | Out of scope for now |
| Storage tampering | No encryption | No verified boot or disk encryption |

## 3. Threat priorities

1. **Capability revocation and lifetime.** Needed before user-space drivers and
   broader userland.
2. **DMA isolation.** A user-space driver without an IOMMU is not a meaningful
   security boundary.
3. **Boot protocol validation.** A unified handoff must validate every pointer
   and range.
4. **User-pointer audit.** Every copy path must be reviewed.
5. **Scheduler starvation.** Band 0 priority and throttle refill need a policy.
6. **Driver isolation.** The first ordinary driver should move to user space
   with explicit MMIO/IRQ capabilities.
7. **Audit surface.** Authority grants and denials need traceability.

## 4. Explicit non-goals

- Protecting against a compromised kernel.
- Protecting against a malicious hypervisor or physical attacker with full
  memory access.
- Formal side-channel resistance.
- Certified Secure Boot.
- Network-facing hardening before a network stack exists.

## 5. Open questions

- Which threats are in scope for the first native userland?
- Is signed boot required before any physical-hardware release?
- What is the minimum IOMMU support for user-space drivers?
- How are kernel audit records exposed without creating an information leak?
- What is the policy for capability revocation while a Flow is blocked?
- How are device resets and surprise removal handled securely?
