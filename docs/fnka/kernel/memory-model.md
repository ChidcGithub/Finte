# Memory model

> **Status:** Partial — x86_64 has paging, private user page tables and a
> static kernel heap. arm64 has no MMU setup and only a static heap. There is
> no general physical page allocator, demand paging or swap.
>
> **Normative for:** physical memory, virtual address spaces, kernel/user
> layout, allocation and mapping ownership.
>
> **Related:** [Arena](../concepts/arena.md), [Kernel boundary](kernel-boundary.md),
> [x86_64](../architecture/x86_64.md), [arm64](../architecture/arm64.md).

## 1. Current physical memory model

Finte does not yet have a full physical memory manager. Both bare-metal ports
use a static kernel heap and a page-granular suballocator:

| Port | Heap | Page allocation |
| --- | --- | --- |
| x86_64 | 32 MiB static `.bss` heap | Bump pointer with free page and 32 KiB stack caches |
| arm64 | 8 MiB static `.bss` heap | Bump pointer with a free page cache |

`hal_pages_alloc(bytes)` rounds to 4 KiB, takes pages from the bump pointer,
and uses cached free pages first. `hal_pages_free()` returns pages to the cache
when they are inside the heap range. There is no NUMA awareness, no physical
page database, no reclaim and no swap.

The kernel image is linked at 64 MiB on x86_64 and at `0x40080000` on arm64.
The x86_64 boot path identity-maps the first 1 GiB with 2 MiB pages.

## 2. x86_64 virtual layout

| Region | Mapping |
| --- | --- |
| `0x00000000`–`0x40000000` | Identity-mapped low memory |
| `0x04000000` (64 MiB) | Kernel image |
| Kernel heap | Static `.bss` inside the kernel image |
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

The arm64 port is linked at `0x40080000` and runs with the firmware's identity
mapping after the UEFI loader calls `ExitBootServices`. It does not program
its own translation tables, does not use ASIDs, and has no user address space.

The kernel uses:

- a static 8 MiB heap in `.bss`;
- a 32 KiB flow stack per `hal_ctx_create()`;
- the same Arena and object pools as the shared core.

A real arm64 MMU contract is a Phase 1 objective.

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
- A process owns its page tables and mapped user pages; teardown releases them.
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

- No general physical page allocator.
- No page database, reclaim or swap.
- arm64 has no MMU or user address space.
- No NX or read-only user mappings.
- No demand paging or COW.
- No general `mmap`/`munmap`.
- No TLB shootdown protocol for user mappings.
- No IOMMU or DMA address-space isolation.
- The x86 user low identity map covers 4 GiB supervisor, not a minimal kernel
  window.

## 10. Open questions

- Physical page allocator design and metadata placement.
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
- No arm64 MMU, user mapping or TLB test yet.
