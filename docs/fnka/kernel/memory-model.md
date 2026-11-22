# Memory model

> **Status:** Partial — a shared physical page manager consumes Boot Protocol
> v1, and both x86_64 and arm64 have Flow-owned private user page tables at
> 4–5 GiB with anonymous and file-backed private `mmap`, copy-on-write fork and
> arm64 ASIDs. There is no demand paging or swap.
>
> **Normative for:** physical memory, virtual address spaces, kernel/user
> layout, allocation and mapping ownership.
>
> **Related:** [Arena](../concepts/arena.md), [Kernel boundary](kernel-boundary.md),
> [x86_64](../architecture/x86_64.md), [arm64](../architecture/arm64.md).

## 1. Current physical memory model

The shared PMM (`src/core/pmm.c`) consumes `fboot_mmap_entry_t[]` from Boot
Protocol v1. It uses two bitmaps (used and allocated) over a bounded physical
range:

| Port | PMM range | Page allocation |
| --- | --- | --- |
| x86_64 | 16 MiB–1 GiB (1 GiB identity map) | `pmm_alloc_pages`, identity addresses |
| arm64 | 1–4 GiB (`0x40000000`–`0x100000000`) | `pmm_alloc_pages`, identity addresses |

`pmm_init()` marks non-usable map entries reserved, clears usable entries, and
the boot code reserves the kernel, ramdisk/modules, boot info, descriptor
arrays, ACPI/DTB, framebuffer and x86 page tables. `hal_pages_alloc(bytes)`
rounds to 4 KiB, allocates zeroed pages from the PMM, and `hal_pages_free()`
returns any allocated page count, including non-page-multiple caller sizes.
With the PMM active, x86 flow stacks are freed through the same path; the
static `g_free_stacks` cache is used only by the no-boot-info fallback.

The old static heaps remain as fallback only:

| Port | Fallback |
| --- | --- |
| x86_64 | 32 MiB `.bss` heap + page/stack caches when no boot memory map exists |
| arm64 | 8 MiB `.bss` heap + page cache for direct `-kernel` boot |

The kernel image is linked at 64 MiB on x86_64 and at `0x40080000` on arm64.
The x86_64 boot path identity-maps the first 1 GiB with 2 MiB pages. The arm64
MMU maps 4 GiB with 2 MiB blocks and selects a 48-bit or 39-bit VA root based
on the firmware TCR.

## 2. x86_64 virtual layout

| Region | Mapping |
| --- | --- |
| `0x00000000`–`0x40000000` | Identity-mapped low memory |
| `0x04000000` (64 MiB) | Kernel image |
| 16 MiB–1 GiB | PMM allocatable identity range (kernel reserved) |
| Kernel heap | Static `.bss` fallback inside the kernel image |
| `0x100000000` (4 GiB) | `FPROC_USER_BASE`, user code/data |
| `0x104000000` | `FPROC_STACK_TOP`, user stack top |
| `0x108000000` | `FPROC_SHARED_VA`, shared page |
| `0x10C000000` | `FPROC_ARENA_VA`, mapped Arena range |
| `0x110000000` | `FPROC_MMAP_BASE`, anonymous mmap range |
| `0x120000000` | fixed ET_DYN shared-object load window |
| `0x140000000` (5 GiB) | `FPROC_USER_LIMIT` |

Each user process gets a private PML4:

- PML4 entry 0 points to a PDPT whose first four entries map the low 4 GiB
  through shared supervisor-only 2 MiB page tables;
- the user window at 4 GiB is mapped through a private user PD and page tables;
- the low identity map is supervisor-only, so ring-3 cannot read kernel memory
  through it.

User page-table entries are present, user-accessible and carry RW/NX per
mapping: code is RX, data/stack/mmap pages are RW-NX, and `EFER.NXE` is enabled
per CPU. Fork shares owned pages read-only and copies on the first write; there
is no demand paging or guard-page enforcement beyond the address-range checks.

## 3. arm64 virtual layout

The arm64 port is linked at `0x40080000`. `src/arch/arm64/mmu.c` builds a
static identity map with 2 MiB blocks:

- RAM from the v1 memory map (or `0x40000000`–`0x80000000` for direct boot) is
  Normal WB, inner-shareable, executable;
- `0x08000000`–`0x0A100000` (GIC, PL011, virtio-mmio window) is Device-nGnRnE,
  non-executable;
- the root is L0 for 48-bit VA or L1 for 39-bit VA, selected from the current
  firmware TCR; `TTBR1` is disabled;
- the BSP and APs share the same tables; secondary CPUs load the saved
  `TTBR0_EL1`, TCR, MAIR and SCTLR values in `arm64_ap_entry`.

The kernel uses PMM-backed pages, a 32 KiB flow stack per `hal_ctx_create()`,
16 KiB static AP stacks, and the same Arena/object pools as the shared core.

Each process root copies the supervisor-only kernel L1 entries, replaces
`L1[4]` with a private L2 table for the 4–5 GiB window, and allocates L3 tables
on demand for 4 KiB user pages. `src/core/process.c` owns this logic and calls
the arch backend for page tables, root activation and trap frames. Page tables
and user pages are PMM-backed and tracked in a refcounted `faddr_space_t`.

arm64 tags `TTBR0_EL1` with an 8-bit process ASID and invalidates the ASID on
activation and teardown; x86_64 uses non-global user PTEs and reloads CR3 on
every root switch. User pages are RX/RW-NX: code is read-only and executable,
data/stack/mmap pages are writable and non-executable.

## 4. Kernel heap and object pools

The core does not use `malloc` on the hot path. Nodes, Flows, Channels, Arenas,
files and directories come from fixed pools in `finte_core_t`. The pool sizes
are compile-time limits in `src/include/finte/base.h`:

| Object | Limit |
| --- | ---: |
| Nodes | 512 |
| Flows | 64 |
| Threads | 64 |
| Channels | 64 |
| Arenas | 16 |
| Files | 64 |
| Directories | 8 |
| Caps per Flow | 32 |
| Channel capacity | 32 messages |
| Message payload | 32 bytes + 2 caps |

Arenas use page-backed first-fit blocks; flow stacks and contexts use
`hal_pages_alloc()`.

## 5. User memory access

The x86 user-copy path checks:

- the virtual range is within `FPROC_USER_BASE..FPROC_USER_LIMIT`;
- the page is present;
- the page is user-accessible;
- writes require a writable entry.

A failed check returns a fault status instead of copying. Kernel pointers are
never treated as user pointers.

## 6. Ownership and lifetime

- Kernel objects are reference counted through node handles.
- A Flow owns its capability table; dropping the Flow drops its caps.
- A Flow owns a reference to its `faddr_space_t`. `finte_flow_exit()` queues
  the space for release after switching to the scheduler root; page tables are
  freed even if `proc_destroy()` is not called. `proc_destroy()` frees the
  process object and any dead fork children.
- A process owns its fd table (initrd/file capabilities), anonymous `mmap`
  mappings and the fixed shared/arena mappings. Fork copies the fd table and
  address space; exec builds a fresh space and closes non-console fds.
- An Arena owns its backing pages until the Arena is destroyed.
- A Channel owns queued messages and their attached capability references.
- Anonymous `mmap`/`munmap` exist with a 64-page-per-mapping cap. File-backed
  private `mmap` copies the fd's bytes into private pages; there is no demand
  paging.

## 7. Target model

The target contract requires:

- a physical page allocator with a page database and explicit ownership;
- virtual memory management on both architectures;
- a documented kernel and user address-space layout per architecture;
- page-table ownership, activation and TLB/ASID rules;
- guard pages and stack growth policy;
- mapping rights (read/write/execute/user);
- unmap and teardown rules;
- DMA-safe buffer ownership;
- a defined interaction between Arenas and address spaces.

## 8. Invariants

1. A page is owned by exactly one allocator or mapping at a time.
2. A user mapping never exposes supervisor memory.
3. A kernel mapping is never writable from ring 3.
4. A freed page is not reachable through any stale mapping.
5. A process cannot map outside its allowed user window.
6. An Arena mapping stays inside the Arena's backing range.
7. Object pools never hand out a live slot twice.
8. Page accounting does not underflow silently.

## 9. Current limitations

- PMM is a bitmap allocator with no per-page database, reclaim or swap.
- PMM ranges are bounded: x86 to the 1 GiB identity map, arm64 to 4 GiB.
- arm64 has an 8-bit ASID allocator and per-ASID broadcast invalidation;
  x86_64 relies on non-global PTEs and CR3 reloads, not an IPI shootdown, under
  the one-thread-per-space invariant.
- User mappings are RX/RW-NX and write-protected code. Fork shares owned pages
  read-only; a write fault makes a private copy (or upgrades the last owner in
  place), with the COW registry holding one reference per mapping space.
- Anonymous `mmap`/`munmap` are capped at 64 pages per mapping; no file-backed
  mmap or demand paging.
- No IOMMU or DMA address-space isolation.
- The x86 user low identity map covers 4 GiB supervisor, not a minimal kernel
  window.
- ACPI/DTB are passed and reserved but not parsed by the allocator or platform
  layer.

## 10. Open questions

- PMM reclaim, page database and memory-pressure policy.
- Kernel/user address layout for both architectures.
- Whether the kernel keeps a full identity map or a minimal direct map.
- Copy-on-write fork and cross-CPU TLB shootdown for shared spaces.
- Guard pages, stack growth and fault handling.
- File-backed `mmap` and a fuller native mapping contract.
- Arena-to-address-space mapping and revocation.
- DMA buffer pinning and IOMMU integration.
- Memory pressure, reclaim and out-of-memory policy.

## 11. Test evidence

- `src/tests/test_main.c`: Arena allocation, object pools, limits.
- `src/core/selftest.c`: heap and mapping checks.
- `procs` / `services`: private page tables, shared page, Arena page caps,
  user copy validation and fault kill.
- `make qemu-disk-test`: filesystem-backed reads and writes.
- `make test`: PMM map consumption, allocation/free, reservation, clamping,
  churn, fragmentation, double-free and exhaustion.
- `make arm64-test`: MMU identity translations for kernel and device addresses,
  GICv2 timer ticks, 4-CPU PSCI startup, per-CPU flow distribution, EL0 user
  pages and the isolated `procs` demo.
- `make arm64-uefi-test`: same checks plus PMM counts from the UEFI memory map.
- `make arm64-disk-test`: virtio-mmio lfc1/ext4/NTFS reads (legacy + modern).
- `make abi-test`: x86_64 fd table, anonymous and file-backed mmap, COW fork,
  waitpid, exec, signal sets, job control, process groups, SIGALRM timers,
  cap transfer, churn, blocked-flow abort and RX/RW-NX PTE checks.
- `make arm64-test`: the same shared ABI suite on 4 CPUs plus the EL0/procs
  demos and Flow-owned space release.
- No shared-address-space TLB shootdown or reclaim test yet.
