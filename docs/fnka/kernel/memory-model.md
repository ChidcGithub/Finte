# Memory model

> **Status:** Partial — a shared physical page manager consumes Boot Protocol
> v1, and both x86_64 and arm64 have private user page tables at 4–5 GiB.
> There is no demand paging, swap, ASID allocator or general user `mmap`.
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
| `0x140000000` (5 GiB) | `FPROC_USER_LIMIT` |

Each user process gets a private PML4:

- PML4 entry 0 points to a PDPT whose first four entries map the low 4 GiB
  through shared supervisor-only 2 MiB page tables;
- the user window at 4 GiB is mapped through a private user PD and page tables;
- the low identity map is supervisor-only, so ring-3 cannot read kernel memory
  through it.

User page-table entries are present, writable and user-accessible (`0x7`) for
the demo mappings. There is no NX policy, copy-on-write, demand paging or
guard-page enforcement beyond the address-range checks.

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

Each arm64 process root copies the supervisor-only kernel L1 entries, replaces
`L1[4]` with a private L2 table for the 4–5 GiB window, and allocates L3 tables
on demand for 4 KiB user pages. The private root is loaded into `TTBR0_EL1` on
context switch; the kernel remains mapped through the shared low identity map.
Page tables and user pages are PMM-backed and tracked for `proc_destroy()`.
There is no ASID allocator or TLB-shootdown protocol yet; switches use
`tlbi vmalle1is`, and user pages are RWX.

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
- A process owns its page tables and mapped user pages; `proc_destroy()`
  releases them. `finte_flow_exit()` does not release the address space itself.
- An Arena owns its backing pages until the Arena is destroyed.
- A Channel owns queued messages and their attached capability references.
- There is no general user-accessible `mmap`/`munmap`; only the demo Arena map
  and fixed shared page exist.

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
- arm64 has an identity kernel map plus per-process user roots, but no ASID
  allocator or TLB-shootdown protocol.
- No NX or read-only user mappings on either architecture.
- No demand paging or COW.
- No general `mmap`/`munmap`.
- No TLB shootdown protocol for user mappings.
- No IOMMU or DMA address-space isolation.
- The x86 user low identity map covers 4 GiB supervisor, not a minimal kernel
  window.
- ACPI/DTB are passed and reserved but not parsed by the allocator or platform
  layer.

## 10. Open questions

- PMM reclaim, page database and memory-pressure policy.
- Kernel/user address layout for both architectures.
- Whether the kernel keeps a full identity map or a minimal direct map.
- Page-table activation and TLB shootdown across CPUs.
- Guard pages, stack growth and fault handling.
- Mapping rights and the native `mmap` contract.
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
- No arm64 `fork`/general `mmap`, TLB-shootdown or reclaim test yet.
