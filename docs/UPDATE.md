# Finte is alive

**Release notes for v0.0.0 “base” — 16 November 2026**

**Post-release update — 17 November 2026: disk manager, NTFS/ext4/lfc1**

**Second post-release update — 17 November 2026: writable lfc1, ring 3,
preemption, firmware I/O**

The boundaries in the first update have moved. Finte now has all of the
following in the same hybrid image:

- **Writable lfc1 v2.** Mount loads the tree into RAM. `create`, `write`,
  `truncate`, `mkdir`, `unlink`, `rename` and `sync` update it. Sync appends a
  complete new tree and writes the superblock last, so an interrupted sync
  leaves the previous tree valid. The host checker `tools/lfc1ck.py` validates
  every directory, extent, checksum and `next_free` cursor, and `--compact`
  reclaims old trees.
- **Read cache.** Eight 4 KiB read-through entries, invalidated on write. It
  keeps directory and extent reads cheap without caching file data twice.
- **Capability file handles.** Disk files are nodes with READ/WRITE rights.
  `FINTE_VERB_FILE_READ` and `FINTE_VERB_FILE_WRITE` enforce the cap; the shell
  opens, installs, uses and drops real handles instead of bypassing authority.
- **A richer shell.** `cd`, `pwd`, `write`, `mkdir`, `rm`, `touch`, `cp`, `mv`,
  `head`, `tail`, `hexdump`, `tree`, `df` and `sync`, plus history, arrow keys,
  home/end/delete and tab completion. Every mutating command syncs.
- **Timer preemption.** The PIT runs at 100 Hz, the PIC is remapped so IRQ0 is
  vector `0x20`, and `kx_switch` saves RFLAGS. Flows run with IF=1; the
  scheduler and the verb dispatcher run with IF=0. `preempt` starts a spinner
  that never yields and a stopper that sets its stop flag, proving progress
  without cooperative yields on both BIOS and UEFI.
- **Ring 3 and ELF64.** `user` loads `init.elf` from the initrd, builds a user
  page table with U/S set below 64 MiB, installs a GDT/TSS, enters CPL3 and
  prints `Hello from ring 3 via int 0x80` through an `int 0x80` gate.
- **Firmware NVMe and USB HID.** OVMF exposes NVMe and USB storage through
  Block I/O, which is appended as `hdN` when it is not a duplicate of a native
  ATA/AHCI disk. Firmware ConIn supplies USB keyboard input. Firmware calls
  temporarily switch to the firmware IDT with interrupts enabled and restore
  the caller's flags; the kernel IDT selector follows the live code segment, so
  OVMF's `CS=0x38` and BIOS's `CS=0x08` both work.

```text
finte> mount hd1 /mnt
mounted hd1 on /mnt type=lfc1 label=NVME_LFC1
finte> write /mnt/nvme-timer.txt nvme persisted with timer
finte> sync
synced
finte> umount /mnt
unmounted /mnt
finte> mount hd1 /mnt
mounted hd1 on /mnt type=lfc1 label=NVME_LFC1
finte> cat /mnt/nvme-timer.txt
nvme persisted with timer

finte> preempt
[shell] preempt=OK spins=61525760 stops=60000000 wall=194658 us
```

**Verified after this update**

| Check | Result |
| --- | --- |
| Host tests, Linux/WSL and native Windows | **16 / 16** |
| Startup POST + core self-check | **9 + 147 / 147** |
| BIOS and UEFI CLI boot tests | pass |
| lfc1 create/write/mkdir/cp/sync/remount | pass on BIOS and UEFI |
| `lfc1ck.py` validation and compaction | pass |
| Ring 3 `user` (BIOS and UEFI) | pass |
| `preempt` (BIOS and UEFI) | **OK**, both flows finish |
| NVMe read/write/sync/remount over firmware Block I/O | pass |
| USB mass-storage read/write/sync over firmware Block I/O | pass |
| USB HID keyboard over firmware ConIn | pass, no duplicate keys |
| Physical laptop | still pending |

Current artifacts:

| Artifact | Size |
| --- | --- |
| `build/kernel64.bin` | **122,048 bytes** |
| `build/finte.img` | **68,157,440 bytes** (65 MiB) |
| `build/initrd.bin` | **5,740 bytes** |
| `build/init.elf` | **5,088 bytes** |

**Third post-release update — 17 November 2026: SMP and startup performance**

Finte now starts application processors and schedules native flows across them:

- **SMP bring-up.** An INIT/SIPI trampoline at physical `0x8000` starts up to
  eight CPUs. Each AP gets a logical slot, GS-based per-CPU state, the BSP's
  GDT/IDT and a kernel stack. The BSP relocates the live GDT into kernel memory
  before the trampoline overwrites low memory.
- **Per-CPU scheduling.** Each CPU has two run-queue bands and its own current
  flow, scheduler context, tick and switch counters. A global scheduler lock
  protects state transitions and a `queued` flag prevents double insertion when
  a wake races with a context switch.
- **Global core lock.** Verbs and object-table mutations are serialized across
  CPUs. Blocking verbs release the lock around the context switch and
  reacquire it on wake. Flows still compute in parallel between verbs.
- **AP timers and IPIs.** APs use TSC-deadline LAPIC timers where available and
  a calibrated periodic fallback otherwise. Reschedule IPIs are sent only to
  CPUs parked in HLT, so a running CPU is not interrupted just to look at its
  queue.
- **Input routing.** The UEFI loader inspects the ConIn device path and records
  whether it is USB. USB keyboards use firmware ConIn; PS/2-only systems use
  direct i8042 polling. This removes the duplicate keys and dropped keys that
  appeared when both paths were polled.
- **Faster startup.** GOP glyphs are drawn with packed 64-bit stores, blank rows
  are not cleared, and serial runs at 115200 baud. With serial disabled, BIOS
  reaches the shell in about **0.8 s**. UEFI's wall time is dominated by OVMF,
  not the kernel.
- **Runtime tuning.** Timer preemption is skipped when a CPU has no competing
  flow; reschedule IPIs are sent only to CPUs with queued work; the lfc1 writer
  tracks dirty state and avoids appending a tree when nothing changed; the
  block cache grew to 32 entries; and shell input polling backs off to 100 µs
  instead of hammering firmware ConIn.
- **Hardware acceleration.** `run-gui.ps1` detects WHPX automatically and falls
  back to TCG. On the test machine the 4-CPU `preempt` benchmark fell from
  about **397 ms** under TCG to **135 ms** under WHPX.
- **Reliable GUI input.** WHPX uses the SDL frontend, and the launcher switches
  its input language to en-US so the Windows IME cannot consume letter keys.
  Direct PS/2 is the default for low typing latency; `-UsbKeyboard` opts into
  the firmware ConIn path.

```text
finte> smp
cpus: 4 online
cpu0 apic=0 ticks=183 ipis=0 switches=25 idle
cpu1 apic=3 ticks=9 ipis=6 switches=10 idle
cpu2 apic=1 ticks=55 ipis=6 switches=58 idle
cpu3 apic=2 ticks=49 ipis=5 switches=51 idle
```

**Verified after this update**

| Check | Result |
| --- | --- |
| 4-CPU BIOS and UEFI boot | pass |
| SMP core self-check | **147 / 147**, 0 failures |
| `demo`, `preempt`, `compat` with 4 CPUs | pass |
| `preempt` with AP LAPIC timers | AP ticks/switches advance |
| Ring 3 `user` with 4 CPUs | pass on BIOS and UEFI |
| UEFI lfc1 write/sync/remount with 4 CPUs | pass |
| NVMe firmware Block I/O with 4 CPUs | pass |
| USB HID and PS/2 input with 4 CPUs | pass, no duplicates or lost keys |
| lfc1 read-only mount leaves the image unchanged | pass |
| WHPX UEFI 4-CPU boot, disk write/read, keyboard | pass |
| WHPX 4-CPU `preempt` | ~135 ms (TCG ~397 ms) |
| Windows host build and 16 tests | pass |
| BIOS shell start, serial disabled | ~0.8 s |
| Physical laptop | still pending |

**Fourth post-release update — 21 November 2026: color console and native CAS**

The shell is no longer monochrome, and it can do algebra without leaving the
metal:

- **ANSI color console.** Both console drivers parse SGR escape sequences.
  The VGA text path maps ANSI codes to the correct VGA palette indices
  (`red` is VGA 4, not VGA 1), and the GOP framebuffer path renders the 16
  standard colors directly. Bold (`1`), reset (`0`), default foreground
  (`39`) and background (`40`–`49`) are handled. The parser keeps partial
  escape state across `write` calls, so split sequences still work.
- **Colored shell.** The prompt is cyan, errors are red, success messages and
  CAS results are bold green, and `color` prints the palette. `SYNC` failure
  output and `selfcheck` results are colored too.
- **Native polynomial CAS.** A compact C core (`core/cas.c`) parses
  expressions with integers, `x`, `+ - * /`, `^` (non-negative integer
  exponents) and parentheses. Coefficients are exact 64-bit rationals, with
  overflow-checked GCD-reduced arithmetic and a degree limit of 32. Results
  are simplified polynomials.
  - `cas <expr>` simplifies: `cas (x+1)*(x-1)` → `x^2 - 1`.
  - `cas diff <expr>` differentiates: `cas diff x^3+2*x` → `3*x^2 + 2`.
  - `cas eval <expr> <x>` evaluates exactly: `cas eval x^2+1 3` → `10`.
  - Division is by constant polynomials only; polynomial division reports an
    error instead of silently truncating.
  - Parenthesis nesting is limited to 8 and the boot stack grew to 32 KiB, so
    a pathological expression cannot smash the stack through parser recursion.
- **Host parity.** The host binary exposes the same CAS core as
  `finte cas ...`, so expressions can be checked outside QEMU.

```text
finte> color
ANSI colors
black  red  green  yellow  blue  magenta  cyan  white
gray  bright red  bright green  bright yellow  bright blue  bright magenta  bright cyan  bright white
finte> cas (x+1)*(x-1)
x^2 - 1
finte> cas diff x^3+2*x
3*x^2 + 2
finte> cas eval x^2+1 3
10
```

**Verified after this update**

| Check | Result |
| --- | --- |
| Host tests | **16 / 16** |
| Startup POST + core self-check | **147 / 147**, 0 failures |
| VGA text palette (`color`) | 15 non-black colors, correct ANSI indices |
| GOP framebuffer palette (`color`) | 1280×800, 15 non-black colors |
| Kernel `cas` simplify/diff/eval over serial | pass |
| Host `finte cas` simplify/diff/eval | pass |
| BIOS and UEFI boot with color + CAS | pass |
| Physical laptop | still pending |

**Fifth post-release update — 21 November 2026: compact help and scrollback**

The shell now makes long output and command discovery usable on the local
console:

- **Compact help.** `help` prints the 43 command names in a column-major grid
  sized to the console width (four columns on 80 columns, more on a wide GOP
  display). `help -v`, `help --verbose` and `help long` keep the one-line
  descriptions. The list is alphabetical and includes `version`, which was
  previously undiscoverable.
- **512-line history.** Both VGA text and the GOP framebuffer shadow their
  output into a character grid with ANSI colors. Scrolling repaints an
  absolute window from the ring buffer, so boot and `selfcheck` output stays
  readable after it leaves the screen. While scrolled, the bottom row becomes
  a status bar (`scroll +N`, key hints) and the VGA cursor is hidden.
- **Keyboard scrolling.** `PageUp`/`PageDown` move by a page, `Shift+Up/Down`
  by one line, and `Home`/`End` jump to the oldest and live ends. These work
  while scrolled and from the live prompt. `Up`/`Down` still recall command
  history at the live prompt.
- **Mouse wheel.** The kernel initializes the PS/2 auxiliary port, negotiates
  IntelliMouse 4-byte packets with the 200/100/80 sample-rate sequence, and
  polls the wheel alongside the keyboard. `input_trywheel()` normalizes the
  PS/2 packet sign (QEMU and Linux treat a negative packet as wheel-up) into
  positive notches, and the shell scrolls three lines per notch. The setup is
  opt-in by capability: if no mouse answers, the keyboard path is unchanged.
- **Safe input mixing.** Auxiliary bytes are routed to the mouse parser and
  never fed to the keyboard keymap; a keyboard byte at the head of the
  controller queue is left for `input_trykey()`.
- **Typing while scrolled.** Any key other than a scroll key snaps back to
  live first, repaints the prompt and line, then acts normally. `clear` resets
  the history and the view.

```text
finte> help
commands:  help -v for descriptions
  boot              echo              mv                sync
  ...
  disks             mount             stat

finte> selfcheck
...
 scroll +24  PgUp/PgDn  Shift+Up/Down  wheel  any key returns
```

**Verified after this update**

| Check | Result |
| --- | --- |
| Compact `help` and verbose `help -v` | pass, 43 commands |
| VGA PageUp/PageDown/Shift+Up/Down/Home/End | pass |
| Typing and `clear` while scrolled | returns live and runs normally |
| PS/2 wheel up/down, VGA and UEFI (QMP/VNC) | pass after sign normalization |
| Mouse motion does not inject characters | pass |
| GOP scroll repaint and status bar | 1280×800, cyan status row |
| Kernel self-check | **147 / 147**, repeated boots |
| Host tests | **16 / 16** |
| Physical laptop | still pending |

**Sixth post-release update — 21 November 2026: boot logo and visible shutdown**

Boot and power-off now say what they are doing:

- **Boot logo.** After POST, the shell prints the requested `Finte` ASCII art
  in bold cyan. The logo is written raw, like the prompt, so timestamp prefixes
  do not push the art out of alignment; the normal log lines around it keep
  their per-line timestamps. `Finte (base) 0.0.0` still precedes
  `Welcome to Finte.`
- **Visible shutdown.** `shutdown` no longer prints `bye.` and exits in one
  step. It shows a staged checklist:

```text
[shutdown] Finte is going down
  [ ok ] stopped shell input
  [ ok ] parked idle CPUs
  [ ok ] flushed writable filesystems
  [ ok ] stopped timer preemption
  [ ok ] disabled interrupts
[shutdown] power off
```

  Each step pauses for 120–150 ms so it is readable, and a sync failure prints
  a red `[fail]` line instead of silently continuing. The sequence then calls
  the existing `hal_exit(0)` power-off path.
- **Art and history.** The logo is fed to the console scrollback like any other
  output, so it remains visible after scrolling back. `reboot` re-runs the
  normal boot path and prints the logo again.

**Verified after this update**

| Check | Result |
| --- | --- |
| BIOS and UEFI boot banner | pass, raw cyan art |
| `Finte (base) 0.0.0` before `Welcome to Finte.` | pass |
| Staged `shutdown` on BIOS and UEFI | all steps print, QEMU exits |
| Kernel self-check | **147 / 147** |
| Host tests | **16 / 16** |
| Physical laptop | still pending |

**Seventh post-release update — 21 November 2026: processes, isolation, IPC and time**

The four basic kernel requirements are now implemented in the native user
path, not just in the kernel simulator:

- **Preemptive round-robin scheduling.** User processes are flows in the same
  per-CPU scheduler as kernel flows. The timer tick now expires sleepers even
  while a CPU-bound task never yields, so a sleeping task can preempt a
  spinner. Four isolated processes run on the BSP; kernel flows still use all
  CPUs.
- **Per-process address spaces.** Every process gets a private PML4. The low
  4 GiB identity map is present only for CPL0; user code lives at 4 GiB with
  private code/data/stack pages, and a deliberate read of supervisor memory
  raises #PF. The fault handler prints the PID/RIP/CR2 and kills only that
  process. `user` now runs `init.elf` through the same loader and returns to
  the shell instead of halting.
- **IPC and shared memory.** Capability channels accept `ipc_send` and
  `ipc_recv` from ring 3, blocking through the scheduler. The kernel also maps
  one shared page into the demo processes so they can exchange data directly.
- **Monotonic time and timers.** `time` returns nanoseconds from the calibrated
  TSC; `sleep` blocks a flow until an absolute deadline; `yield` round-robins.
  The timer IRQ drives sleeper wakeups, so timeouts and periodic tasks work
  while another process is CPU-bound.

```text
finte> procs
starting isolated user processes...
[pid 1] ipc send=0x... secret=0x1111111122222222
[pid 1] probing kernel memory at 0x4000000
[user] pid 1 killed: exception 14 rip=0x... cr2=0x4000000
[pid 2] ipc=OK isolation=OK shm=OK sleep=OK elapsed=0x...ns
[pid 4] heartbeat=0x3
[pid 3] spin=0x... heartbeat=0x3
[procs] ipc=OK isolation=OK shm=OK time=OK preempt=OK fault=OK heartbeat=3
```

The shared page is intentionally shared; all other pages are private. The
frame allocator is still a bump allocator, and one TSS means user processes
stay on the BSP for now.

**Verified after this update**

| Check | Result |
| --- | --- |
| `procs` on BIOS, UEFI and `-smp 1` | all six checks OK |
| Two consecutive `procs` runs in one boot | pass |
| Same VA in two processes holds private values | pass |
| Kernel-memory read from ring 3 | #PF kills only that process |
| IPC send/recv through a channel capability | pass |
| Shared-page write/read across processes | pass |
| `sleep` elapsed time | ~50–59 ms for a 50 ms request |
| Spinner heartbeat with a CPU-bound flow | 3 wakeups |
| `user` (isolated init.elf) | prints and returns to the shell |
| Kernel self-check | **147 / 147** |
| Host tests | **16 / 16** |
| Physical laptop | still pending |

Current artifacts: `build/kernel64.bin` **145,568 bytes**, `build/initrd.bin`
**12,816 bytes** (`init.elf` 5,088, `ipc.elf` 7,040), image unchanged at
**68,157,440 bytes**.

**Eighth post-release update — 21 November 2026: 3-strike watchdog**

Abnormal events no longer disappear into the log. A small system watchdog keeps
one counter and tolerates two events before shutting the machine down:

- **Sources.** Unexpected ring-3 faults, scheduler deadlocks and disk-sync
  failures each add a strike. A user-fault strike records PID and vector
  (`user fault pid 1 vec 14`); a deadlock records `scheduler deadlock`.
- **Expected events are excluded.** `selfcheck` suspends the watchdog around its
  intentional deadlock test, and the `procs` PID 1 kernel-memory probe is marked
  as an expected fault. The demo can therefore run repeatedly without tripping
  the counter.
- **Panic and shutdown.** The third strike prints:

```text
[watchdog] abnormal 3/3: manual test
[watchdog] PANIC after 3 abnormal events: manual test
[watchdog] shutdown
```

  The panic path stops the local timer, disables interrupts, waits briefly so
  the console and serial output are visible, and calls `hal_exit(0x70)`.
- **Control.** `watchdog` prints `N/3 strikes` and the last reason;
  `watchdog test` injects one manual strike (three calls exercise the panic);
  `watchdog reset` clears the counter.

**Verified after this update**

| Check | Result |
| --- | --- |
| Three `watchdog test` calls | panic printed, QEMU powered off |
| Strikes and last reason display | pass |
| `watchdog reset` | returns to 0/3 |
| `selfcheck` | 0 strikes, **147 / 147** |
| `procs` expected fault | 0 strikes, all six checks OK |
| Unexpected user fault (temporary check) | 1 strike with PID/vector |
| Host tests | **16 / 16** |
| Physical laptop | still pending |


**Ninth post-release update — 21 November 2026: table-driven modules**

The growing command surface had started to duplicate itself. The shell kept
three separate command lists (dispatch, completion and help), and the three
console consumers each carried a copy of the ANSI parser. Those copies are now
single modules:

- **One shell command table.** Each row pairs a name, description and handler;
  `run_line`, tab completion, `help` and `help -v` all read the same table.
  Adding a command is one row, and dispatch no longer walks a 50-branch
  `strcmp` chain. Commands receive a common `sh_ctx_t` instead of a different
  parameter list per command.
- **One ANSI parser.** `src/core/ansi.c` owns the incremental SGR state
  machine and the 16-color palettes. VGA, GOP and the scrollback grid feed bytes
  through it, so escape handling and color semantics cannot drift between
  backends.
- **One host CLI table.** The native `finte` binary uses the same pattern for
  dispatch and usage text; `--help` and `-h` resolve to the `help` row.
- **Small reuse helpers.** `FINTE_ARRAY_SIZE` replaces repeated
  `sizeof(a)/sizeof(a[0])`, and flow-state names come from a table guarded by a
  compile-time assertion.

**Verified after this update**

| Check | Result |
| --- | --- |
| Clean kernel/image rebuild | pass, kernel **144,832 bytes** |
| Host tests | **17 / 17** |
| Startup self-check | **147 / 147** |
| BIOS and UEFI CLI boot tests | pass |
| BIOS disk test | pass |
| `procs` IPC/isolation/time/preempt/fault | all OK, **0 strikes** |
| Shell help, verbose help, completion and arguments | pass |
| Native Windows build | **17 / 17** |


```text
  ______ _       _
 |  ____(_)     | |
 | |__   _ _ __ | |_ ___
 |  __| | | '_ \| __/ _ \
 | |    | | | | | ||  __/
 |_|    |_|_| |_|\__\___|

              it boots.
```

Finte is no longer a design document.

It is a bootable x86_64 system: its **own BIOS bootloader**, its **own UEFI
loader**, its **own 64-bit kernel**, its **own shell**, and a **Linux syscall
personality** — packed into one hybrid image.

No GRUB. No Linux kernel underneath. No libc on the metal.

## The moment

The complete path works from firmware to prompt:

```text
firmware -> Finte bootloader -> long mode -> POST -> self-check -> shell
```

On a BIOS/CSM machine, a 512-byte MBR and a CHS stage2 loader pull the kernel
from the USB image, build a 1 GiB identity map with 2 MiB pages, and enter long
mode. On a UEFI machine, the custom PE32+ `BOOTX64.EFI` reads the FAT32 ESP,
finds GOP, places the kernel at 64 MiB, and jumps to it.

Then the kernel introduces itself:

```text
[    4.365978] [post] startup: PASS (9 POST checks, 147 core checks)
[    4.405831] core: nodes=512 flows=64 channels=64 caps/flow=32
[    4.431100] Finte shell. Type 'help' for commands.
[    4.445840] boot: custom BIOS loader
[    4.466978] Finte (base) 0.0.0
[    4.494982] Welcome to Finte.
[    4.535691]
[    4.556271] Finte is a capability-first system core with finite flows.
[    4.588133] Try: help uname ls cat demo compat selfcheck boot
[    4.620690] finte>
```

That is the whole milestone in one screenful: firmware handed over, the kernel
checked itself, and a person can type at a prompt.

## Milestone checklist

- [x] Boot from a custom MBR under SeaBIOS
- [x] Boot from a custom PE32+ loader under OVMF
- [x] Reach 64-bit long mode with a 1 GiB identity map
- [x] Load a ramdisk and start a CLI shell
- [x] Drive VGA text and a GOP framebuffer console
- [x] Accept PS/2 keyboard and serial input
- [x] Boot from a QEMU USB mass-storage attachment
- [x] Open a windowed QEMU GUI and accept typing
- [x] Run 16 host tests, 9 POST checks, and 147 core checks
- [ ] Validate on a specific physical laptop (next)

## What is new in v0.0.0 “base”

### A name, a version, and a real greeting

- Version is **0.0.0**; nickname is **base**.
- The shell prints `Finte (base) 0.0.0` immediately before the motd line
  `Welcome to Finte.`
- Version and nickname live in `include/finte/base.h`, so changing them changes
  the boot banner, `uname`, and the Linux persona release string together.

### Time on every output line

Every output line starts with `[seconds.microseconds]` since kernel console
init. The interactive prompt and the text being edited are deliberately raw:
they carry no timestamp, so history recall and cursor redraws cannot erase or
duplicate one.

```text
[    0.108382] [post] boot protocol: ok (custom BIOS loader)
[    0.127570] [post] boot info: ok (magic/ramdisk bounds)
```

The timestamp is derived from the TSC, calibrated against PIT channel 2 over
about 10 ms. QEMU reports roughly **2.996 GHz**, and the value tracks wall time
instead of assuming a fixed clock. The same prefix is emitted to VGA, the GOP
framebuffer, and COM1.

### A self-check you can actually read

The startup suite now prints **all 147 checks individually**, numbered and
truncated to fit one 80-column line:

```text
[    0.395512] #001 ok: ch != FINTE_HANDLE_NONE
[    0.413487] #002 ok: finte_node_type(ch) == FINTE_NODE_CHANNEL
...
[    4.253687] #147 ok: finte_linux_call(g, FINTE_NR_getpid, NULL) == (int64..
[    4.289683] [post] linux persona: ok (19 checks, 0 failures)
[    4.318785] [post] self-check PASS: 6 sections, 147 checks
```

Each of the six sections still ends with its own summary, so a failure can be
located by section and by check number without reading a wall of text.

### A framebuffer that scrolls straight

Two rendering bugs were fixed in the GOP console:

- Row recycling cleared only one pixel scanline, leaving glyph fragments behind.
- A circular row window displayed lines in rotated order because the hardware
  always scans top-to-bottom.

The console now clears the full 18-pixel text row (16-pixel glyph plus leading)
and scrolls linearly with 64-bit moves. A full 147-line `selfcheck` scroll
renders in order with monotonic timestamps and no stale pixels.

### A GUI that listens

`run-gui.ps1` opens Finte in a QEMU GTK window in either mode. A subtle input
bug is fixed: with `-serial none`, an absent COM1 read back as `0xFF`, which
looked like “serial data ready forever” and starved the PS/2 keyboard. The
launcher now rebuilds the image, stages it under `%LOCALAPPDATA%\FinteGUI`
because QEMU for Windows cannot open the non-ASCII project path, and the window
accepts typing after one click.

### A disk manager, three filesystems, one small format

Finte can now enumerate real block devices and read partitions:

- **BIOS**: ATA PIO, up to four drives, LBA28/LBA48.
- **UEFI**: AHCI DMA over PCI SATA, up to eight devices.
- **Partition tables**: MBR (including extended chains) and GPT, with labels.
- **Filesystems**: read-only NTFS and ext4, plus the native **lfc1** format,
  now writable in v2.
- **Shell**: `disks`, `parts`, `mount`, `umount`, `fsinfo`, and absolute paths
  in `ls`, `cat` and `stat`.

```text
finte> disks
hd0  65.0 MiB  sector=512  MBR  partitions=1
hd1  64.0 MiB  sector=512  MBR  partitions=3
finte> mount hd1p2 /mnt
mounted hd1p2 on /mnt type=ext4 label=FINTE_EXT4
finte> cat /mnt/hello.txt
hello from ext4
```

**lfc1** is Finte's own read-optimized format: a 512-byte superblock, sorted
16-byte directory entries, one contiguous extent per file, no inode table,
journal or allocation bitmap. Version 2 makes it writable through a
copy-on-write append: sync writes a fresh tree and switches the superblock last.
It is built with `tools/mklfc1.py`, validated/compacted with
`tools/lfc1ck.py`, and documented in [disk.md](disk.md).

NTFS and ext4 remain read-only. Their unsupported features (NTFS attribute
lists, ext4 journal recovery) are rejected cleanly. Storage is reached through
ATA PIO on BIOS and AHCI plus firmware Block I/O on UEFI; NVMe and USB storage
work through the firmware path.

## Verified

| Check | Result |
| --- | --- |
| Host tests (Linux/WSL and native Windows) | **16 / 16** |
| Startup POST checks | **9 / 9** |
| Core self-check | **147 / 147**, 0 failures |
| BIOS CLI boot test (SeaBIOS) | pass |
| UEFI CLI boot test (OVMF) | pass |
| QEMU USB mass-storage attachment | pass |
| QEMU GTK GUI, BIOS and UEFI | pass |
| Windowed keyboard input with `-serial none` | pass |
| Host disk manager: lfc1, ext4, NTFS, nested dirs, checksums | pass |
| BIOS disk manager: ATA PIO + MBR + three filesystems | pass |
| UEFI disk manager: AHCI + MBR + three filesystems | pass |
| GPT partition parsing and reads | pass |
| Physical laptop | pending |

Artifacts at the first disk milestone (superseded by the current table above):

| Artifact | Size |
| --- | --- |
| `build/kernel64.bin` | **83,472 bytes** |
| `build/finte.img` | **68,157,440 bytes** (65 MiB) |
| `build/initrd.bin` | **616 bytes** |

## Run it

Build the hybrid image and open the modern path:

```powershell
.\run-gui.ps1 -Mode uefi
```

Legacy path:

```powershell
.\run-gui.ps1 -Mode bios
```

Inside the window, click once to focus, then try:

```text
uname
selfcheck
uptime
demo
preempt
compat
ls
cat hello.txt
mount hd1p1 /mnt
cd /mnt
write notes.txt hello from the USB image
cat notes.txt
sync
shutdown
```

`user` runs the isolated ring-3 ELF demo and returns to the shell. `procs`
runs four processes that exercise IPC, shared memory, monotonic `sleep`,
preemptive scheduling and fault isolation.

`Ctrl+Alt+G` releases the keyboard grab. Closing the window also stops QEMU.

## Put it on a USB stick

1. Build `build/finte.img`.
2. In Rufus, select the USB device and the image.
3. Choose **DD Image mode** — not ISO mode.
4. Disable Secure Boot; the UEFI loader is unsigned.
5. Boot the USB entry. Do not write the image to an internal drive.

The image is self-contained and runs from RAM. Finte does not install itself.
Mounted lfc1 disks are writable; NTFS/ext4 remain read-only. See
[building.md](building.md) and [bootloader.md](bootloader.md) for the full
procedure.

## Limits, honestly

- Ring 3 now uses per-process page tables: private code/data/stack pages at
  4 GiB, a supervisor-only low identity map, validated user copies, user-fault
  process kill, capability IPC, shared memory and `time`/`sleep` syscalls.
  There is still no `fork`/`exec`, signal delivery, per-process file table,
  general `mmap` or user-to-user capability transfer. User processes share one
  TSS and therefore run on the BSP; the Linux persona still runs in the kernel
  address space. Process teardown does not free its bump-allocated frames yet.
- Only lfc1 is writable. Its sync is append-only, so old trees consume space
  until `tools/lfc1ck.py --compact` runs on the host. NTFS and ext4 are
  read-only; there is no journal replay.
- NVMe and USB storage/input go through UEFI firmware services, not native
  kernel drivers. The loader does not call `ExitBootServices`. BIOS has ATA PIO
  only.
- The UEFI loader is unsigned, so Secure Boot must be off.
- Physical-laptop validation is still pending; QEMU with NVMe, USB HID, USB
  mass storage and a windowed GUI is the current proof.

## Next

1. `fork`/`exec`, signals, per-process file descriptors, general `mmap`, and
   user-to-user capability transfer.
2. Per-CPU TSS/GDT so user processes can run on every CPU, then a program
   registry and dynamic linking.
3. Native NVMe/xHCI drivers, or a deliberate firmware-lifetime model.
4. Journaled lfc1 or writable NTFS/ext4.
5. Physical-hardware validation and a signed UEFI build.

## Raise a glass

Finte is alive.

Not because it draws a window, but because it found its own way there: 512
bytes of MBR, a stage2 loader, a FAT32 ESP, a PE32+ loader, a kernel at 64 MiB,
and a prompt that waits for a human to type.

It prints the time before every output line because time is finite.
It prints all 147 checks because trust should be earned at boot.
It says `Finte (base) 0.0.0` before `Welcome to Finte.` because it finally has
a name to introduce.

To the boot sector that did not crash.
To the checks that all passed.
To the keyboard that finally answered.

**Finte is alive.**
