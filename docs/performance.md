# Performance notes

Finte is designed around bounded, short paths rather than around matching Linux's syscall table. This document separates what was measured from what was not.

## How the numbers were produced

```bash
make bench
# or
./build/finte bench
```

The benchmark harness:

- runs one warm process per row,
- keeps all flows on one core,
- uses `hal_cycles()` for timing,
- reports one run, not a statistical distribution.

On Linux/WSL, `hal_cycles()` is `CLOCK_MONOTONIC_RAW` in nanoseconds. On Windows it is `QueryPerformanceCounter` ticks converted with the measured frequency. The Windows host uses Win32 fibers; Linux/WSL uses `ucontext`.

These are **host simulator** numbers. The core code is the same on bare metal, but the host context mechanism and libc are different, and the kernel path is not microbenchmarked yet.

## Results

WSL Ubuntu 26.04, x86_64, GCC 15, one core:

| operation | ns/op | Mops/s |
| --- | ---: | ---: |
| verb + clock read (`CLOCK_NOW`) | 21.9 | 45.6 |
| capability validate + info | 5.4 | 185.2 |
| arena alloc + free (64 bytes) | 14.3 | 69.8 |
| channel roundtrip (blocking) | 733.5 | 1.36 |
| flow spawn + run (no-op) | 1995.3 | 0.50 |

Native Windows 11, x86_64, clang-cl, one core:

| operation | ns/op | Mops/s |
| --- | ---: | ---: |
| verb + clock read (`CLOCK_NOW`) | 20.9 | 47.8 |
| capability validate + info | 5.1 | 196.4 |
| arena alloc + free (64 bytes) | 14.7 | 67.9 |
| channel roundtrip (blocking) | 223.1 | 4.48 |
| flow spawn + run (no-op) | 10128.1 | 0.10 |

### What each row means

- **Verb + clock read**: one `finte_syscall` dispatch, a capability check, a node lookup, a HAL clock read, and a copy-out of one `uint64_t`.
- **Capability validate + info**: one syscall, capability bounds/generation/rights check, node lookup, and a 24-byte struct copy-out.
- **Arena alloc + free**: first-fit allocation, split accounting, free, coalescing pass.
- **Channel roundtrip**: two flows ping-pong 20,000 messages. Each roundtrip includes two blocking operations and the scheduler context switches around them. On Linux this is `swapcontext`/`makecontext`; on Windows it is `SwitchToFiber`.
- **Flow spawn + run**: create 32 flows, allocate their stacks/contexts, start them, run the scheduler to completion. The row includes host stack allocation and context construction, not just the run queue insertion.

The Windows fiber path is much faster for channel switching (223 ns vs 734 ns) and much slower for creating 32 fiber stacks (10.1 µs/flow vs 2.0 µs/flow). This is exactly the kind of platform difference the HAL is meant to isolate.

## Kernel footprint

`make image` produces:

- `build/kernel64.bin`: **145,568 bytes** of flat 64-bit kernel text/data (v0.0.1, GCC 15, `-O2`).
- `build/finte.img`: **68,157,440 bytes** hybrid BIOS+UEFI USB image (64 MiB FAT32 ESP).
- `build/initrd.bin`: **12,816 bytes** with `init.elf` and `ipc.elf`.
- `build/init.elf`: **5,088 bytes** · `build/ipc.elf`: **7,040 bytes**.
- 32 MiB static bump heap in `.bss`, cleared at boot.
- Static core tables: 512 nodes, 64 flows, 64 channels, 16 arenas, 64 files, 8 dirs.
- 32 KiB stack per flow, allocated from the kernel heap.
- 32 KiB boot stack in `.bss.stack`.

The QEMU boot tests drive the CLI over serial on both BIOS/SeaBIOS and
UEFI/OVMF, run the native pipeline and Linux persona from the shell, and check
the POST.

## Why the core paths are short

1. **Handle lookup is an index.** `slot = handle & 0xffffffff - 1`, bounds check, generation compare. No hash table, no path walk.
2. **Capability check is a bounds check plus a bitmask.** `(rights & need) == need`.
3. **Run queues are O(1).** Intrusive FIFO lists per priority band; no tree search.
4. **Messages do not allocate.** A 48-byte message is copied into a preallocated ring; the attached caps are references.
5. **Bulk data does not cross the kernel.** Arenas are shared by capability; offsets travel in messages.
6. **Spawn is not fork.** No address space copy, no COW tree, no fd table clone. A flow is an object, a stack/context, and a queue insertion.
7. **Personas are opt-in.** Native flows do not execute any Linux translation code.
8. **The object table is static.** The hot path never calls a general-purpose allocator. Arenas use page-backed first-fit blocks, and the block list is address-ordered.

## Costs and caveats

- **Preemptive per CPU, coarse-grained SMP.** The BSP uses the PIT and APs use
  calibrated LAPIC timers at 100 Hz. Each CPU has its own run queue, but flows
  keep a static affinity and every verb takes one global core lock. A timer
  tick only switches out the current flow when its CPU has another queued flow,
  so a lone compute-bound flow runs without a context switch every 10 ms.
- **Reschedule IPIs cost more than local scheduling.** IPIs are sent only to
  CPUs parked in HLT with queued work; a wakeup targeting a running CPU waits
  for its next yield or timer tick. There is no work stealing or migration.
- **Disk writes avoid redundant trees.** lfc1 tracks a dirty flag, so a
  read-only mount/umount or an explicit `sync` with no changes does not append a
  new tree. The block cache holds 32 4 KiB entries.
- **Startup is console-bound.** The GOP path uses packed 64-bit glyph stores and
  blank-row tracking, and serial runs at 115200 baud. The kernel reaches the
  shell in about 0.8 s on BIOS with serial disabled; UEFI wall time is mostly
  OVMF firmware.
- **WHPX changes the host bound, not the guest.** The Windows launcher uses
  WHPX when available: the 4-CPU `preempt` benchmark is about 135 ms versus
  397 ms under TCG. The guest image and its semantics are unchanged.
- **Isolated user processes.** Each user process has a private PML4 with
  supervisor-only kernel mappings and private 4 KiB pages at 4 GiB. User
  processes run on the BSP because the kernel keeps one TSS; native kernel
  flows still use every CPU. Page-table walks validate every user buffer.
- **Host context switches dominate channel latency.** The core's send/receive logic is far below the 734 ns roundtrip; the four `ucontext` switches are the bulk.
- **Spawn cost is host stack/context setup.** The kernel crafts a stack with six saved registers and a return address, which is much cheaper than `makecontext`/`CreateFiber`.
- **One run, one process.** These numbers are not a distribution; run the benchmark multiple times before drawing fine-grained conclusions.
- **Not a Linux comparison.** A Linux `read`/`write` benchmark includes a trap, a validated user pointer, VFS path resolution, security checks and often a scheduler interaction. Finte's native verb path is a direct call. Comparing them directly would be misleading; the relevant comparison is the architectural work performed.

## Reproduce

```bash
make bench
# WSL: Linux ucontext
# Windows: build-win.bat && build\finte.exe bench
```

`make qemu-test` verifies the metal path and prints the kernel's own counters:

```text
syscalls=90 flows=4/4 switches=12
msgs=24/24 caps=11/11 denied=2 fuel=288 epochs=0
```

## Next performance steps

1. Replace host `ucontext` with the kernel's assembly switch for x86_64 hosts.
2. Cache host stacks per size class instead of `malloc`/`free` per spawn.
3. Measure the `int 0x80`/IRET and per-process CR3 switch cost on metal.
4. Add mapped arena windows and page-granular capability slices; eliminate host-pointer assumptions.
5. Per-CPU TSS/GDT so user processes can migrate across cores; then per-core scheduler shards.
6. A kernel microbenchmark that reads the TSC, so metal numbers do not depend on the host HAL.
