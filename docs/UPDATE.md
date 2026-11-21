# Finte updates

Short notes only. GitHub releases carry the per-commit changes.

## 2026-11-21 — memory isolation
- User copy-in/out walks page tables and checks write rights.
- Page-scoped arena caps reserve slices; `procs` maps one page per service.
- Exited user processes return all pages; heap stays flat across runs.
- Releases run only for pushed tags.

## 2026-11-21 — table-driven modules
- Shell dispatch, completion and help share one command table.
- Syscall, verb, node-release and input key maps are table-driven.
- One ANSI parser serves VGA, GOP and scrollback; host tests now 19.
- Version follows git: `0.0.0-<short commit>`.
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
