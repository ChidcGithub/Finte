# Testing contract

> **Status:** Partial — the core suite runs on both architectures and the x86
> boot/disk paths are covered. arm64 lacks interrupt, SMP, MMU and user-mode
> tests.
>
> **Normative for:** what a support claim means, which tests define it, and
> which architecture/feature combinations are currently covered.
>
> **Related:** [README](../README.md), [arm64](../architecture/arm64.md),
> [x86_64](../architecture/x86_64.md), [Scheduler](scheduler.md).

## 1. Rule

A feature is supported only to the extent demonstrated by a repeatable test on
a named platform.

- “Builds” is not “boots”.
- “Boots” is not “passes the core suite”.
- “Passes the core suite” is not “supports the hardware”.
- “Runs under QEMU `virt`” is not “supports AArch64 hardware”.
- A test passing on x86_64 is not evidence that arm64 supports the same
  feature.

## 2. Test levels

| Level | Purpose | Current examples |
| --- | --- | --- |
| Compile-time | Types, warnings, architecture builds | `make all`, `make images`, `build-win.bat` |
| Unit tests | Objects, caps, queues, allocators, boot protocol, PMM, address spaces | `make test` (24 host tests) |
| Kernel self-tests | Controlled kernel environment | 147 checks, 6 sections |
| QEMU boot tests | Startup, shell, exceptions, timers | `make qemu-test`, `make qemu-uefi-test` |
| Integration tests | Userland, IPC, storage, drivers | `make qemu-disk-test`, `procs`, `services` |
| Stress tests | Concurrency, allocation pressure, repeated teardown | PMM churn/fragmentation host test; no SMP allocator stress suite |
| Hardware qualification | Selected physical systems | not started |

## 3. Current commands

```bash
make test                 # 24 host tests
make selfcheck            # 147 core checks on the host
make qemu-test            # BIOS boot and CLI checks
make qemu-uefi-test       # UEFI/OVMF boot and CLI checks
make qemu-disk-test       # BIOS + ATA PIO + lfc1/ext4/NTFS
make qemu-uefi-disk-test  # UEFI + AHCI + filesystems
make abi-test             # x86_64 1-CPU fd/mmap/filemmap/cow-fork/waitpid/exec/signal/sigset/job/pgroup/alarm/cputime/fdtab/vm/cap/churn
make nvme-test            # x86_64 UEFI + polled NVMe + lfc1 mount/read/write
make xhci-test            # x86_64 UEFI + USB HID keyboard + USB BOT storage
make arm64-test           # arm64 4 CPUs: MMU/timer/SMP + EL0 user/procs/ABI + core checks
make arm64-uefi-test      # same arm64 platform checks under AAVMF
make arm64-disk-test      # arm64 virtio-mmio legacy + modern lfc1/ext4/NTFS
make arm64-uefi-disk-test # arm64 UEFI virtio disk boot under AAVMF
make images               # x86_64 and arm64 IMG/ISO release media
```

Local image validation also covers:

- x86_64 ISO boot under SeaBIOS;
- x86_64 ISO boot under OVMF;
- arm64 ISO boot under AAVMF;
- arm64 IMG boot under AAVMF.

These image boot checks are currently manual/local, not part of the default CI
matrix.

## 4. Cross-architecture matrix

| Area | x86_64 | arm64 | Notes |
| --- | --- | --- | --- |
| Build and link | Required / passing | Required / passing | CI builds both |
| Boot to kernel entry | Passing | Passing | BIOS/UEFI; `-kernel` and AAVMF |
| Boot protocol v1 | Passing | Passing | E820/UEFI map, ACPI/DTB, modules, validator |
| Core self-tests | Passing (147) | Passing (147) | Same core suite |
| Physical allocator | Passing | Passing | Shared PMM from boot map; static-heap fallback |
| Virtual memory | Partial | Partial | Shared process core; per-process roots, NX/RO, anonymous mmap on both |
| Exception handling | Passing | Passing | arm64 user faults, IRQ and SVC return; EL1 sync panics |
| Interrupt dispatch | Passing | Passing | x86 IDT/APIC; arm64 GICv2 |
| Timer operation | Passing | Passing | x86 PIT/LAPIC; arm64 PPI 30 at 100 Hz |
| SMP startup | Passing | Passing | x86 and arm64 up to 8 CPUs; 4-CPU tests |
| Work stealing | Disabled | N/A | `steal_one()` guarded by `&& 0` |
| Flow lifecycle | Passing | Passing | Core checks |
| Thread execution entity | Passing | Passing | Internal one-per-Flow Thread; host split test |
| Capability validation | Passing | Passing | Core checks |
| Channel behavior | Passing | Passing | Core checks |
| Arena mapping | Passing | Passing | Page-scoped arena caps on both user demos |
| User-mode transition | Passing | Passing | x86 ring 3 `int 0x80`; arm64 EL0 `svc #0` |
| Process ABI | Passing (1 CPU) | Passing (4 CPUs) | dynamic fd table with dup/dup2/fcntl and shared open-file descriptions, demand-paged mmap, file-backed mmap, mincore, MADV_PAGEOUT, raw-block swap, COW fork, waitpid, exec, signal masks/SIGUSR1/2/SIGCHLD/defaults, SIGSTOP/SIGCONT job control, process groups via setpgid/getpgrp and group kill/waitpid, SIGALRM/SIGVTALRM/SIGPROF timers, cap transfer, service registry, ET_DYN load, churn, blocked abort |
| Block storage | Passing | Passing | x86 ATA/AHCI + polled NVMe (UEFI) + USB BOT; arm64 virtio-mmio legacy + modern |
| ISO/IMG boot | Passing | Passing | Local checks and CI arm64 targets |
| Hardware validation | Passing (bare metal + VMware UEFI) | Pending | x86_64 `hwtest`; arm64 needs ACPI/DTB/GICv3 |

## 5. Current coverage gaps

- No arm64 GICv3, ACPI/DTB or physical-hardware test. x86_64 bare metal and
  VMware UEFI `hwtest` pass.
- No fd-table stress test beyond 256 descriptors or across concurrent closes;
  the 80-file disk test covers the former capability-slot limit.
- x86_64 process ABI runs on one CPU; arm64 covers 4 CPUs. Simultaneous
  fork/sleep/migration is not stress-tested.
- No virtio IRQ or queue-stress test; the arm64 block driver is polled.
- No work-stealing stress test; stealing is disabled.
- No multi-Thread or Parallel Flow test.
- No transitive revocation test.
- No user-space driver or IRQ capability test.
- No IOMMU/DMA isolation test.
- No unified boot-protocol conformance test.
- x86_64 physical and VMware UEFI validation pass; arm64 physical
  qualification remains.
- No long-running stress or leak test.

## 6. What a new feature needs

Before a feature is labeled **Implemented**:

1. A host or core unit test for the state machine and error paths.
2. A QEMU boot or integration test where the feature affects boot, interrupts,
   storage or userland.
3. An arm64 test if the feature claims cross-architecture support.
4. A negative test for the main denial or failure path.
5. A documented limitation if coverage is partial.

Before a release:

1. Run the host tests and core self-check.
2. Run the BIOS and UEFI CLI tests.
3. Run the disk tests.
4. Build and boot the arm64 kernel.
5. Build the release images and boot each image at least once.
6. Record the exact platforms and QEMU/firmware versions used.

## 7. Reporting

Test output should state:

- the architecture;
- the platform (QEMU machine, firmware);
- the test count and failures;
- any known skipped coverage.

An example claim is:

> “arm64 QEMU `virt` boots through AAVMF and passes the 147 core checks.”

It is not:

> “Finte supports arm64.”

## 8. Open questions

- Which image boot checks should become mandatory CI jobs?
- How to run OVMF/AAVMF tests in CI reliably.
- How to add arm64 MMU/GIC/SMP tests without a second test harness.
- What stress duration and allocation pressure are meaningful.
- How to test hardware without risking unrecoverable boards.
- How to publish a machine-readable support matrix.
