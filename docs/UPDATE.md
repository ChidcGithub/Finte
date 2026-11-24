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
- Added file-backed private `mmap`: the kernel reads the fd's bytes into private
  pages, so writes to the mapping never modify the underlying file. The new
  `filemap.elf` checks the mapped contents, a private write and an unchanged
  fd read.
- Extended the signal layer: per-process masks via `sigprocmask`, pending
  signals delivered on unblock, SIGUSR1/2/SIGCHLD handlers, SIGCHLD queued on
  child exit, and default terminate actions for SIGTERM/SIGINT/SIGSEGV. The
  new `sigset.elf` checks masks, two blocked signals, SIGCHLD and the default
  actions.
- Added basic SIGSTOP/SIGCONT job control: a stopped Flow parks as an
  indefinite sleeper, SIGCONT wakes it, and `waitpid` reports stops with
  WUNTRACED and continues with WCONTINUED. Exit status is now POSIX-encoded as
  `(code & 0xFF) << 8`; `job.elf` covers stop, continue and the SIGTERM exit.
- Added process groups: each process has a `pgid`, `setpgid`/`getpgrp` manage
  it, `kill(-pgid)` targets every member, `waitpid(-pgid)` selects children in
  a group, and group SIGSTOP/SIGCONT/SIGTERM drive the whole job. `waitpid`
  now rescans all matching children per event, so one stopped child no longer
  hides another. The new `pgroup.elf` covers group membership, group stop,
  continue, terminate and statuses; the shared ABI line reports `pgroup=OK`.
- Added SIGALRM signal timers: `alarm(2)`, `getitimer(2)` and `setitimer(2)`
  provide one-shot and repeating ITIMER_REAL timers. Expiry is detected in the
  timer IRQ and again on the syscall return path, so delivery does not depend
  on interrupts being unmasked while the scheduler idles. The new `alarm.elf`
  checks remaining-time reporting, cancel, one-shot delivery, a repeating
  200 ms timer, `getitimer` and the default SIGALRM exit status; the shared ABI
  line reports `alarm=OK`.
- Replaced the fixed 16-entry fd array with a dynamic descriptor table (up to
  256 slots) backed by refcounted open-file descriptions. `dup`, `dup2` and
  `fcntl(F_DUPFD)` share descriptions and offsets; `fcntl` also exposes
  `FD_CLOEXEC` and the descriptor flags. Fork clones descriptors onto the same
  descriptions, exec preserves every descriptor except `FD_CLOEXEC`, and close
  drops one reference before the file handle is released. The new `fdtab.elf`
  covers growth past 16 slots, shared offsets, exact and minimum dup targets,
  fork and fork+exec inheritance and close isolation; the shared ABI line
  reports `fdtab=OK`.
- Decoupled file descriptors from capability slots: a descriptor now reads and
  writes through its open-file description's kernel file handle using
  offset-based read/write helpers, so neither `open` nor `dup` spends one of
  the flow's 32 capability slots. `FINTE_MAX_FILES` grows to 256. The ABI
  harness now mounts the lfc1 test disk, and `fdtab.elf` creates and opens 80
  independent disk files (`[fdtab] disk=OK`), beyond both the old 32-slot cap
  limit and the old 64-file table.
- Added ITIMER_VIRTUAL and ITIMER_PROF. The timer IRQ charges 10 ms to the
  current process, with virtual time only when the interrupted context is user
  mode and prof time in both user and kernel. Expiry raises SIGVTALRM/SIGPROF,
  reloads the interval or disarms, and the timer IRQ now delivers pending
  signals immediately so a CPU-bound loop is interrupted without syscalls.
  `cputimer.elf` checks CPU-vs-wall semantics, a pure spin loop, repeated
  intervals and invalid selectors; the ABI line reports `cputime=OK`.
- Added demand paging and swap. `mmap` and `mmap_file` now reserve mappings
  without allocating physical pages; user faults fill anonymous pages with
  zero, load file-backed pages from the open description, or restore a page
  from a swap slot. `mincore` reports residency, `MADV_PAGEOUT` writes resident
  pages to a raw block swap device and invalidates their PTEs, and swapped
  pages are reference-counted so fork children can share the slot. `swapon`
  uses a dedicated raw block device; the QEMU tests attach `build/swap.img` as
  `hd1`. `vm.elf` checks lazy residency, 16-page pageout/fault-in with intact
  patterns, private file mappings across pageout and swapoff; the ABI line
  reports `vm=OK`.
- Added the `hwtest` shell command and
  `docs/hardware-validation.md`. `hwtest` runs the 147 core checks, the
  four-process demo, `init.elf` and a CPU/disk/mount report, then prints
  `[hw] PASS`; the CLI boot test now executes it. The actual physical-laptop
  run remains a manual step and is recorded as pending in the results table.
- Recorded a VMware Workstation UEFI run from `Finte-UEFI.vmx`: `[hw] PASS`
  with the 147 checks, procs demo and `init.elf` at 2 vCPUs. The bare-metal
  x86_64 row remains pending.
- Physical x86_64 `hwtest` passed on real hardware, completing the x86_64
  hardware-validation row. Arm64 physical boot still needs DTB/ACPI platform
  discovery.
- Added a polled x86_64 NVMe driver: PCI class `01:08:02` discovery, admin
  and I/O queues, Identify Controller/Namespace, 4 KiB PRP transfers and HAL
  block registration. QEMU UEFI mounts lfc1 from an NVMe namespace and verifies
  file read and write (`make nvme-test`); CI installs OVMF and runs it.
- Added a polled xHCI controller, boot HID keyboard and bulk-only USB mass
  storage. USB storage registers through `hal_blk_*`; `usb-kbd` input is polled
  by the shell. The QEMU test types `uname` through the emulated keyboard and
  mounts/reads/writes lfc1 over `usb-storage` (`make xhci-test`, CI). Current
  scope is the USB2 port set, boot keyboard reports and BOT/SCSI; hubs and
  SuperSpeed companion descriptors are not handled.
- Added an lfc1 write-ahead superblock journal. lfc1 reserves a four-sector tail
  region with two 1 KiB records; sync writes tree/data, commits a checksummed
  record with the new superblock, then updates sector 0. Mount replays the
  newest valid record if it is ahead of the superblock and ignores stale or
  torn records. `make journal-test` simulates a crash between commit and
  superblock write, verifies replay, then corrupts the record and verifies the
  old tree is used.
- NVMe details: UEFI only, namespace 1, 512-byte logical blocks, one polled I/O
  queue and 4 KiB PRP transfers. The QEMU test and CI run mount, read, write and
  sync lfc1 over the NVMe namespace.
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
