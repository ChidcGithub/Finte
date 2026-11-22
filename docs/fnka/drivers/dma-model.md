# DMA model

> **Status:** Open — kernel-resident AHCI uses DMA with static buffers.
> There is no IOMMU, DMA capability, pinning contract or user-space DMA path.
>
> **Normative for:** DMA buffer ownership, device address spaces, cache
> coherency and teardown.
>
> **Related:** [Driver model](driver-model.md), [Device model](device-model.md),
> [Memory model](../kernel/memory-model.md), [Security model](../security/security-model.md).

## 1. Current implementation

- x86_64 AHCI uses DMA command lists, FIS structures and PRDT entries with
  physical addresses.
- The AHCI driver uses static, aligned kernel buffers.
- Legacy ATA PIO uses programmed I/O, not DMA.
- x86 UEFI firmware Block I/O performed DMA inside firmware; the current
  loader exits boot services, so only native AHCI DMA remains.
- There is no IOMMU configuration, no DMA address-space abstraction and no
  user-space DMA buffer grant.
- The arm64 virtio-mmio block driver passes physical descriptor/data addresses
  to the device and polls for completion; it assumes QEMU's coherent DMA and
  has no cache-maintenance or IOMMU path of its own.

The current model is safe only because all DMA users are kernel-resident and
use static buffers that are never freed or reused concurrently.

## 2. Why DMA is different

A device can access memory without CPU involvement. Kernel pointer validation
and page-table permissions do not, by themselves, constrain a bus-master
device. Without an IOMMU or equivalent, a malicious or buggy device can read or
write arbitrary physical memory.

DMA therefore needs:

- an explicit buffer owner;
- pinning for the duration of the transfer;
- a device-visible address;
- cache coherency rules;
- completion and error handling;
- revocation and quiescence on teardown;
- IOMMU isolation where available.

## 3. Target contract

### Buffer ownership

- A DMA buffer has one owner responsible for its lifetime.
- The owner pins the buffer before mapping it for a device.
- The buffer cannot be freed or repurposed until the transfer completes and
  the device mapping is revoked.
- Shared buffers need explicit synchronization and lifetime rules.

### Device address space

- Each device or driver receives a constrained DMA address window.
- The kernel translates CPU physical addresses to device-visible addresses.
- An IOMMU, where available, enforces the window.
- Without an IOMMU, the platform must document the reduced isolation and
  restrict DMA-capable drivers accordingly.

### Cache coherency

- x86_64: DMA is coherent with CPU caches for normal memory; the driver still
  needs ordering and completion rules.
- arm64: cache maintenance and memory barriers may be required depending on
  the device and platform. The contract must state whether the platform is
  coherent and who performs maintenance.

### Completion

- A transfer is complete only after the device reports completion and the
  driver observes the required ordering.
- Timeouts and device resets are part of the contract.
- A failed transfer must not leave a buffer pinned forever.

## 4. Invariants

1. A DMA buffer is not freed while a device can access it.
2. A device cannot access memory outside its granted window.
3. A DMA mapping is revoked before the buffer is reused.
4. Completion is observed with the required memory ordering.
5. A device reset quiesces outstanding DMA.
6. Cache maintenance follows the platform contract.
7. A user-space driver cannot create an arbitrary physical DMA mapping.
8. A failed transfer releases its pins and mappings.

## 5. Current limitations

- No IOMMU support.
- No DMA capability or per-device window.
- No pinning API.
- No user-space DMA.
- No generic cache-maintenance contract.
- AHCI uses static buffers; no dynamic DMA allocation.
- No DMA fault reporting or recovery.

## 6. Open questions

- IOMMU support on x86_64 (VT-d/AMD-Vi) and arm64 (SMMU).
- Whether the first user-space driver uses a pre-mapped DMA window.
- Buffer pinning and revocation API.
- Coherent vs non-coherent platform rules.
- DMA-buf-like sharing between drivers.
- Device reset and outstanding-transfer handling.
- How firmware-owned DMA is quiesced before native driver takeover.

## 7. Test evidence

- `make qemu-uefi-disk-test`: AHCI DMA reads/writes in QEMU.
- No IOMMU, fault-injection, pinning or teardown test yet.
