# Capability contract

> **Status:** Partial — install/check/drop/duplicate/grant exist. Transitive
> revocation, revocation domains, move semantics and per-object rights masks
> do not.
>
> **Normative for:** authority representation, capability-table operations,
> rights derivation, transfer, invalidation and lifetime.
>
> **Related:** [Flow](flow.md), [Channel](channel.md), [Arena](arena.md),
> [Kernel boundary](../kernel/kernel-boundary.md), [Security model](../security/security-model.md).

## Resolved P0 decisions (2026-11-22)

1. **Handles remain generation-checked identifiers; capability-table indices are
   the user-visible authority.** Verbs never accept a raw node handle as
   authority. The handle layout stays `(generation << 32) | (slot + 1)` for
   v0.1.
2. **The current `FINTE_R_*` vocabulary is frozen for the 0.0.x ABI.** The
   target names (`EXECUTE`, `TRANSFER`, `DUPLICATE`, `MAP`, `WAIT`) are reserved
   for the native ABI and will not be introduced by silently renaming bits.
3. **Per-object rights masks are fixed as follows for v0.1:**

   | Object | Valid rights |
   | --- | --- |
   | Console | `READ`, `WRITE`, `CONTROL` |
   | Flow | `READ`, `CONTROL` (wait), `GRANT`, `SIGNAL` |
   | Channel | `SEND`, `RECV`, `CONTROL`, `GRANT`, `WAIT` |
   | Arena | `READ`, `WRITE`, `MAP`, `CONTROL` |
   | File | `READ`, `WRITE`, `CONTROL` |
   | Directory | `READ`, `WRITE`, `CONTROL` |

   Current verbs may use `WRITE` for Arena allocation/mapping and `CONTROL` for
   Flow wait until `MAP`/`WAIT` are added in the native ABI.
4. **Derivation is rights-reducing copy.** `CAP_DUP` and `CAP_GRANT` may only
   produce a subset of the source rights. `CAP_GRANT` copies; it does not move.
   There is no move/transfer operation in v0.1.
5. **Revocation is individual in v0.1.** Dropping a capability removes one
   table entry. There are no revocation domains and no transitive revocation of
   derived copies. Node destruction invalidates outstanding capabilities
   lazily through generation checks.
6. **In-flight operations keep their validated authority.** Verbs are
   serialized by the core lock. A blocking operation that validated its
   capability before sleeping may complete after another Flow drops its own
   copy; the kernel does not tear down an in-flight operation.
7. **No per-Thread capability tables in v0.1.** A Thread uses its Flow's table.
   Per-Thread restrictions are deferred.

## 1. Governing rule

> **No Capability, No Access.**

A Flow receives no ambient authority. To operate on an object, it must hold a
capability for that object with the required rights. The kernel validates the
capability at the protected boundary.

Knowledge of an object's numeric handle is not authority. Verbs take a
capability-table index, not a raw object handle. A handle by itself cannot be
used to obtain a capability.

## 2. Current representation

### Node and handle

Every kernel object is a node in a fixed table:

```c
typedef struct fnode {
    uint16_t type;
    uint16_t flags;
    uint32_t gen;
    uint32_t refs;
    uint32_t index;
    uint32_t aux;
} fnode_t;
```

A handle is:

```text
(gen << 32) | (slot + 1)
```

- Zero is never valid.
- Lookup is O(1): bounds check, type check, generation check.
- A reused slot increments `gen`, so a stale handle fails instead of aliasing
  a new object.
- Nodes are reference counted. Capabilities, queued Channel messages, the
  scheduler, directories and other holders retain references.

Handles are identifiers, not authority. The capability table is the authority.

### Capability entry

`fcap_t` in `src/include/finte/object.h` currently contains:

| Field | Meaning |
| --- | --- |
| `handle` | Node handle |
| `rights` | Granted rights bitmask |
| `flags` | `FINTE_CAP_SCOPED` for range-scoped capabilities |
| `offset`, `length` | Range for a scoped capability |

Each Flow has a fixed table of `FINTE_MAX_CAPS` (32) entries. A table slot is
identified by its index. The index is local to the Flow; the same numeric
index in another Flow is unrelated.

## 3. Current rights

The current rights vocabulary is defined in `src/include/finte/base.h`:

| Right | Current use |
| --- | --- |
| `FINTE_R_READ` | File read, inspection |
| `FINTE_R_WRITE` | Console write, Arena allocation/free/map, file write |
| `FINTE_R_SEND` | Channel send |
| `FINTE_R_RECV` | Channel receive |
| `FINTE_R_EXEC` | Reserved for executable images |
| `FINTE_R_GRANT` | Permission to grant capabilities to a target Flow |
| `FINTE_R_CONTROL` | Flow wait, Channel close, lifecycle control |
| `FINTE_R_SIGNAL` | Reserved for asynchronous notification |

Operation checks in the current kernel:

| Operation | Required right |
| --- | --- |
| `CHAN_SEND` | `SEND` on the Channel cap |
| `CHAN_RECV` | `RECV` on the Channel cap |
| `CHAN_CLOSE` | `CONTROL` on the Channel cap |
| `FLOW_WAIT` | `CONTROL` on the Flow cap |
| `ARENA_ALLOC` / `ARENA_FREE` | `WRITE` on the Arena cap |
| User Arena map (x86) | `WRITE` and a scoped cap |
| `FILE_READ` | `READ` on the file cap |
| `FILE_WRITE` | `WRITE` on the file cap |
| `CAP_GRANT` | `GRANT` on the target Flow cap |
| `CAP_DUP` | Any valid cap; optional rights subset |
| `CONSOLE_WRITE` | `WRITE` on the console cap |

Not every right is meaningful for every object type. The current verbs enforce
the specific bits they need; there is no single per-object rights mask yet.

## 4. Operations

### Install

`finte_cap_install()` places a handle and rights into the first free slot and
retains a node reference. `finte_cap_install_scoped()` adds an offset/length
range. `finte_cap_install_arena_pages()` reserves a page range and installs a
scoped Arena capability.

Installation must never create authority: the caller must already hold the
object or be constructing it as a trusted creator.

### Check

`finte_cap_check(f, slot, need, out)`:

1. validates the slot index;
2. rejects an invalid handle;
3. looks up the node;
4. rejects a missing node and clears the stale slot;
5. checks `(cap.rights & need) == need`;
6. increments denial counters on failure;
7. optionally returns a copy of the capability entry.

A failed check returns `FINTE_ERR_NOENT`, `FINTE_ERR_PERM` or another explicit
status. It does not silently downgrade.

### Drop

`finte_cap_drop_index()` clears one slot and releases its node reference. It
does not revoke other copies of the same object. `finte_cap_drop_all()` drops
every capability when a Flow exits.

### Duplicate

`CAP_DUP` copies an existing capability into another slot of the same Flow,
optionally reducing rights:

```text
Rights(child) ⊆ Rights(parent)
```

The current verb does not require a separate `DUPLICATE` right; holding the
source capability is sufficient.

### Grant

`CAP_GRANT` installs a copy of a source capability into a target Flow. The
current implementation requires `GRANT` on a capability for the target Flow,
then checks the source capability and optionally reduces rights. The source
Flow retains its capability. This is a **copy**, not a move.

### Channel attachment

A Channel message may carry up to two capability attachments. The sender must
hold each attached capability. On send, the Channel takes a node reference for
each attachment; the sender keeps its capability. On receive, the receiver gets
copies installed in its own table. This is capability **sharing**, not
transfer.

## 5. Target rights vocabulary

The target contract proposes a more operation-oriented vocabulary:

| Target right | Meaning | Current mapping |
| --- | --- | --- |
| `READ` | Read or inspect an object | `FINTE_R_READ` |
| `WRITE` | Modify an object | `FINTE_R_WRITE` |
| `EXECUTE` | Execute an object or invoke an executable operation | `FINTE_R_EXEC` |
| `TRANSFER` | Transfer a capability to another authorized holder | `FINTE_R_GRANT` (copy, not move) |
| `DUPLICATE` | Create a restricted duplicate | currently implicit in holding the cap |
| `CONTROL` | Change lifecycle or configuration | `FINTE_R_CONTROL` |
| `MAP` | Map an object into an address space | currently `WRITE` for Arena map |
| `WAIT` | Wait for an event or state change | currently `CONTROL` for Flow wait |
| `SIGNAL` | Request asynchronous notification | `FINTE_R_SIGNAL`, reserved |

This mapping is **provisional**. Renaming or adding rights is an ABI change and
must be accompanied by a migration note and tests.

## 6. Derivation, transfer and revocation

### Derivation

A derived capability must never grant more authority than its parent:

```text
Rights(child) ⊆ Rights(parent)
```

The current `CAP_DUP` and `CAP_GRANT` paths enforce the rights subset. Scoped
Arena capabilities additionally constrain the byte range.

### Transfer

The current model has no move operation. `CAP_GRANT` and Channel attachment
create additional references. The source remains usable unless its holder
drops it.

Target semantics must distinguish:

- **copy:** both holders retain authority;
- **move:** the source loses authority atomically;
- **share:** a new reference to the same object with independently constrained
  rights.

### Revocation

Current behavior:

- Dropping one capability removes only that table entry.
- Other capabilities for the same node remain valid.
- Node destruction invalidates capabilities lazily: a later check finds the
  node missing and clears the stale entry.
- There is no transitive revocation of capabilities derived from a common
  parent.
- There is no revocation domain, epoch or bulk-revoke operation.
- In-flight operations are serialized by the global core lock; a capability
  dropped by another Flow cannot race an ordinary verb. A blocking operation
  may have already validated its capability before it sleeps.

Target behavior must define:

- whether individual capabilities can be revoked;
- whether derived capabilities are revoked transitively;
- how in-flight operations behave;
- whether revocation is synchronous or asynchronous;
- how object destruction interacts with outstanding capabilities;
- how stale slots are detected;
- how references prevent use-after-free.

## 7. Invariants

1. A capability slot belongs to exactly one Flow.
2. A capability's rights are a subset of the rights it was derived from.
3. A capability never outlives its node without becoming invalid.
4. A stale handle never aliases a new object; generation checks prevent this.
5. Dropping one capability does not silently revoke unrelated copies.
6. A Flow cannot use another Flow's capability slot index as authority.
7. A message attachment retains the node while the message is queued.
8. A capability check failure is explicit and counted.

## 8. Current limitations

- No revocation domains or transitive revocation.
- No `TRANSFER`/move operation.
- No per-object rights mask shared by all verbs.
- `GRANT` is checked on the target Flow capability, not the source object.
- `CAP_DUP` does not require a `DUPLICATE` right.
- Rights names are not yet the target vocabulary.
- No capability audit log beyond counters.
- No user-to-user capability transfer except through Channel attachments or
  `CAP_GRANT` with a target Flow capability.

## 9. Remaining open questions (post-v0.1)

- Move/transfer semantics and whether `CAP_GRANT` becomes move or stays copy.
- Revocation domains and transitive revocation.
- Whether a blocking operation should be aborted when its capability is dropped.
- Capability inheritance policy at Flow creation.
- The native rights vocabulary and the final per-object masks.
- Auditing and debugging of authority grants.
- Whether capabilities can be scoped to operations as well as objects.
- How revocation interacts with mapped Arena pages and DMA windows.

## 10. Test evidence

- `src/tests/test_main.c`: capability install/check/drop, scoped Arena caps,
  rights denial.
- `src/core/selftest.c`: core capability checks.
- `procs`/`services`: Channel capability passing and Arena page caps.
- No test yet for transitive revocation, move semantics or a full revocation
  domain.
