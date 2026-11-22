# Finte

[![Build and release](https://github.com/ChidcGithub/Finte/actions/workflows/release.yml/badge.svg)](https://github.com/ChidcGithub/Finte/actions/workflows/release.yml)

**A finite, capability-first system core.** Linux-class responsibilities, a deliberately different shape.

Finte is a from-scratch x86_64/arm64 system core and a small CLI OS. It has its **own BIOS bootloader** and **own UEFI loaders**, runs the same core as a host simulator, and carries a Linux syscall personality on top. It is not a Linux clone: it replaces the parts of a kernel that shape how software is written.

```
        native flows              linux64 persona
             |                         |
             +-------- verbs ----------+
                          |
              caps | flows | channels | arenas
                          |
             fuel scheduler (2 bands)
                          |
                 HAL: host / x86_64 / arm64
```

## Status

- **v0.0.1** (nickname **base**), built as `0.0.1-<short commit>`, 21 self-tests passing on Linux/WSL and Windows.
- **Own boot path**, no GRUB: 512-byte MBR boot sector + CHS stage2 for BIOS, PE32+ UEFI loaders for x86_64 and arm64, and FAT ESPs. `make images` produces x86_64 BIOS/UEFI hybrid media and arm64 UEFI ISO/IMG media.
- **Boots in QEMU** under SeaBIOS and OVMF, and runs a **CLI shell** with VGA text (BIOS) or a GOP framebuffer (UEFI).
- **Startup POST** checks boot protocol, boot info, CPU mode, GDT/IDT, kernel mapping, TSC and heap, then runs **147 core checks** before the shell starts.
- **Disk manager** with MBR/GPT partitions, ATA PIO (BIOS), AHCI (UEFI; firmware Block I/O only if `fw_services`), writable **lfc1 v2**, and read-only **NTFS**/**ext4**. An 8-entry read cache, atomic superblock-last sync, host `lfc1ck.py` validation/compaction, and capability-backed file handles.
- **Shell** with `cd`/`pwd`, `write`, `mkdir`, `rm`, `touch`, `cp`, `mv`, `head`, `tail`, `hexdump`, `tree`, `df`, `sync`, history, arrows, home/end/delete and tab completion, in addition to the diagnostic commands.
- **Timer preemption**: PIT at 100 Hz on the BSP, calibrated LAPIC timers on APs, PIC/LAPIC EOI handling, RFLAGS-aware context switching. Flows run with interrupts enabled; the scheduler and verb dispatcher run with them off. The `preempt` command proves two non-yielding flows make progress.
- **Flow/Thread split**: each Flow now owns an internal `fthread_t` with its own context, stack, CPU affinity, scheduling state and runtime accounting; Fuel, capabilities and task state stay on the Flow. User-visible Thread creation and Parallel Flows are future work.
- **SMP / multi-core scheduling**: a SIPI trampoline starts up to 8 CPUs, each with per-CPU thread run queues and locks, local APIC timers, idle IPIs and a per-CPU GDT/TSS. `finte_sched_migrate()` moves flows between CPUs; work-stealing code exists but is currently disabled (`smp` reports `steals=0`). Per-CPU page/stack caches reduce allocator contention. A global core object lock still serializes verbs while flows compute in parallel. `smp` shows per-CPU ticks, IPIs, switches, steals and migrations.
- **Color console**: the VGA text and GOP framebuffer consoles parse ANSI SGR sequences (`30`–`37`, `90`–`97`, bold) and expose a `color` palette command. Shell prompts, errors and results use color on real hardware as well as in QEMU.
- **Scrollback**: both consoles keep a 512-line text history. `PageUp`/`PageDown`, `Shift+Up/Down`, `Home`/`End` and the PS/2 mouse wheel scroll it; any other key returns to the live prompt. `help` prints a compact multi-column command grid, and `help -v` keeps the description list.
- **Boot logo and visible shutdown**: the shell prints a cyan `Finte` ASCII logo after POST. `shutdown` walks through a staged checklist — shell, idle CPUs, disk flush, timer preemption, interrupts — with short pauses, then powers off.
- **Native CAS**: `cas` parses polynomial expressions with exact rational coefficients and simplifies, differentiates (`cas diff`) or evaluates (`cas eval`) them entirely in the kernel — no external tools, no floating point.
- **Startup performance**: the GOP console draws glyphs with packed 64-bit stores and skips clearing rows that are already blank. With serial disabled, the BIOS path reaches the shell in about 0.8 seconds; UEFI wall time is dominated by OVMF firmware.
- **Isolated user processes**: `user`, `procs` and `services` load ELF64 programs into private 4 KiB page tables at 4 GiB, with the low identity map supervisor-only. Each process has its own code, data and page-scoped arena window; `procs` runs four of them concurrently and demonstrates message passing, shared memory, monotonic time, `sleep`, preemptive round-robin scheduling, page-level arena caps, and process-kill on a kernel-memory fault.
- **3-strike watchdog**: unexpected user faults, scheduler deadlocks and disk-sync failures add a strike. On the third, the kernel prints a red panic and powers off. `selfcheck` and the expected `procs` fault are excluded. `watchdog` shows the state, `watchdog test` injects a strike, and `watchdog reset` clears it.
- **Portable HAL**: the core builds against a platform-neutral `finte/hal.h`. The x86_64 backend keeps VGA/GOP, PIT/LAPIC/TSC, CR3 and ATA/AHCI; an arm64 QEMU `virt` backend uses PL011, the generic counter, PSCI and the same scheduler. `make arm64-test` runs all 147 core checks under `qemu-system-aarch64`.
- **Ring 3 / ELF64**: `user` loads `init.elf` from the initrd as an isolated process, enters CPL3 through a GDT/TSS and `int 0x80` gate, and returns to the shell when the process exits.
- **Boot Protocol v1**: x86 BIOS, x86 UEFI and arm64 UEFI produce one validated structure with kernel/ramdisk, E820 or UEFI memory map, ACPI/DTB, modules and framebuffer. Both UEFI loaders call `ExitBootServices`; the kernel no longer uses firmware Block I/O or ConIn.
- Runs the **same producer/transform/sink pipeline** in the host simulator and on the metal image.
- Runs the **Linux x86_64 persona** in both environments.
- Native Windows host build uses Win32 fibers; Linux uses `ucontext`.

**Physical-hardware status:** verified under QEMU's BIOS and UEFI firmware,
including a 4-CPU SMP boot, USB mass-storage boot media, AHCI after
`ExitBootServices`, a windowed GTK GUI, and PS/2/serial shell input. NVMe and
USB-HID input need native drivers. It has not yet been validated on a specific
physical laptop. Secure Boot must be off (the UEFI loader is unsigned).

## Quick start

On Windows, the kernel build uses WSL because it needs a Linux linker and QEMU:

```powershell
.\build.ps1 -Target all           # host simulator + build/finte.img
.\build.ps1 -Target test
.\build.ps1 -Target selfcheck
.\build.ps1 -Target qemu          # BIOS boot, serial console
.\build.ps1 -Target qemu-uefi-test
```

If you are already in WSL/Linux:

```bash
make all            # host simulator + build/finte.img
make image          # hybrid BIOS + UEFI USB image
make images         # x86_64 + arm64 ISO and IMG release media
make iso            # both ISO images only
make user           # build init.elf + ipc.elf (packed into the initrd)
make test           # 21 extended core self-tests
make selfcheck      # startup POST/self-check suite
make qemu           # BIOS boot, serial console (4 CPUs by default)
make qemu-uefi      # UEFI/OVMF boot, serial console
make qemu-test      # BIOS CLI boot test
make qemu-uefi-test # UEFI CLI boot test
make disk-test      # host disk manager test (lfc1/ext4/NTFS)
make qemu-disk-test      # BIOS + ATA PIO + three filesystems
make qemu-uefi-disk-test # UEFI + AHCI + three filesystems
make demo           # host-side native pipeline
make compat         # host-side Linux persona
make bench          # host microbenchmarks
make arm64          # arm64 QEMU virt kernel (build/finte-arm64.elf)
make arm64-test     # boot arm64 and verify the 147 core checks
make qemu-arm64     # interactive arm64 serial console
python3 tools/lfc1ck.py build/disk-test.img --list
```

Builds inside a git checkout report `0.0.1-<7-char commit>`; without git they
fall back to `0.0.1`. Pushing to `main` runs the build workflow but does not
release. To publish a stable version, tag the current commit:

```bash
sh tools/tag_release.sh 0.0.1
```

The release workflow publishes the x86_64 and arm64 ISO/IMG media plus
`finte.exe`, with notes generated since the previous stable tag.

A nightly workflow runs at **22:00 Asia/Shanghai**. If `main` has advanced since
the last nightly, it publishes a prerelease named
`Nightly-MMDD-<short commit>` with the same artifacts. If nothing was pushed,
the nightly is skipped. It can also be started manually with an optional force
flag.

### Release media

| file | firmware / use |
| --- | --- |
| `build/finte-x86_64.img` | BIOS and UEFI; write to USB with Rufus in DD mode |
| `build/finte-x86_64.iso` | BIOS and UEFI; boot from CD/DVD or attach as a CD-ROM |
| `build/finte-arm64.img` | UEFI; MBR + FAT16 ESP, attach as a disk (QEMU `virt` + AAVMF) |
| `build/finte-arm64.iso` | UEFI; boot as a CD-ROM (QEMU `virt` + AAVMF) |

The arm64 media use a small AArch64 UEFI loader (`BOOTAA64.EFI`) that loads
`KERNEL.ELF` at `0x40080000` and jumps to it. The same kernel is also available
as `build/finte-arm64.elf` for `qemu-system-aarch64 -kernel`.

```bash
cp /usr/share/AAVMF/AAVMF_VARS.fd build/AAVMF_VARS.fd
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 512M -nographic \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,unit=1,file=build/AAVMF_VARS.fd \
  -cdrom build/finte-arm64.iso
```

Native Windows host simulator (MSVC Build Tools + LLVM `clang-cl`):

```powershell
.\build-win.bat
.\build\finte.exe test
.\build\finte.exe selfcheck
.\build\finte.exe demo
```

## Run with a GUI

Use the Windows QEMU launcher to open the OS in a window:

```powershell
.\run-gui.ps1 -Mode uefi    # GOP framebuffer, modern laptop path
.\run-gui.ps1 -Mode uefi -CPUs 4   # 4 virtual CPUs
.\run-gui.ps1 -Mode uefi -Accel whpx  # force WHPX, fall back with -Accel auto
.\run-gui.ps1 -Mode bios    # VGA text, legacy/CSM path
.\run-gui.ps1 -Mode uefi -TestDisk   # attach the lfc1/ext4/NTFS test disk
.\run-gui.ps1 -Mode uefi -FullScreen
```

It rebuilds `build/finte.img` first, stages the image/firmware under
`%LOCALAPPDATA%\FinteGUI` (QEMU for Windows does not handle the non-ASCII
project path), and opens a QEMU window. When available it enables **WHPX**
hardware virtualization automatically, with TCG as a fallback; `-Accel tcg`
forces software emulation. WHPX runs with the **SDL** frontend because QEMU's
GTK frontend can stall before the guest starts on some Windows/WHPX setups;
TCG uses GTK. The launcher also switches the SDL window to the US keyboard
layout so the Windows IME cannot swallow letter keys. Direct PS/2 is the
default for low typing latency. `-UsbKeyboard` now warns and falls back to PS/2
because the UEFI loader exits boot services; USB-HID input needs a native
driver. The guest image and its multi-core behavior are unchanged.
QEMU diagnostics are written to `%LOCALAPPDATA%\FinteGUI\qemu.log` instead of
cluttering the launcher with benign firmware/WHPX warnings.

**Click once inside the QEMU window** to give it keyboard focus, then type at
the `finte>` prompt. `Ctrl+Alt+G` releases the mouse/keyboard grab. `shutdown`
exits; closing the window also stops QEMU.

With `-TestDisk`, a second 64 MiB disk is attached containing lfc1, ext4 and
NTFS partitions. The first is writable:

```text
disks
parts
mount hd1p1 /mnt
cd /mnt
write notes.txt lfc1 is writable
cat notes.txt
sync
umount /mnt
mount hd1p1 /mnt
cat /mnt/notes.txt
```

`hd1p2` (ext4) and `hd1p3` (NTFS) mount read-only.

`make all` produces:

| file | purpose |
| --- | --- |
| `build/finte` | Linux/WSL host simulator |
| `build/finte.exe` | native Windows host simulator (after `build-win.bat`) |
| `build/finte.img` | **hybrid BIOS + UEFI USB image** — write this with Rufus |
| `build/finte-x86_64.img` | architecture-labelled copy of `finte.img` |
| `build/finte-x86_64.iso` | x86_64 BIOS/UEFI hybrid ISO |
| `build/finte-arm64.img` | arm64 UEFI disk image (MBR + FAT16 ESP) |
| `build/finte-arm64.iso` | arm64 UEFI ISO |
| `build/uefi/BOOTX64.EFI` | custom x86_64 UEFI loader |
| `build/uefi/BOOTAA64.EFI` | custom arm64 UEFI loader |
| `build/kernel64.bin` | flat 64-bit kernel (loaded at 64 MiB) |
| `build/finte.elf` | optional Multiboot fallback (`make kernel-elf`) |

## Write it to a USB stick

Use **Rufus** on Windows:

1. Build `build/finte-x86_64.img` with `.\build.ps1 -Target image` (or
   `make images`).
2. In Rufus, select the USB device and `build/finte-x86_64.img`.
3. When Rufus asks between **ISO Image mode** and **DD Image mode**, choose
   **DD Image mode**. The image already contains its own MBR, EFI System
   Partition and FAT32 filesystem; ISO mode would re-partition and destroy it.
4. **Disable Secure Boot** in firmware, or the unsigned UEFI loader will be
   rejected. For a BIOS/CSM machine, enable Legacy/CSM boot.
5. Boot the USB entry. On UEFI you get the GOP framebuffer console; on BIOS/CSM
   you get the VGA text console. Serial is also mirrored on COM1.

**Only target the USB device.** Do not write `finte.img` to your internal
drive. Finte does not install itself; the image is self-contained and runs from
RAM. Mounted lfc1 disks are writable, so back up anything important first.

More detail: [docs/bootloader.md](docs/bootloader.md).

## The idea

A conventional Unix core gives a program a process, an fd table, a uid, and a path namespace. Authority is ambient: if you can name it, you can usually try to open it. Finte makes three changes:

1. **Authority is a capability, not an identity.** A flow can only touch a node if it holds a capability for it, with the required rights. There is no `uid` check in the core, and no global "open by path" operation.
2. **Execution is finite.** Every flow has a fuel budget. The scheduler charges fuel per slice, throttles exhausted flows, and refills them in epochs. "Run forever" is a service pattern, not the default.
3. **Composition is a graph, not a tree.** There is no `fork()` and no `exec()`. The boot image spawns flows from immutable program images and hands out capabilities explicitly. Bulk data moves by transferring a capability to an arena, not by copying buffers through the kernel.

The core has **no path namespace**. Names and directories are a policy of the persona using the core. That is why the Linux personality can provide `/hello.txt` without the core knowing what a path is.

## Startup checks

The kernel refuses to start user work until the core passes its POST:

```text
[    0.108382] [post] boot protocol: ok (custom BIOS loader)
[    0.127570] [post] boot info: ok (magic/ramdisk bounds)
[    0.153926] [post] long mode: ok (PE|PG, PAE, LME|LMA)
[    0.175955] [post] gdt/cs: ok (64-bit CS 0x08)
[    0.195014] [post] idt: ok (32 exception gates)
[    0.212878] [post] kernel mapping: ok (64 MiB entry mapped)
[    0.241061] [post] tsc: ok (monotonic)
[    0.260978] [post] heap: ok (page aligned, writable)
[    0.364755] [post] core self-check
[    0.382299] [post] objects
[    0.395512] #001 ok: ch != FINTE_HANDLE_NONE
[    0.413487] #002 ok: finte_node_type(ch) == FINTE_NODE_CHANNEL
...
[    4.253687] #147 ok: finte_linux_call(g, FINTE_NR_getpid, NULL) == (int64..
[    4.289683] [post] linux persona: ok (19 checks, 0 failures)
[    4.318785] [post] self-check PASS: 6 sections, 147 checks
[    4.365978] [post] startup: PASS (9 POST checks, 147 core checks)
   ___  _         _         
  / __\(_) _ __  | |_   ___ 
 / _\  | || '_ \ | __| / _ \
/ /    | || | | || |_ |  __/
\/     |_||_| |_| \__| \___|

[    4.431100] Finte shell. Type 'help' for commands.
[    4.445840] boot: custom BIOS loader
[    4.466978] Finte (base) 0.0.1
[    4.494982] Welcome to Finte.
```

The **boot logo** is written raw — like the prompt, it carries no timestamp so
the art stays aligned. Every log line around it starts with
`[seconds.microseconds]` measured from kernel console init. The interactive
`finte>` prompt and the command being typed are intentionally left raw, so
history recall, backspace and cursor redraws never rewrite a timestamp. The TSC
is calibrated against PIT channel 2, so the values track real time instead of
assuming a fixed clock. The self-check prints **all 147 checks individually**
(numbered, with long expressions truncated to keep one physical line),
followed by a per-section summary.

The same suite runs from the host with `finte selfcheck`. It covers handles and
generation reuse, capability rights/grants, channel FIFO and cap transfer,
blocking send/receive, deadlock detection, flow wait/join, arena coalescing,
fuel epochs, and the Linux persona's `openat`/`read`/`uname`/`mmap` path.

## The CLI

The kernel boots into `finte>`:

```text
Finte shell. Type 'help' for commands.
boot: UEFI loader
Welcome to Finte.
...
finte> ls
  hello.txt (103 bytes)
  motd.txt (146 bytes)
  readme.txt (265 bytes)
finte> cat hello.txt
Hello from the Finte ramdisk.
finte> demo
[shell] pipeline=OK checksum=650/650 denied=2
finte> procs
...
[procs] ipc=OK isolation=OK shm=OK arena=OK time=OK preempt=OK fault=OK heartbeat=3
finte> shutdown
[shutdown] Finte is going down
  [ ok ] stopped shell input
  [ ok ] parked idle CPUs
  [ ok ] flushed writable filesystems
  [ ok ] stopped timer preemption
  [ ok ] disabled interrupts
[shutdown] power off
```

Commands: `help`, `uname`, `uptime`, `mem`, `boot`, `disks`, `parts`, `mount`,
`umount`, `fsinfo`, `df`, `cd`, `pwd`, `ls`, `cat`, `stat`, `write`, `mkdir`,
`rm`, `touch`, `cp`, `mv`, `head`, `tail`, `hexdump`, `tree`, `sync`, `user`,
`procs`, `services`, `watchdog`, `ps`, `demo`, `preempt`, `smp`, `compat`,
`selfcheck`, `clear`, `color`, `cas`, `echo`, `motd`, `reboot`, `shutdown`,
`halt`. The
ramdisk is loaded by the
bootloader (raw sectors on BIOS, FAT32 file on UEFI), not by a Linux file
system. The disk manager mounts NTFS, ext4 and lfc1 partitions; lfc1 is
writable. See [docs/disk.md](docs/disk.md).

`help` prints the command names in a compact grid; `help -v` adds one-line
descriptions. At the prompt, `PageUp`/`PageDown`, `Shift+Up/Down`,
`Home`/`End` and the mouse wheel browse the last 512 console lines. Any
other key returns to the live prompt.

```text
finte> disks
hd0  65.0 MiB  sector=512  MBR  partitions=1
finte> mount hd0p1 /mnt
mounted hd0p1 on /mnt type=lfc1 label=FINTE_LFC1
finte> cd /mnt
finte> write hello.txt written by Finte
finte> cat hello.txt
written by Finte
finte> umount /mnt
unmounted /mnt
```

`user` loads the ELF64 `init.elf` from the initrd into its own address space
and runs it in ring 3; exiting returns to the shell. `procs` and `services`
start four isolated `ipc.elf` processes: one maps its own arena page, sends a
capability-checked message and then faults on supervisor memory; one maps a
different page at the same virtual address, verifies isolation, receives the
message and checks that the address holds its own private value; one busy-spins;
and one wakes from sleeps to update a shared heartbeat. The command prints a
seven-part result:
IPC, isolation, shared memory, sleep, preemption and fault kill.

`preempt` starts a spinner and a stopper; the spinner never yields, so it can
only finish after the timer preempts it and the stopper sets the stop flag.

## What the demo shows

The `demo` command builds three flows and two channels:

```
producer --raw[4]--> transform --out[4]--> sink
   |                    |                   |
 console[READ]      console[READ]        console[WRITE]
```

- The channels have capacity 4, so the producer blocks and the scheduler interleaves the flows.
- The producer and transformer hold a console capability with **READ only**. Their writes return `EPERM`; the sink's `WRITE` capability succeeds. No ambient authority, no uid check.
- The sink validates every square and prints `checksum=650`.
- The same pipeline runs in the bootable image. `make qemu-test` and
  `make qemu-uefi-test` drive the shell and check the result.

```
[sink] 12^2 = 144
[sink] checksum=650 expected=650
[sink] authority: 2 write attempts denied (EPERM)
[sink] pipeline OK

-- kernel stats --
  syscalls=67 flows=4/4 switches=120
  msgs=24/24 caps=11/11 denied=2 fuel=288 epochs=0
```

The extra switches come from timer preemption and cross-core scheduling while
the flows compute. The
`preempt` command isolates that behavior: one flow spins without yielding, the
other counts to 60 million and then sets the stop flag. Without a timer tick the
spinner would never let the stopper run; with it, both make progress:

```text
finte> preempt
[shell] preempt=OK spins=57748529 stops=60000000 wall=190638 us
```

The pipeline and the `preempt` flows are distributed across online CPUs. `ps`
shows each flow's assigned CPU, and `smp` shows the per-CPU scheduler:

```text
finte> smp
cpus: 4 online
cpu0 hwid=0 ticks=183 ipis=0 switches=25 steals=0 migrations=0 idle
cpu1 hwid=3 ticks=9 ipis=6 switches=10 steals=0 migrations=0 idle
cpu2 hwid=1 ticks=55 ipis=6 switches=58 steals=0 migrations=2 idle
cpu3 hwid=2 ticks=49 ipis=5 switches=51 steals=0 migrations=0 idle
```

## Linux compatibility

The `linux64` persona translates Linux syscall numbers into Finte verbs, then validates the resulting capability operations. It supports a useful subset, including `open`, `openat`, `read`, `write`, `close`, `stat`, `fstat`, `lstat`, `access`, `lseek`, `pread64`, `dup`, `dup2`, `mmap`, `munmap`, `mprotect`, `brk`, `nanosleep`, `sched_yield`, `getpid`, `gettid`, `getppid`, `getcwd`, `uname`, `getrandom`, `clock_gettime`, `gettimeofday`, `getuid`, `getgid`, `sched_getaffinity`, `readv`, `writev`, `exit`, and `exit_group`.

`finte compat` runs a guest that opens and reads `/hello.txt`, calls `uname`, reads the monotonic clock, asks `mmap` for an arena-backed page, writes through the returned pointer, calls `brk`, and gets random bytes. The same flow runs on bare metal.

This is **ABI-shaped compatibility**, not full binary compatibility. The Linux
persona still runs as a kernel flow with a read-only RAMFS. Separately, `user`
runs an isolated ring-3 ELF process and `procs` demonstrates private address
spaces, capability IPC, shared memory and `time`/`sleep` syscalls. There is no
libc/TLS, no signal delivery, and no general Linux binary loader yet. See
[docs/compat.md](docs/compat.md) for the exact table and limits.

## Performance

Measured by `finte bench` on one core (Windows 10 MHz QPC; WSL `CLOCK_MONOTONIC_RAW`). Numbers are indicative, not a Linux-vs-Finte comparison:

| operation | WSL/Linux | native Windows |
| --- | ---: | ---: |
| verb + clock read | 21.9 ns | 20.9 ns |
| capability validate + info | 5.4 ns | 5.1 ns |
| arena alloc + free | 14.3 ns | 14.7 ns |
| channel roundtrip | 734 ns | 223 ns |
| flow spawn + run | 2.0 µs | 10.1 µs |

A channel roundtrip includes two blocking channel operations and the scheduler's context switches. Flow spawn includes host stack/context setup. The bare-metal core is about 142 KiB of flat kernel code and a 32 MiB static heap; it has not been microbenchmarked on metal yet. See [docs/performance.md](docs/performance.md).

Startup work was reduced as well: the GOP console uses packed 64-bit glyph stores and skips clearing blank rows, and the serial UART runs at 115200 baud. With serial disabled, the BIOS path reaches the shell in about **0.8 s**; the kernel itself reaches the shell in roughly **1 s** under UEFI, with most of the displayed UEFI wall time spent in OVMF firmware.

For the Windows GUI, `run-gui.ps1` now enables **WHPX** when the host supports it. On the test machine, the 4-CPU `preempt` benchmark dropped from about **397 ms** under TCG to **135 ms** under WHPX (roughly 3×), and interactive commands are correspondingly faster. TCG remains the automatic fallback.

The performance argument is structural: handles are direct indexes, capability checks are a bounds check and a bitmask, the run queues are O(1), messages are fixed-size and copied into a ring (no allocator), bulk data moves by capability, and there is no address-space copy on spawn.

## Design laws

1. **No ambient authority.** The only way to operate on a node is a capability with the right rights.
2. **Finite by default.** Fuel is charged at dispatch. A flow that exhausts its budget waits for the epoch.
3. **Small messages, large capabilities.** The ABI moves 32 bytes inline and up to 2 capabilities. Anything larger is shared through an arena capability.
4. **Names are persona policy.** The core exposes numbers and capabilities, never paths.
5. **Compatibility is a layer.** The Linux persona is optional; the core does not grow Linux-shaped syscalls.
6. **Fail closed.** Stale handles, missing caps, wrong node types and exhausted budgets return explicit statuses.

## Limits and roadmap

**Works now**

- Own BIOS boot sector/stage2 (CHS) and own x86_64/arm64 UEFI PE loaders.
- Hybrid x86_64 raw USB image with MBR, FAT32 ESP, BIOS payload and UEFI payload.
- x86_64 and arm64 ISO/IMG release media; arm64 boots through AAVMF.
- VGA text console, GOP framebuffer console, PS/2 + serial input. Firmware
  ConIn is unavailable after `ExitBootServices`; USB-HID needs a native driver.
- CLI shell with ramdisk access, writable lfc1 filesystems, line editing,
  history, completion and diagnostic commands.
- Disk manager over ATA PIO and AHCI. Firmware Block I/O remains only for a
  producer that keeps `fw_services`; current loaders exit boot services, so
  NVMe/USB storage needs native drivers,
  with MBR/GPT parsing, an 8-entry read cache, an append-only lfc1 v2 writer,
  host-side `lfc1ck.py` validation/compaction, and capability-backed file
  handles.
- Node table with generation-checked handles and refcounts.
- Capability install/drop/dup/grant and cap transfer in messages.
- Channels with bounded rings, blocking send/receive, backpressure, close/EOF.
- First-fit arenas with splitting and coalescing.
- Fuel scheduler with two priority bands, epochs, sleep/wake, deadlock
  detection, and preemptive per-CPU scheduling.
- SMP bring-up for up to 8 CPUs: SIPI trampoline, per-CPU schedulers, LAPIC
  timers, reschedule IPIs, and a global core lock.
- Flow wait/join with a `CONTROL` capability.
- Ring-3 ELF64 entry, GDT/TSS, per-process page tables, user-mode IPC and
  time syscalls, an `int 0x80` gate, and process-kill on user faults.
- Linux x86_64 persona subset and RAMFS.
- Host simulator on Linux (ucontext) and Windows (fibers).
- Startup POST (9 POST checks) and 147 core checks in the kernel.
- 21 extended self-tests, BIOS/UEFI CLI boot tests, lfc1/ext4/NTFS disk tests,
  writable-lfc1 persistence, timer-preemption, boot protocol v1 and ring-3
  tests.

**Deliberately not here yet**

- **A general process ABI.** Per-process page tables, ring-3 code/data/stack
  isolation, user faults, message passing, a shared page and `time`/`sleep` are
  in place. There is still no `fork`, `exec`, signal delivery, per-process file
  descriptor table, general `mmap`, or user-to-user capability transfer.
- **Per-CPU process teardown.** Per-CPU TSS/GDT is in place and user processes
  can run on APs, but the frame allocator is still a bump allocator, so process
  teardown does not return all pages yet.
- **Fine-grained SMP scalability.** Up to 8 CPUs are started, but flows keep a
  static default affinity, verbs take one global core lock, and work stealing
  is currently disabled. `finte_sched_migrate()` can move flows between CPUs;
  there is no general load balancer or Parallel Flow model yet.
- **Writable NTFS/ext4 and journaling.** Only lfc1 is writable. lfc1 sync is
  copy-on-write and append-only, so old trees consume space until compacted on
  the host; there is no journal or automatic garbage collection.
- **Native NVMe/USB drivers.** x86 UEFI uses native AHCI for SATA. NVMe and USB
  storage still need native drivers; BIOS has ATA PIO only.
- **USB-HID input on a kernel-native stack.** The UEFI loaders exit boot
  services, so firmware ConIn is gone. Serial and PS/2 input work; USB-only
  keyboards need a future HID driver.
- **UEFI ownership.** Both UEFI loaders call `ExitBootServices` and clear the
  firmware pointers. The GOP framebuffer remains valid memory; no firmware
  IDT or Boot Services calls are used after handoff.
- **Secure Boot.** The UEFI loader is unsigned; disable Secure Boot or sign it.
- **Physical-hardware validation.** Verified in QEMU SeaBIOS/OVMF, not yet on a
  specific physical laptop.
- **Full Linux ABI.** No libc/TLS setup, no `fork`, no `clone`, no `futex`, no
  dynamic linker.
- **Arena slice lifetime.** A reserved page slice lasts until the arena is
  destroyed; dropping a scoped cap does not unreserve it.
- **User page permissions.** User code and data are mapped read/write; code is
  not yet read-only or execute-only.

Roadmap order: user-service registry + dynamic linking → native NVMe/xHCI →
journaled lfc1 or writable NTFS/ext4 → per-core scheduler shards →
physical-laptop validation and a signed
UEFI build.

## Repository map

All system sources live under `src/`. The build launchers, `tools/`, `docs/`
and generated `build/` output stay outside it.

```
src/                 all system source
  include/finte/     public ABI: base, HAL, objects, verbs, persona, boot, UEFI, disk, ANSI
  core/              object table, caps, channels, arenas, scheduler, SMP locks, persona, initrd, disk/fs, file handles, CAS, ANSI
  host/              host HAL, CLI, benchmarks, guest program, image block devices
  tests/             core self-tests
  boot/              own BIOS boot sector + CHS stage2
  uefi/              own PE32+ UEFI loader
  arch/x86_64/       long mode, context switch, IDT, timer/APIC, SMP bring-up, VGA, GOP, input, ATA/AHCI/firmware block, user mode, shell
  arch/arm64/        QEMU virt PL011 console, generic counter, PSCI, EL1 contexts and exception vectors, core self-check boot
  user/              init.elf and ipc.elf sources for the isolated ring-3 processes
  ramdisk/           files packed by tools/mkinitrd.py
tools/               image/FAT32/initrd builders, lfc1 builder/checker, QEMU tests
docs/                architecture, ABI, compatibility, performance, building, bootloader, disk, update
Makefile             host + kernel + ring-3 + hybrid image build
build.ps1            WSL driver
build-win.bat        native Windows host build
run-gui.ps1          windowed QEMU launcher for Windows (BIOS/UEFI)
```

## Documentation

- [FNKA contracts](docs/fnka/README.md) — normative Flow/Thread, capability, Fuel, Channel, Arena and kernel contracts.
- [Finte updates](docs/UPDATE.md) — short changelog; GitHub releases hold per-commit notes.
- [Disk manager](docs/disk.md) — block devices, MBR/GPT, NTFS/ext4/lfc1, and the lfc1 format.
- [Architecture](docs/architecture.md) — objects, flows, scheduling, boot path, design trade-offs.
- [ABI](docs/abi.md) — handles, capabilities, verbs, messages, errors.
- [Linux persona](docs/compat.md) — syscall coverage, semantics, limits.
- [Performance](docs/performance.md) — methodology, numbers, what is and isn't measured.
- [Bootloader](docs/bootloader.md) — own BIOS/UEFI boot path, hybrid image, Rufus.
- [Building](docs/building.md) — requirements, targets, QEMU, native Windows.

## License

MIT. See [LICENSE](LICENSE).
