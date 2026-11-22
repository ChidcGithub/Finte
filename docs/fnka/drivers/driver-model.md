# Driver model

> **Status:** Partial — block drivers and filesystems are kernel-resident.
> There is no user-space driver framework, device manager or resource-grant
> contract yet.
>
> **Normative for:** where drivers run, how they receive resources, their
> lifecycle and their failure boundaries.
>
> **Related:** [Kernel boundary](../kernel/kernel-boundary.md), [Device model](device-model.md),
> [DMA model](dma-model.md), [Capability](../concepts/capability.md).

## 1. Selected direction

Finte uses a **hybrid driver model**:

- essential and selected critical drivers may run in kernel space;
- ordinary drivers should preferentially run in isolated user space;
- placement is based on boot dependency, latency/throughput requirements,
  fault containment, DMA/interrupt needs, platform support and complexity;
- “important” is not by itself a reason to keep a driver in the kernel.

Every kernel-resident driver increases the trusted computing base.

## 2. Current implementation

| Driver | Placement | Notes |
| --- | --- | --- |
| Legacy ATA PIO | Kernel | BIOS/SeaBIOS path |
| AHCI | Kernel | q35/UEFI path, DMA command lists |
| UEFI firmware Block I/O | Kernel | x86 UEFI path, firmware calls |
| Disk manager / partitions | Kernel | MBR/GPT parsing |
| lfc1 / ext4 / NTFS | Kernel | Filesystem implementations |
| Console / serial / GOP | Kernel | Early output and shell |
| Input (PS/2, COM1, firmware ConIn) | Kernel | Shell input |
| USB storage | Firmware Block I/O | No native USB stack |

There is no user-space driver, no driver registry, no device manager and no
MMIO/IRQ/DMA capability grant. The current position is a deliberate bootstrap
stage, not the target.

## 3. Target placement

### Kernel-resident core drivers

Likely candidates:

- CPU and SMP management;
- interrupt-controller support;
- fundamental timer support;
- memory-management hardware support;
- minimal early console;
- other strictly necessary boot-time components.

Keep this set small and review every addition.

### Trusted performance-critical drivers

Some drivers may remain in kernel space when there is a demonstrated
performance or execution requirement. This must be an explicit exception
supported by measurements and a security review. High throughput alone does
not justify kernel placement; shared memory and batching in user space may be
sufficient.

### Isolated user-space drivers

Potential candidates:

- many USB device drivers;
- input-device drivers;
- audio drivers and services;
- ordinary network-device drivers;
- hot-pluggable or independently recoverable devices.

A user-space driver receives explicit capabilities for its MMIO, IRQ, DMA and
device resources. It must not access arbitrary physical memory or device
registers.

## 4. Resource grants

The driver model must define how a driver receives:

- MMIO regions;
- interrupt lines or vectors;
- DMA buffers;
- device queues;
- bus resources;
- power-management operations.

Each grant is validated and constrained by the kernel. DMA requires special
care because a device may access memory independently of the CPU. Plan for an
IOMMU or equivalent platform isolation mechanism where available.

## 5. Lifecycle

```text
DISCOVERED
    |
    v
PROBED
    |
    v
INITIALIZED
    |
    v
RUNNING
    |
    v
STOPPING
    |
    v
REMOVED
```

The framework should define:

- discovery and matching;
- probe and initialization;
- resource acquisition;
- start and stop;
- error reporting;
- recovery or restart;
- removal and resource release.

Kernel-space and user-space drivers may share lifecycle semantics while using
different call and IPC mechanisms.

## 6. Failure and recovery

- A user-space driver failure should not automatically crash the kernel.
- The supervising service decides whether to restart, isolate or remove the
  driver.
- Restart requires reclaiming MMIO, IRQ and DMA resources without leaking
  authority.
- A device reset may be required before re-probing.
- Kernel-resident drivers should validate hardware responses and avoid
  unbounded loops.

## 7. Invariants

1. A driver only accesses resources granted to it.
2. A user-space driver cannot access arbitrary physical memory.
3. An IRQ is acknowledged exactly once.
4. DMA buffers have a defined owner and lifetime.
5. A driver crash does not corrupt unrelated kernel state.
6. Device removal quiesces DMA and interrupts before freeing resources.
7. Driver restart does not duplicate authority.
8. Kernel-resident drivers stay small and auditable.

## 8. Current limitations

- All drivers are in the kernel.
- No MMIO/IRQ/DMA capability model.
- No IOMMU.
- No user-space driver host.
- No device manager or hotplug.
- No driver restart or recovery framework.
- The x86 UEFI path depends on firmware drivers for storage and input.
- No native USB stack.

## 9. Open questions

- Which driver is the first user-space target?
- How are MMIO, IRQ and DMA capabilities represented?
- How does a user-space driver map device registers?
- How does a driver acknowledge and complete an interrupt?
- How is a device reset coordinated with other drivers?
- What is the recovery path for the boot storage device?
- How are firmware-provided devices replaced incrementally?
- What performance measurements justify kernel placement?

## 10. Test evidence

- `make qemu-disk-test`: ATA PIO and filesystems.
- `make qemu-uefi-disk-test`: AHCI and filesystems.
- `make qemu-uefi-test`: firmware Block I/O and shell.
- No user-space driver test yet.
