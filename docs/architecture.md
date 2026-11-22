# Finte architecture

For normative mechanism contracts, see the [FNKA contract set](fnka/README.md).

Finte is a system core, not a complete operating system. It owns the parts that decide **who may do what**, **what executes**, **how work is scheduled**, and **how data and authority move**. It currently ships x86_64 and arm64 ports, a host simulator port, one Linux personality, a RAMFS, and bootable BIOS/UEFI media for both architectures.

The design is optimized for two things:

- **Predictable cost**: short, bounded paths for authority checks, message passing and scheduling.
- **Composability without ambient authority**: programs are wired together with explicit capabilities.

## 1. Objects and handles

Every kernel object is registered in a flat node table:

```c
typedef struct fnode {
    uint16_t type;      // console, channel, arena, flow, file, dir
    uint16_t flags;
    uint32_t gen;       // generation
    uint32_t refs;
    uint32_t index;     // pool slot
    uint32_t aux;
} fnode_t;
```

A handle is:

```
(gen << 32) | (slot + 1)
```

- `0` is never a valid handle.
- Lookup is **O(1)**: bounds check, type check, generation check.
- When a node slot is reused, `gen` increments. Stale handles fail instead of aliasing a new object.
- Nodes hold reference counts. Caps, queued messages, directory entries and the scheduler hold references; the last reference frees the node slot and its pooled object.

There is no global `kmalloc` on the hot path. Nodes, flows, channels, arenas, files and directories come from fixed pools sized in `src/include/finte/base.h`. Arenas get page-granular backing memory from the HAL. This keeps object operations bounded and makes the memory envelope visible at build time.

Limits (defaults):

| object | count |
| --- | ---: |
| nodes | 512 |
| flows | 64 |
| threads | 64 |
| channels | 64 |
| arenas | 16 |
| files | 64 |
| dirs | 8 |
| caps per flow | 32 |
| channel capacity | 32 messages |
| message payload | 32 bytes + 2 caps |

## 2. Capabilities

A capability is a per-flow table entry:

```c
typedef struct fcap {
    fhandle_t handle;
    uint32_t rights;
    uint32_t flags;
} fcap_t;
```

Rights:

| bit | meaning | checked by |
| --- | --- | --- |
| `READ` | read a file, inspect an object | `FILE_READ`, dir lookup |
| `WRITE` | mutate an arena, write a console | `ARENA_ALLOC/FREE`, `CONSOLE_WRITE` |
| `SEND` | send on a channel | `CHAN_SEND` |
| `RECV` | receive on a channel | `CHAN_RECV` |
| `EXEC` | reserved for executable images | — |
| `GRANT` | grant this capability to another flow | `CAP_GRANT` |
| `CONTROL` | wait for a flow, close a channel (and future lifecycle control) | `FLOW_WAIT`, `CHAN_CLOSE` |
| `SIGNAL` | reserved for asynchronous notification | — |

A flow starts with **no caps**. The boot image installs exactly the caps a flow needs. A capability can be:

- installed (`finte_cap_install`),
- duplicated with a subset of rights (`CAP_DUP`),
- dropped (`CAP_DROP`),
- granted to another flow that the caller has a `GRANT` cap for (`CAP_GRANT`),
- attached to a channel message (up to 2).

No operation in the core consults a uid, gid, path or global permission table. If a flow lacks a capability, it cannot name the object at all.

### Capability transfer semantics

Sending a message with an attached cap **duplicates the reference**:

- The sender keeps its cap.
- The channel queue holds one reference.
- The receiver gets a new local cap index with the same rights.

The receiver cannot use an index from the sender; indices are local. If the receiver's cap table is full, the receive returns `FINTE_ERR_NOMEM` and the message stays queued. This is deliberate: there is no silent authority loss.

## 3. Flows

A Flow owns task state; its primary Thread owns CPU execution state:

```c
typedef struct fflow {
    uint8_t state;          // mirrors the primary Thread in v0.1
    uint8_t persona;        // NATIVE or LINUX
    uint32_t fuel;          // credits remaining
    uint32_t fuel_max;
    fcap_t caps[32];
    fthread_t *thread;
    ...
} fflow_t;

typedef struct fthread {
    uint8_t state;          // CREATED, READY, RUNNING, BLOCKED, SLEEPING, THROTTLED, DEAD
    uint32_t cpu;           // affinity
    uint32_t priority;
    uint32_t quantum;       // charged per dispatch
    fhal_ctx_t *ctx;
    ...
} fthread_t;
```

Each Flow has exactly one primary Thread in the serial-only stage. The Thread
owns the context, stack, CPU affinity, scheduler links and blocking state. The
Flow owns capabilities, Fuel, persona and task state.

Flows are created from an in-image program:

```c
typedef struct fprogram {
    const char *name;
    void (*entry)(fflow_t *flow);
    uint32_t default_fuel;
} fprogram_t;
```

There is **no fork** and **no exec**:

- Spawning is `finte_flow_create` + `finte_cap_install` + `finte_flow_start`.
- The program image is immutable and linked into the image.
- There is no address-space copy, no copy-on-write tree, no inherited fd table.
- Authority is installed explicitly, not inherited from a parent.

This is the biggest usage difference from Unix. A Unix process is born with a uid, a cwd, a umask, signal dispositions and an fd table. A Finte flow is born with exactly the caps installed into its table and a fuel budget.

Flow lifecycle:

```
FREE -> READY -> RUNNING -> READY/BLOCKED/SLEEPING/THROTTLED -> ... -> DEAD
```

On exit, the flow drops every capability, returns to the scheduler, and the scheduler releases its ownership reference. If other caps still reference the flow, the flow node remains as a dead object until the last cap is dropped; this keeps handles safe.

A flow that holds a `CONTROL` cap for another flow can wait for it to finish
(`FLOW_WAIT`). Waiters are kept on the target's `waiters` list; exit wakes them
before the dead flow's capabilities are dropped. This is the join primitive a
supervisor uses without `waitpid`, process trees or signals.

## 4. Channels and messages

A message is fixed-size:

```c
typedef struct fmsg {
    uint32_t tag;
    uint16_t len;          // <= 32
    uint8_t cap_count;     // <= 2
    uint8_t flags;
    uint16_t caps[2];      // local cap indices
    uint32_t reserved;
    uint8_t data[32];
} fmsg_t;                  // 48 bytes
```

Channels are bounded rings of these messages. Sending never allocates. Large data is not copied through a channel; the sender shares an **arena capability** and sends offsets or fixed headers.

Backpressure is explicit:

- `CHAN_SEND` blocks when the ring is full, unless `FINTE_MSG_NONBLOCK` is set.
- `CHAN_RECV` blocks when the ring is empty.
- A send or receive wakes one waiter.
- `fx_send`/`fx_recv` are blocking wrappers; `fx_try_send`/`fx_try_recv` return `FINTE_ERR_AGAIN`.

A holder of a `CONTROL` capability can close the channel. Closing wakes every
waiter; queued messages remain readable, then receive returns `FINTE_ERR_EOF`
and send returns `FINTE_ERR_STATE`. This gives pipelines an explicit
end-of-stream instead of a deadlock or an out-of-band sentinel.

If every runnable flow is blocked and no flow is sleeping, the scheduler reports
`FINTE_ERR_DEADLOCK` and records the statistic rather than spinning forever.

## 5. Arenas

An arena is a page-backed region with a first-fit allocator:

```
[block header | payload][block header | payload]...
```

- Payloads are 16-byte aligned.
- Allocation splits a free block when the remainder is useful.
- Free coalesces adjacent free blocks.
- `ARENA_ALLOC` returns an **offset**, not a pointer. The owner computes `arena->base + offset`; another flow with an arena cap can do the same.
- A cap may be scoped to a page range. Reserving the range splits the block list so a slice cannot overlap a neighbor, and scoped `ARENA_ALLOC` only uses blocks fully inside it.
- A scoped user service maps its page at `FPROC_ARENA_VA`; the mapped payload begins after the block header.
- `ARENA_FREE` is explicit and scoped caps cannot free outside their range. There is no compaction and no moving GC.
- `mmap` in the Linux persona allocates from an arena and returns a host/kernel pointer directly.

The arena is the intended bulk-data path: a producer allocates, writes, sends the arena cap plus offsets, and the consumer reads in place. This is why the channel message is only 48 bytes.

## 6. Scheduler

The scheduler is a **two-band, O(1) FIFO** over ready Threads:

- band 0: latency-sensitive
- band 1: batch
- within a band, round-robin FIFO
- run queues are intrusive linked lists (`next_run`)
- blocked Threads live on channel wait lists
- sleepers live on a separate list
- Threads with exhausted Flow Fuel live on a throttle list

Each dispatch charges `FINTE_FUEL_QUANTUM` (24) from the owning Flow's `fuel`.
If the Thread yields after its last credit, it is throttled. When the run queues
are empty and only throttled Threads remain, the scheduler runs an **epoch**: it
refills every throttled Flow to `fuel_max`, increments `epochs`, and re-enqueues
their Threads. A Flow cannot monopolize the core by consuming infinite CPU; it
can only consume its budget and then wait for the next epoch.

The scheduler is **timer-preemptive and SMP-aware**:

- The BSP keeps PIT channel 0 at 100 Hz (PIC IRQ0 -> vector `0x20`). APs use
  calibrated local APIC timers on the same vector, with TSC-deadline mode when
  available and a periodic fallback otherwise.
- APs are started through an INIT/SIPI trampoline at physical `0x8000`. Each AP
  loads the BSP's GDT/IDT (relocated to kernel memory first), claims a logical
  CPU slot, installs its own GS base, builds its own GDT/TSS lazily on first
  Thread switch, and enters `finte_sched_ap_run()`.
- Each CPU owns a two-band Thread run queue and a **per-CPU run-queue lock**. A
  small state lock covers sleep/throttle lists and blocked/ready transitions,
  not queue pops. A `queued` flag prevents double insertion.
- `steal_one()` implements work stealing without taking the state lock, but
  the scheduler currently disables it with `if (!t && 0)`. `finte_sched_migrate()`
  still moves a Flow's primary Thread between CPUs and changes affinity for
  running/blocked/sleeping Threads; `smp` reports the steal and migration
  counters per CPU.
- Per-CPU page and 32 KiB stack caches absorb allocator traffic before the
  global free lists; node and Flow allocation scan from per-CPU hints.
- The timer gate calls `finte_sched_preempt()`, which marks the running Thread
  READY and switches to that CPU's scheduler stack. `kx_switch` saves RFLAGS as
  well as the callee-saved registers. Threads run with IF=1; schedulers keep
  IF=0.
- `finte_syscall` takes a global **core lock** and disables interrupts. The lock
  serializes object, capability, channel, arena, disk and console operations
  across CPUs. Blocking verbs release it around the context switch and
  reacquire it when woken; a timer cannot preempt a Thread in the middle of a
  verb. Splitting this object-table lock is the next scheduling stage.
- Reschedule IPIs are sent only to CPUs parked in HLT. Batch start broadcasts a
  wakeup; an idle AP sleeps until an IPI or its local timer. AP timers are
  masked while no Thread is running.
- The host HAL implements the interrupt and SMP hooks as no-ops, so host tests
  keep deterministic single-threaded behavior.

The `preempt` shell command proves preemption: a spinner never yields, a second
flow counts to 60 million and sets the stop flag, and both finish only because
timer ticks interleave them. Under `-smp 4`, `smp` shows all CPUs and their
per-CPU tick/IPI/switch counters; `ps` shows each Thread's assigned CPU.

Sleeping Threads use `hal_now_ns`. The timer IRQ calls `finte_sched_tick()`,
which expires sleepers even while a CPU-bound Thread never yields; this is what
lets a sleeping task preempt a user spinner. The scheduler loop still checks
the earliest wake time when the CPU is idle. User processes can run on APs; the
per-CPU TSS/GDT binds the user stack on each switch. Firmware calls temporarily
switch to the firmware IDT with interrupts enabled, then restore the caller's
IDT and RFLAGS. The BSP's LAPIC timer state is left to OVMF so firmware ConIn
and Block I/O keep working after APs are online.

Statistics are kept globally and per flow: syscalls, context switches, messages, caps installed/dropped/denied, fuel burned, epochs, deadlocks.

### System watchdog

`src/arch/x86_64/watchdog.c` keeps a three-strike abnormality counter. Unexpected
user-mode faults, scheduler deadlocks and disk-sync failures each add a strike
and print `[watchdog] abnormal N/3`. The third strike prints a red panic and
powers off (`hal_exit`). The self-check suite suspends the watchdog because it
deliberately provokes a deadlock, and the `procs` PID 1 fault is marked
expected because it is the isolation test. `watchdog` prints the counter,
`watchdog test` injects a manual strike, and `watchdog reset` clears it.

## 7. Verb ABI

All operations go through one dispatcher:

```c
fret_t finte_syscall(fflow_t *flow, uint32_t verb, const uint64_t args[6]);
```

`fret_t` separates status from value:

```c
typedef struct fret {
    int32_t status;   // 0 or a negative FINTE_ERR_*
    uint32_t detail;
    uint64_t value;
} fret_t;
```

Unlike the `errno` convention, a successful call cannot be confused with an error
and a value can be 64-bit without sign ambiguity. The verb set is intentionally
small: 23 verbs. See [abi.md](abi.md). Verbs dispatch through a direct-indexed
handler table; the Linux personality uses a sorted syscall table and binary
search.

Native flows call the dispatcher directly. The ring-3 demo uses a separate
`int 0x80` gate with a minimal `write`/`exit`/`getpid` ABI in
`src/arch/x86_64/usermode.c`; general verbs do not yet cross the trap, and user
pointers are only range-checked against the 64 MiB user limit. A production
user ABI needs per-flow address spaces and copy-in/copy-out validation.

## 8. Linux persona

The Linux personality is a layer above the verb dispatcher:

```
Linux syscall number + args
        |
finte_linux_call
        |
fd -> cap table, path -> dir lookup, mmap -> arena
        |
finte_syscall (verbs)
        |
capability check
```

It provides:

- fd 0/1/2 mapped to a console cap
- read-only flat RAMFS with `openat`, `read`, `lseek`, `fstat`, `newfstatat`
- `mmap` from an arena, `brk`, `munmap`, `mprotect`
- `uname`, `clock_gettime`, `gettimeofday`, `nanosleep`
- `getpid`, `gettid`, `getuid`, `getgid`, `getrandom`
- `readv`, `writev`, `rt_sigaction`, `fcntl`, `arch_prctl`
- `exit`, `exit_group`

The core does not know what a path is. The persona owns the directory object and resolves names. This keeps naming policy out of the trusted core and makes alternative namespaces possible (a graph service registry, a capability-only UI, etc.).

See [compat.md](compat.md) for supported numbers, return conventions and limitations.

## 9. Boot path

Finte does not use GRUB. One hybrid raw image contains two independent boot paths.

### BIOS/CSM

1. **MBR boot sector** (`src/boot/boot.S`, 512 bytes)
   - reads CHS geometry with INT 13h AH=08h
   - loads 32 sectors of stage2 to `0x0000:0x8000` one sector at a time
   - CHS is used instead of EDD LBA for maximum BIOS USB compatibility
2. **Stage2** (`src/boot/stage2.S`)
   - reads the image header at LBA 33
   - loads the kernel to `0x10000` and the ramdisk to `0x40000`
   - copies the kernel to **64 MiB**
   - builds PML4 → PDPT → PD, identity mapping 1 GiB with **8-byte** 2 MiB entries
   - enables PAE, EFER.LME, CR0.PG, far-jumps to `0x4000000`
   - passes `fboot_info*` in RDI and `0xF17E0001` in RSI
3. **UEFI path** (`src/uefi/loader.c`)
   - PE32+ EFI application on a FAT32 EFI System Partition
   - opens `EFI/FINTE/KERNEL.BIN` and `RAMDISK.BIN` via the firmware file protocol
   - allocates 36 MiB for code+bss+heap at 64 MiB
   - fills `fboot_info` with the ramdisk, UEFI table pointers, GOP framebuffer,
     the firmware IDT descriptor, and the firmware Block I/O device list
   - jumps to the flat kernel; it does **not** call `ExitBootServices`

### Kernel

- `src/arch/x86_64/entry64.S` clears `.bss` (the 32 MiB heap) and calls `kmain`.
- IDT with 32 exception stubs, a timer gate on `0x20`, a reschedule IPI gate,
  and an `int 0x80` gate;
  a fault prints vector and CR2. Gate selectors follow the live `CS`, so the
  same code works with BIOS's `0x08` and OVMF's `0x38`.
- SMP bring-up copies an INIT/SIPI trampoline to `0x8000`, starts up to 8 APs,
  gives each a per-CPU run queue, GS base, IDT/GDT and LAPIC timer, and parks
  idle CPUs in HLT until an IPI arrives.
- startup POST: boot protocol, boot info, long mode, GDT/CS, IDT, 64 MiB mapping,
  TSC, heap, then the shared core suite (**9 POST checks, 147 core checks**).
- Console selection:
  - BIOS: 80x25 VGA text + COM1.
  - UEFI: GOP linear framebuffer + COM1. Console output never calls firmware.
  Both drivers feed the same incremental ANSI parser in `src/core/ansi.c`
  (`0`, `1`, `22`, `30`–`37`, `39`, `40`–`47`, `49`, `90`–`97`), so a sequence
  split across writes still applies and neither backend can drift. VGA maps
  ANSI codes to the classic palette indices; GOP uses the shared 16-color RGB
  table.
- The bootloader loads a read-only ramdisk archive; the shell uses it for
  `ls`/`cat`/`stat` and for `init.elf`.
- The shell runs on the kernel main context and provides file operations
  (`cd`, `pwd`, `write`, `mkdir`, `rm`, `touch`, `cp`, `mv`, `head`, `tail`,
  `hexdump`, `tree`, `df`, `sync`), disk commands, diagnostics (`boot`, `mem`,
  `ps`, `disks`, `parts`, `mount`, `umount`, `fsinfo`), and the demos
  (`demo`, `preempt`, `smp`, `compat`, `user`, `procs`, `selfcheck`). One
  command table supplies the dispatch lookup, tab completion and both help
  layouts, so a new command is one row rather than three separate edits. `help`
  prints the command grid; `help -v` adds descriptions.
- The shell banner is the cyan `Finte` ASCII logo, written raw so timestamps do
  not skew it. `shutdown` prints a staged checklist (shell input, idle CPUs,
  disk sync, timer preemption, interrupts) before powering off.
- Both consoles keep a shared 512-line history. `PageUp`/`PageDown`,
  `Shift+Up/Down`, `Home`/`End` and the PS/2 wheel move the view; any other key
  returns to live. While scrolled, the bottom row is a status bar and the VGA
  cursor is hidden.
- Once POST passes, the shell prints `Finte (base) 0.0.1` immediately before the
  motd line `Welcome to Finte.` The version (`0.0.1`) and nickname (`base`) come
  from `base.h`.
- The optional Multiboot ELF (`make kernel-elf`) is a development fallback only.

The flat kernel is about 142 KiB before bss. `make qemu-test` drives the BIOS
shell over serial; `make qemu-uefi-test` does the same under OVMF. Any POST
failure halts before the shell starts.

### Startup self-check

The POST is deliberately built from the same code as the runtime. It creates
temporary objects and flows, runs them, then calls `finte_reset()` before the
boot scenario. The six sections are:

| section | covers |
| --- | --- |
| objects | handles, generation reuse, refcounts, node types |
| capabilities | rights denial, dup, grant, grant escalation rejection |
| channels | FIFO, full/empty nonblocking, cap transfer, blocking pair |
| arenas | split, coalesce, reuse, alignment rejection, out-of-memory |
| scheduler | spawn/run, fuel epochs, flow wait/join, deadlock detection |
| linux persona | RAMFS open/read, uname, mmap, getpid |

Each **output** line carries a `[seconds.microseconds]` prefix from a
PIT-calibrated TSC. The live `finte>` prompt and the command being edited are
written raw, without a prefix, so line editing and history recall do not rewrite
a timestamp. During the self-check every individual assertion is printed as a
numbered `#NNN ok: ...` line, and long expressions are truncated so a check
never wraps on an 80-column console. The host exposes the same suite as
`finte selfcheck`.

## 10. Disk manager

The disk manager is a thin layer over block devices and filesystems:

- **Block HAL**: `hal_blk_count/info/read/write`; BIOS uses ATA PIO, UEFI uses
  AHCI DMA plus firmware Block I/O for NVMe/USB storage, and the host simulator
  can attach raw image files. Firmware devices whose capacity duplicates a
  native disk are filtered out so `hd0`/`hd1` stay stable on OVMF. The core
  never touches controller registers.
- **Firmware calls**: `hal_fw_begin()` / `hal_fw_end()` save RFLAGS, disable
  interrupts, switch to the firmware IDT, enable interrupts, then restore the
  kernel IDT and the caller's flag state. OVMF's timer and device interrupts run
  while the firmware owns the CPU; the kernel's own timer resumes afterwards.
- **Partitions**: MBR primary/extended entries and GPT headers are parsed at
  mount time. A device with no table is exposed as a whole-disk partition.
- **Mounts**: `mount hd1p2 /mnt` stores a filesystem instance and a target
  prefix. Absolute shell paths are resolved by longest-prefix match; relative
  names stay on the current disk directory or the ramdisk.
- **Filesystems**: NTFS, ext4 and lfc1 implement `fdfs_ops_t` in
  `src/core/fs_internal.h`. NTFS and ext4 are read-only. lfc1 v2 is writable.
- **lfc1 writes**: mount loads the whole tree into RAM (512 nodes, 8 MiB data,
  64 KiB directory blobs). Mutations update RAM only. `sync` assigns fresh
  extents at `next_free`, writes directories and files, then writes a new
  superblock last; an interrupted sync leaves the previous tree valid. Old trees
  are reclaimed with `tools/lfc1ck.py --compact`.
- **Read cache**: eight 4 KiB read-through entries, invalidated on any write to
  the mount.
- **File handles**: opened disk files are core nodes with READ/WRITE rights.
  `FINTE_VERB_FILE_READ` and `FINTE_VERB_FILE_WRITE` check the capability before
  calling the filesystem, so the shell cannot bypass its own authority.

There is no journal, block allocator, VFS inode cache or page cache. `docs/disk.md`
contains the command reference, the on-disk lfc1 layout and the exact NTFS/ext4
coverage.

## 11. Console color and CAS

**Color.** ANSI SGR parsing lives in one shared module (`src/core/ansi.c`),
used by VGA, the GOP framebuffer and the scrollback grid, so any kernel output
can use it without a backend-specific parser. The shell exposes `color` for a
palette preview and uses cyan for the prompt, red for errors and bold green for
success and CAS results. The serial console passes the raw bytes through, so a
terminal emulator sees the same colors.

**History and scrollback.** `scroll.c` mirrors every byte sent to the local
console into a 512-line, 256-column grid of character cells with ANSI color
indices. The grid uses a ring index, so long boot and self-check runs do not
shift memory. A scroll request clamps an absolute view line to the retained
history, repaints the window through `vga_render_cell` or `fb_render_cell`, and
draws a status bar on the last row. Returning to live repaints the tail and
restores the VGA CRTC cursor. While scrolled, `console_emit` keeps feeding the
grid but skips the live drivers, so background output cannot overwrite the
history view. PS/2 IntelliMouse packets provide the wheel; the input layer
normalizes QEMU's negative-for-wheel-up packet into a positive notch count.

**CAS.** `src/core/cas.c` implements exact rational polynomial algebra in one
variable:

- A recursive-descent parser handles integers, `x`, unary `+`/`-`, `+`, `-`,
  `*`, `/` by a constant polynomial, `^` with a non-negative integer exponent,
  and parentheses. Multiplication must be explicit.
- Coefficients are `int64_t` numerator/denominator pairs kept reduced and
  positive-denominator. Every add, multiply and power checks for overflow with
  `__builtin_*_overflow`.
- The degree limit is 32 and parenthesis nesting is limited to 8, so a deep
  expression cannot overflow the 32 KiB boot stack through parser recursion.
  `cas_parse` returns a normalized polynomial;
  `cas_diff` differentiates term by term; `cas_eval` uses Horner's rule on
  rationals and returns an exact fraction.
- `cas_format` prints the canonical `a*x^n + b*x + c` form, skipping zero terms
  and coefficient `1` where conventional.
- The host binary links the same code, so `finte cas ...` is useful for
  regression checks outside QEMU.

## 12. Memory and isolation

| environment | backing | isolation |
| --- | --- | --- |
| host simulator | `malloc`-aligned pages | one process, one address space |
| kernel flows | 32 MiB static bump heap | one address space, ring 0 |
| user processes | private 4 KiB pages per process | own PML4, CPL3, supervisor kernel map |

Native kernel flows are still ring-0 code and can touch the whole image.
User processes use a private PML4 built by `src/arch/x86_64/process.c`:

- The low 4 GiB identity map is shared as **supervisor-only** 2 MiB pages, so
  the kernel retains direct physical access while user code cannot read VGA,
  page tables, firmware memory or the kernel image at 64 MiB.
- Each process gets private 4 KiB mappings at 4 GiB: ELF `PT_LOAD` segments are
  copied into freshly zeroed pages, a 16 KiB stack is mapped just below
  4 GiB + 64 MiB, an optional shared page is mapped at 4 GiB + 128 MiB, and a
  page-scoped arena window is mapped at 4 GiB + 192 MiB.
- The PML4/PDPT/PD/PT pages, user data pages and the process record come from
  the kernel page heap. Teardown switches back to the kernel CR3, then returns
  every tracked page. Flow stacks are also recycled.
- User pointers go through `finte_user_copy_in` / `finte_user_copy_out`, which
  check the user range, walk every page, require the user and write bits as
  needed, and copy through the physical mappings.
- A page-scoped arena cap reserves a page slice in the block list. Allocation
  from that cap stays inside the slice, and a whole-arena cap cannot allocate
  or free inside it.
- A user-mode fault (#PF, #GP, #UD) prints the PID, faulting RIP and CR2, then
  kills only that flow; kernel faults still panic.
- User processes currently run on the BSP because the kernel uses one TSS.
  Kernel flows keep using all CPUs.

The native user ABI is `int 0x80` with `rax` as the call number:

| call | number | behavior |
| --- | --- | --- |
| `write` | 1 | copy a validated buffer to the console |
| `getpid` | 39 | per-process PID |
| `exit` | 60 | terminate the process and return to the shell |
| `time` | 200 | monotonic nanoseconds from `hal_now_ns()` |
| `sleep` | 201 | block the flow until the deadline; the timer tick wakes sleepers |
| `yield` | 202 | round-robin yield |
| `ipc_send` | 210 | send up to 48 bytes through a `SEND` capability |
| `ipc_recv` | 211 | block for a message through a `RECV` capability |
| `shm_addr` | 220 | return the process's shared-page VA |
| `arena_map` | 230 | map a page-scoped arena cap at its fixed VA |

`procs` exercises this ABI with four processes: message passing, shared memory,
private-data isolation, a 50 ms sleep, two CPU-bound tasks with a heartbeat, and
a deliberate read of supervisor memory that kills only the offending process.

Still missing: `fork`/`exec`, signals, per-process file descriptors, general
`mmap`, user-to-user capability transfer, and per-CPU TSS support.

## 13. HAL and portability

The core sees only `src/include/finte/hal.h`: console, monotonic time, CPU-local
state, IRQ masks, spinlocks, page allocation, context switching, block I/O and a
firmware-call window. Platform-specific boot data and UEFI hooks live in
`src/arch/x86_64/hal_x86.h`; the host image hook lives in
`src/host/hal_host.h`; the generic three-strike watchdog is in `src/core/`.

- **x86_64** (`src/arch/x86_64/`): VGA/GOP/serial consoles, PIT/LAPIC/TSC
  timers, CR3 address spaces, ATA/AHCI/UEFI block devices.
- **arm64** (`src/arch/arm64/`): QEMU `virt` with a PL011 console, `CNTPCT_EL0`
  timebase, PSCI power-off, DAIF interrupt masking, TTBR0 address-space hook,
  packed context switch, and an EL0 exception vector table. Block devices are
  not wired yet.
- `make arm64-test` boots the arm64 ELF and runs the same **147** core checks.
  The Linux syscall persona remains x86_64-numbered; the arm64 port currently
  exercises the core and scheduler, not the shell or disk stack.
- The public CPU/context fields are neutral (`hw_id`, `timer_mode`,
  `addr_space`, `sp`), so adding another platform does not change core code.

## 14. Why this shape performs

- **No path resolution.** A handle is an index; a cap is an index plus a rights mask.
- **Bounded object tables.** No global lock, no allocator on lookup.
- **O(1) run queues.** Intrusive FIFO lists, not a red-black tree scan.
- **No fork/COW.** Spawn is an object allocation, a stack/context setup, and a queue insertion.
- **Fixed messages.** Channel send copies 48 bytes into a ring; no allocation, no fragmentation.
- **Bulk by capability.** Payloads stay in arenas; the kernel does not copy megabytes.
- **Persona opt-in.** Native flows pay nothing for Linux compatibility; the translator only runs when a flow is marked Linux.
- **Fuel caps tails.** A misbehaving flow is throttled at slice granularity instead of running to completion.

The trade-offs are equally explicit: preemptive but coarse-grained SMP (a
global core lock), static flow affinity, one shared user address space, static
table limits, no demand paging, append-only lfc1, and a small Linux subset.
These are the boundaries of v0.0.1, not accidents.

## 15. Comparison with Linux

| concern | Linux | Finte v0.0.1 |
| --- | --- | --- |
| authority | uid/gid + mode/ACL + namespaces | capability per object + rights |
| process model | fork/exec, process tree | immutable flow image, explicit graph |
| fd table | inherited across fork | cap table, installed explicitly |
| names | global VFS + mount namespace | persona policy; core has no paths |
| scheduling | CFS/EEVDF, preemptive, SMP | two-band per-CPU FIFO, preemptive, up to 8 CPUs |
| IPC | pipes, sockets, shared memory | fixed messages + cap transfer + arena |
| error style | return value + errno | status + value |
| compatibility | native | Linux persona subset + minimal ring-3 ELF |
| isolation | MMU + rings | ring 3 for one demo address space; native flows ring 0 |

The point is not that one is universally better. Finte explores what a core looks like when authority and finiteness are the primary design constraints, and then adds enough Linux shape to run useful code.
