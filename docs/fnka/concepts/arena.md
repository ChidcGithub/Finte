# Arena contract

> **Status:** Partial — page-backed first-fit Arenas, scoped capabilities and
> x86 user mappings exist. Revocation, read-only mappings, unmap and
> cross-architecture user mappings are incomplete.
>
> **Normative for:** Arena ownership, allocation, scoped sharing, mapping,
> freeing and lifetime.
>
> **Related:** [Capability](capability.md), [Channel](channel.md),
> [Memory model](../kernel/memory-model.md), [Security model](../security/security-model.md).

## Resolved P0 decisions (2026-11-22)

1. **v0.1 supports writable scoped Arena mappings on x86_64 only.** A mapping
   requires a scoped capability with `WRITE` (and later `MAP`). Read-only and
   execute mappings are deferred.
2. **The mapping is valid only while its scoped capability remains held.** The
   process must keep the capability for the mapping's lifetime. Process
   teardown drops capabilities and releases its page tables; the Arena object
   remains until its node references are gone.
3. **No unmap or mapped-page revocation in v0.1.** These require a mapping
   reference and TLB-shootdown contract. They are deferred to v0.2.
4. **The backing pages are freed when the Arena node is destroyed.** Destruction
   must not race an outstanding mapping; v0.1 relies on the capability/reference
   rule above and process teardown ordering.
5. **Allocation is first-fit with 16-byte payload alignment.** `align > 16` is
   rejected. Page ranges are reserved at 4 KiB granularity.
6. **Shared writable synchronization is userland's responsibility.** The kernel
   provides the mapping, not a memory-ordering protocol for user data.
7. **Cross-architecture user mappings are deferred** until arm64 has an MMU and
   user mode.

## 1. Definition

An **Arena** is a managed memory region and sharing domain. It provides:

- a creator-owned backing region;
- first-fit block allocation;
- capability-scoped page ranges;
- a place to share bulk data without copying it through Channels.

An Arena is not a general process address space and does not grant authority to
unrelated resources.

## 2. Current object model

`farena_t` in `src/include/finte/object.h` contains:

| Field | Meaning |
| --- | --- |
| `base`, `size` | Page-backed backing region |
| `used`, `live_bytes`, `peak_bytes` | Accounting |
| `free_list` | Address-ordered block list |
| `self` | Node handle |
| `allocs`, `frees` | Counters |
| `name` | Diagnostic name |

`finte_arena_create(name, bytes)`:

- defaults to 64 KiB when `bytes == 0`;
- rounds the size up to 4096 bytes;
- allocates pages through `hal_pages_alloc()`;
- stores the Arena in a fixed pool of `FINTE_MAX_ARENAS` (16).

## 3. Allocation model

The allocator is a first-fit block list with:

- 16-byte block payload alignment;
- a block header (`FINTE_ARENA_BLOCK_HDR`);
- address-ordered blocks;
- splitting when the remainder is useful;
- coalescing adjacent free unscoped blocks.

`finte_arena_alloc(a, size, align, *offset)` allocates anywhere in the Arena.
`align` is clamped to at least 16; values greater than 16 are rejected.

`finte_arena_alloc_scoped(a, offset, length, size, align, *out)` allocates only
inside the given range and marks the block as scoped.

The returned offset points to the payload after the block header.

## 4. Scoped capabilities

`finte_cap_install_arena_pages(f, arena, first_page, pages, rights)`:

1. validates the Arena node and page range;
2. calls `finte_arena_reserve()` to mark the range as a scoped free block;
3. installs a capability with `FINTE_CAP_SCOPED`, `offset` and `length`.

The scoped capability constrains later allocation and mapping to the reserved
range.

`finte_arena_reserve()` splits a free block into head, scoped range and tail.
It rejects ranges that would leave a fragment smaller than a block header.

## 5. Free and destroy

`finte_arena_free(a, offset)` frees an unscoped block.
`finte_arena_free_scoped(a, offset, scoped)` enforces that the scoped flag
matches the block. It updates accounting and coalesces adjacent free unscoped
blocks.

`finte_arena_destroy(a)` frees the backing pages and clears the object. It does
not itself revoke capabilities or unmap user pages; node and mapping lifetime
must be handled by the caller and the address-space teardown path.

## 6. x86 user mapping

The x86 ring-3 path supports mapping a scoped Arena capability into a process:

- the cap must have `FINTE_R_WRITE`;
- the cap must be `FINTE_CAP_SCOPED`;
- `length` must be non-zero and at most `FPROC_ARENA_MAX_PAGES` (4) pages;
- the mapped pages are placed at `FPROC_ARENA_VA + offset`;
- the syscall returns `FPROC_ARENA_VA + FINTE_ARENA_BLOCK_HDR`, the payload
  address after the block header.

The current mapping is writable user memory. There is no read-only Arena map,
no `unmap` verb, and no arm64 user mapping.

## 7. Ownership and lifetime

- The Arena creator owns the backing pages.
- A capability is a reference; dropping it does not free the Arena while other
  node references exist.
- Queued Channel messages may carry Arena capabilities; the Channel retains
  the node reference while the message is queued.
- A user process may hold a scoped Arena capability and a mapping; process
  teardown releases its page tables and capability table, but the Arena object
  remains until its node references are gone.
- An Arena capability does not grant access to another Arena or to the owning
  Flow's other resources.

## 8. Target contract

The target model must define:

- creator and owner;
- which Flows may map the Arena;
- mapping rights and read-only/writable modes;
- unmap and destruction behavior;
- synchronization responsibility for shared writable memory;
- cache and memory-ordering requirements;
- behavior when an owner exits while other mappings remain;
- whether mappings survive capability revocation;
- how DMA and device access interact with Arena backing pages.

Arena access must remain capability-controlled. Sharing an Arena must not
silently grant unrelated rights.

## 9. Invariants

1. An Arena's block list remains address-ordered.
2. A scoped allocation stays inside its reserved range.
3. A scoped free cannot free an unscoped block.
4. A capability's `offset + length` stays inside the Arena.
5. A mapped page belongs to the Arena's backing region.
6. Freeing a block does not free the Arena while references remain.
7. Arena accounting never underflows silently.
8. A process cannot map an Arena without a scoped, writable capability.

## 10. Current limitations

- No read-only mappings.
- No explicit unmap operation.
- No arm64 user mapping.
- No revocation of an already-mapped range.
- `finte_arena_destroy()` does not coordinate with outstanding mappings.
- No cross-architecture cache/ordering contract.
- No IOMMU or device mapping support.
- The allocator is first-fit with 16-byte alignment, not a general-purpose
  page allocator.

## 11. Remaining open questions (post-v0.1)

- Whether mappings are revoked synchronously when a capability is dropped.
- How owner exit affects mappings held by other Flows.
- Read-only and execute mappings.
- Unmap semantics and TLB/cache maintenance.
- Shared writable memory synchronization primitives.
- DMA buffer ownership and pinning.
- Whether Arenas can be resized or split.
- How Arena capabilities interact with the native ABI and userland allocators.
- How arm64 maps Arena pages once it has an MMU.

## 12. Test evidence

- `src/tests/test_main.c`: Arena allocation, scoped capabilities, free.
- `src/core/selftest.c`: Arena checks.
- `procs`: per-process Arena page caps, shared page and isolation checks.
- `services`: shared Arena/page behavior across user services.
- No test yet for unmap, read-only mappings or revocation of mapped ranges.
