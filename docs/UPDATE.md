# Finte updates

Short notes only. GitHub releases carry the per-commit changes.

## 2026-11-22 — shared process layer
- Moved ELF loading, user copy, syscall dispatch, fault policy, fd tables,
  mmap, fork, exec, signals and the user demos into `src/core/process.c`.
- x86_64 and arm64 now keep only page-table, root-switch, trap-frame and
  user-entry backends.
- Address spaces are refcounted and Flow-owned; flow death releases page tables
  even when `proc_destroy()` is omitted.
- Added arm64 ASID allocation/invalidation and RX/RW-NX user mappings on both
  architectures.
- Added the x86_64 `abi-test` and extended the arm64 boot tests with the shared
  process ABI suite, including a capability service registry and fixed-address
  ET_DYN shared-object loading.
- Added blocking `waitpid` with child status and reaping. The first activation
  of a user Flow now installs its address-space root before the switch, so
  `hal_ctx_switch` binds the TSS stack and CR3 on the first entry instead of
  leaving the flow on the previous stack.
- Fork is copy-on-write: owned pages are shared read-only, a per-page registry
  counts mapping spaces, and the first write copies (or upgrades the last owner
  in place). Write faults are resolved in the x86 page-fault return path and
  the arm64 data-abort path; kernel user-copy paths COW before writing.
- Fixed the CI boot by pinning x86 builds to `-march=x86-64`; the runner's
  compiler emitted BMI2 `shlx`, which the `qemu64` guest rejects. The ABI
  harness boots the BIOS floppy image and reports boot milestones on COM1.
- Host suite is now 24 tests, including address-space refcount/lifetime and
  RX/RW-NX checks.

## 2026-11-22 — 0.0.2
- Version bumped to 0.0.2.
- Shared PMM now backs flow stacks and process page tables, with a host
  churn/fragmentation test and stable-free teardown checks.
- arm64 EL0 user mode: per-process L0/L1/L2/L3 tables, TTBR0 switching,
  `svc #0` syscalls, user-fault kill, embedded initrd, `init.elf` and the
  four-process `procs` demo.
- arm64 virtio-mmio block driver (legacy v1 and modern v2) with direct and
  AAVMF lfc1/ext4/NTFS tests.
- CTR_EL0-based arm64 D-cache clean/I-cache invalidation for MMU tables and AP
  startup; the UEFI loader uses the same line sizes.
- Fixed x86 flow-stack PMM accounting, shell file-buffer frees and the POST
  probe page leak.
- Host suite is now 23 tests; arm64 user mode and disk checks run in CI.

## 2026-11-22 — Boot Protocol v1
- One validated `fboot_info` structure for x86 BIOS, x86 UEFI and arm64 UEFI.
- BIOS E820 memory map and ramdisk module; UEFI memory map, ACPI/DTB, modules
  and GOP framebuffer.
- Both UEFI loaders call `ExitBootServices`; the kernel no longer uses firmware
  Block I/O, ConIn or the firmware IDT.
- Host boot-protocol validation test; suite is now 21 tests.
- x86 UEFI input is serial/PS/2; USB HID needs a native driver.

## 2026-11-22 — first-stage Flow/Thread split
- Added `fthread_t`; each Flow owns one primary Thread.
- Scheduler queues, sleep/throttle lists, channel wait lists and migration now
  operate on Threads.
- Fuel, capabilities, Channels, Arenas and persona remain on the Flow.
- Added the `flow/thread split` host test; suite is now 20 tests.
- User-visible Threads and Parallel Flows remain deferred.

## 2026-11-22 — FNKA contract set
- Added `docs/fnka/`: Flow, Thread, Capability, Fuel, Channel, Arena, kernel,
  driver, architecture and security contracts.
- Each contract has an explicit Implemented/Partial/Proposed/Open status.
- Documented current gaps: Flow/Thread merge, disabled work stealing, x86 UEFI
  firmware dependency, arm64 missing MMU/GIC/SMP/user mode.

## 2026-11-21 — ISO/IMG release media
- `make images` builds x86_64 and arm64 `.iso` and `.img` files.
- x86_64 images are BIOS/UEFI hybrid; arm64 images are UEFI/MBR/FAT16.
- x86_64 ISO is BIOS + UEFI; arm64 ISO/IMG boot through AAVMF.
- Added a minimal AArch64 UEFI loader (`BOOTAA64.EFI`).
- Releases and nightlies now publish all four images plus `finte.exe`.

## 2026-11-21 — nightly prereleases
- Nightly run at 22:00 Asia/Shanghai when `main` has new commits.
- Version `Nightly-MMDD-<commit>`; published as a prerelease with images and exe.

## 2026-11-21 — 0.0.1
- Version bumped to 0.0.1.

## 2026-11-21 — per-CPU scheduling
- Per-CPU run-queue locks, loops, work stealing and migration API.
- Per-CPU GDT/TSS lets user flows run on APs.
- Per-CPU page/stack caches and node/flow allocation hints.
- Global core object lock remains for the next stage.

## 2026-11-21 — portable HAL and arm64
- HAL is platform-neutral: console, time, IRQ, contexts, pages, block I/O.
- Added QEMU `virt` arm64 backend and `make arm64-test`.
- arm64 runs the same 147 core checks; Linux persona remains x86_64.

## 2026-11-21 — memory isolation
- User copy-in/out walks page tables and checks write rights.
- Page-scoped arena caps reserve slices; `procs` maps one page per service.
- Exited user processes return all pages; heap stays flat across runs.
- Releases run only for pushed tags.

## 2026-11-21 — table-driven modules
- Shell dispatch, completion and help share one command table.
- Syscall, verb, node-release and input key maps are table-driven.
- One ANSI parser serves VGA, GOP and scrollback; host tests now 19.
- Version follows git: `0.0.x-<short commit>`.
- Tag-triggered image and Windows build releases.

## 2026-11-21 — 3-strike watchdog
- Panic and power off after three abnormal events.

## 2026-11-21 — user processes
- Private address spaces, IPC/shared memory, time and timer preemption.

## 2026-11-21 — console polish
- Boot logo, visible shutdown stages, compact help and scrollback.

## 2026-11-21 — color and CAS
- ANSI color consoles and exact-rational polynomial CAS.

## 2026-11-17 — SMP and performance
- Up to 8 CPUs plus startup and I/O fast paths.

## 2026-11-17 — filesystems and ring 3
- Writable lfc1, NTFS/ext4 reads, timer preemption, firmware NVMe/USB.

## 2026-11-16 — base
- Own BIOS and UEFI boot, 147 core checks, shell and Linux syscall persona.
