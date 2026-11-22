# Device model

> **Status:** Partial — raw block devices are abstracted through the HAL, but
> there is no generic device registry, bus model, driver matching or hotplug.
>
> **Normative for:** device identity, discovery, registry, resource assignment
> and lifecycle.
>
> **Related:** [Driver model](driver-model.md), [DMA model](dma-model.md),
> [Kernel boundary](../kernel/kernel-boundary.md).

## 1. Current implementation

- `finte/hal.h` exposes raw block devices through `hal_blk_count()`,
  `hal_blk_info()`, `hal_blk_read()` and `hal_blk_write()`.
- x86 BIOS uses ATA PIO.
- x86 UEFI uses AHCI where available and firmware Block I/O otherwise.
- The disk manager parses MBR/GPT partitions and mounts lfc1, ext4 and NTFS.
- There is no generic device object, registry, bus enumeration or driver
  matching.
- There is no hotplug or device removal path beyond boot-time enumeration.

## 2. Target model

A Device Manager should coordinate:

- device discovery;
- device registry;
- driver matching;
- resource assignment;
- device lifecycle;
- hotplug events where supported;
- driver/service health;
- access-control policy.

Discovery and authority assignment are separate steps. Discovering a device
must not automatically grant every service permission to operate it.

## 3. Proposed device object

| Field | Meaning |
| --- | --- |
| `device_id` | Stable identity |
| `bus` | PCIe, platform, USB, firmware, etc. |
| `vendor`, `device` | Identifiers where available |
| `class` | Device class |
| `resources` | MMIO, IRQ, DMA, bus ranges |
| `driver` | Bound driver or service |
| `state` | Discovered, probed, running, stopping, removed |
| `owner` | Device-manager policy owner |
| `health` | Error and recovery state |

## 4. Resource assignment

Resource assignment should produce explicit grants:

- MMIO range capability;
- IRQ capability;
- DMA window/buffer capability;
- bus-master enablement;
- power-management operations;
- device-specific queues.

The kernel validates each grant and records the owner. A grant is revocable
when the driver or device is removed.

## 5. Lifecycle

```text
DISCOVERED -> MATCHED -> PROBED -> RUNNING -> STOPPING -> REMOVED
```

The exact recovery path depends on whether the driver is kernel-resident or a
user-space service.

## 6. Invariants

1. A device has one authoritative owner at a time.
2. Discovery does not grant access.
3. Resource grants are explicit and revocable.
4. A removed device is not accessible through stale capabilities.
5. Hotplug events do not race device teardown.
6. A driver cannot bind to a device it is not authorized to manage.
7. Device identity is stable across a boot where the platform provides one.

## 7. Current limitations

- No generic device registry.
- No bus model beyond ad hoc PCIe/ACPI probing.
- No driver matching framework.
- No hotplug.
- No resource revocation.
- No device health model.
- No user-space device manager.

## 8. Open questions

- Whether the device manager is kernel-resident or a user-space service.
- How ACPI and Device Tree descriptions are normalized.
- How PCIe functions, bridges and SR-IOV are represented.
- How hotplug and surprise removal are handled.
- How device resources are revoked safely.
- How firmware-provided devices coexist with native drivers.
- What device identity userland sees.

## 9. Test evidence

- `make qemu-disk-test`: one ATA disk with three partitions.
- `make qemu-uefi-disk-test`: AHCI disk with three partitions.
- `make qemu-uefi-test`: firmware Block I/O.
- No hotplug or device-removal test yet.
