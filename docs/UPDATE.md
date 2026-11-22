# Finte updates

Short notes only. GitHub releases carry the per-commit changes.

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
