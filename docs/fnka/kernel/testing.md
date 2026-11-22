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
| Unit tests | Objects, caps, queues, allocators, boot protocol | `make test` (21 host tests) |
| Kernel self-tests | Controlled kernel environment | 147 checks, 6 sections |
| QEMU boot tests | Startup, shell, exceptions, timers | `make qemu-test`, `make qemu-uefi-test` |
| Integration tests | Userland, IPC, storage, drivers | `make qemu-disk-test`, `procs`, `services` |
| Stress tests | Concurrency, allocation pressure, repeated teardown | partial; no dedicated suite |
| Hardware qualification | Selected physical systems | not started |

## 3. Current commands

```bash
make test                 # 21 host tests
make selfcheck            # 147 core checks on the host
make qemu-test            # BIOS boot and CLI checks
make qemu-uefi-test       # UEFI/OVMF boot and CLI checks
make qemu-disk-test       # BIOS + ATA PIO + lfc1/ext4/NTFS
make qemu-uefi-disk-test  # UEFI + AHCI + filesystems
make arm64-test           # arm64 QEMU virt, 147 core checks
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
| Physical allocator | Partial | Partial | Static heap + page cache |
| Virtual memory | Partial | Missing | x86 paging only |
| Exception handling | Passing | Vectors only | arm64 faults park the CPU |
| Interrupt dispatch | Passing | Missing | No GIC/timer IRQ on arm64 |
| Timer operation | Passing | Counter only | No timer interrupt on arm64 |
| SMP startup | Passing | Missing | x86 up to 8 CPUs |
| Work stealing | Disabled | N/A | `steal_one()` guarded by `&& 0` |
| Flow lifecycle | Passing | Passing | Core checks |
| Thread execution entity | Passing | Passing | Internal one-per-Flow Thread; host split test |
| Capability validation | Passing | Passing | Core checks |
| Channel behavior | Passing | Passing | Core checks |
| Arena mapping | Passing | Partial | x86 user map only |
| User-mode transition | Passing | Missing | x86 ring 3 only |
| ISO/IMG boot | Passing | Passing | Local checks |

## 5. Current coverage gaps

- No arm64 MMU, exception recovery, GIC, timer IRQ, IPI or SMP tests.
- No arm64 user mode or syscall tests.
- No work-stealing stress test; stealing is disabled.
- No multi-Thread or Parallel Flow test.
- No transitive revocation test.
- No user-space driver or IRQ capability test.
- No IOMMU/DMA isolation test.
- No unified boot-protocol conformance test.
- No physical hardware qualification.
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
