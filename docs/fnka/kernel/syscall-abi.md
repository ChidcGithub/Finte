# Native syscall ABI

> **Status:** Partial — a stable internal verb ABI exists and x86_64 has a
> small ring-3 demo syscall set. There is no frozen native user ABI, native
> libc, or ABI version negotiation.
>
> **Normative for:** syscall numbering, calling convention, argument passing,
> error reporting, capability handles, user/kernel structure layout and
> versioning.
>
> **Related:** [Capability](../concepts/capability.md), [Flow](../concepts/flow.md),
> [Kernel boundary](kernel-boundary.md), [Linux persona](../../compat.md).

## 1. Current internal verb ABI

Kernel components use:

```c
fret_t finte_syscall(fflow_t *f, uint32_t nr, const uint64_t args[6]);
```

A verb is a small fixed request. Arguments are values, never paths. Bulk data
is represented by a capability, usually an Arena capability.

`fret_t` returns status and value separately:

```c
typedef struct fret {
    int32_t status;
    uint32_t detail;
    uint64_t value;
} fret_t;
```

A successful call has `status == FINTE_OK`; errors use the `FINTE_ERR_*`
statuses. A successful call cannot be confused with an error.

The current verb table (`src/include/finte/verb.h`) contains:

| Number | Verb |
| ---: | --- |
| 0 | `NOP` |
| 1 | `SYS_INFO` |
| 2 | `CAP_INFO` |
| 3 | `CAP_DUP` |
| 4 | `CAP_DROP` |
| 5 | `CAP_GRANT` |
| 6 | `CHAN_SEND` |
| 7 | `CHAN_RECV` |
| 8 | `CHAN_INFO` |
| 9 | `FLOW_YIELD` |
| 10 | `FLOW_EXIT` |
| 11 | `FLOW_INFO` |
| 12 | `FLOW_FUEL` |
| 13 | `ARENA_ALLOC` |
| 14 | `ARENA_FREE` |
| 15 | `CONSOLE_WRITE` |
| 16 | `FILE_READ` |
| 17 | `CLOCK_NOW` |
| 18 | `SYS_STATS` |
| 19 | `SYS_DUMP` |
| 20 | `FLOW_WAIT` |
| 21 | `CHAN_CLOSE` |
| 22 | `FILE_WRITE` |

Verbs take capability-table indices, not raw node handles. Unknown or sparse
verb numbers return `FINTE_ERR_NOSYS`.

## 2. Current x86_64 ring-3 ABI

Ring 3 enters through `int 0x80` into `usermode_syscall()`, which calls
`finte_process_syscall(uint64_t *regs)`.

The demo syscall numbers are defined in `src/arch/x86_64/process.h`:

| Number | Operation |
| ---: | --- |
| 1 | `write` |
| 39 | `getpid` |
| 60 | `exit` |
| 200 | `time` (monotonic nanoseconds) |
| 201 | `sleep` (nanoseconds, capped at 5 s) |
| 202 | `yield` |
| 210 | `ipc_send` |
| 211 | `ipc_recv` |
| 220 | `shm_addr` |
| 230 | `arena_map` |

The current convention returns a single `uint64_t`; `-1` indicates failure.
Some Linux-persona functions translate this into `errno`-style behavior. This
is a demo interface, not the native ABI.

## 3. Current ABI properties

- Arguments are passed in registers; the exact register order is
  architecture-specific and not yet documented as a stable contract.
- Capability arguments are 16-bit indices in the calling Flow's table.
- User pointers are validated by the x86 user-copy path.
- The kernel returns status in `fret_t` internally; the demo ring-3 path
  collapses errors to `-1`.
- There is no ABI version field, feature negotiation or structure-size
  negotiation.
- The verb table is internal; it is not a published user ABI.

## 4. Target native ABI

Finte should define its own native ABI before investing in compatibility
layers. The ABI specification must cover:

- syscall numbers and calling conventions per architecture;
- parameter validation and error reporting;
- user/kernel data-structure layout and alignment;
- Process and Thread semantics;
- Flow lifecycle operations;
- Capability operations;
- Channel operations;
- Arena mapping operations;
- time and clock interfaces;
- file and device interfaces;
- ABI versioning and compatibility guarantees.

A stable ABI is a prerequisite for a durable native userland.

## 5. Target error contract

The native ABI should preserve the separation between status and value:

- success is explicit;
- errors carry a stable status code;
- a value is never overloaded as an error sentinel unless the ABI says so;
- partial progress is reported explicitly;
- `-1`-style collapsing belongs only in compatibility layers.

The current `fret_t` is a useful internal foundation, but its layout and
lifetime rules must be fixed before userland depends on it.

## 6. Handle and pointer rules

- Userland receives capability-table indices, not authority-bearing handles.
- A raw node handle is diagnostic information, not authority.
- User pointers are validated against the process address space.
- Kernel pointers are never returned to userland unless explicitly part of a
  mapping contract.
- Structures passed across the boundary need fixed width, alignment and
  version/size fields.

## 7. Versioning

Target rules:

- every user-visible structure begins with a version and size;
- the kernel rejects unknown major versions and unsupported feature bits;
- additive changes preserve existing field offsets;
- breaking changes require a new ABI major version;
- the native ABI version is independent of the kernel release version;
- syscall numbers are never reused for different semantics.

The current kernel version (`0.0.1`) is not an ABI version. Boot info has a
`version` field; the verb table does not.

## 8. Linux persona

The Linux persona is an experimental compatibility layer:

- it translates a small set of Linux syscall numbers;
- it provides a path namespace above the core's capability model;
- it is not Linux compatibility;
- it must not shape the native ABI;
- it should remain optional.

A useful Linux compatibility environment would also need process behavior,
signals, threads, file descriptors, synchronization and networking semantics.

## 9. Invariants

1. A syscall never trusts a raw user handle as authority.
2. A failed capability check returns an explicit error.
3. User pointers are validated before access.
4. A syscall does not silently truncate an argument without documenting it.
5. A syscall number keeps its meaning for the life of an ABI major version.
6. Compatibility layers do not change native syscall semantics.
7. Structure layout is fixed and tested across architectures.

## 10. Current limitations

- No stable native user ABI.
- No ABI version negotiation.
- No native libc.
- No general process/thread syscall model.
- No file-descriptor table in the native ABI.
- The ring-3 demo uses `-1` for errors.
- No cross-architecture syscall conformance suite.
- The Linux persona is in-kernel and experimental.

## 11. Open questions

- Syscall ABI versioning and compatibility policy.
- The exact calling convention on x86_64 and arm64.
- Whether Threads are exposed as handles or only as Flow attributes.
- Process and signal semantics, if introduced.
- Native libc scope and runtime startup.
- Filesystem and file-handle model.
- How capabilities are passed in arguments and messages.
- Which compatibility layers are worth supporting after the native system is
  stable.

## 12. Test evidence

- `src/tests/test_main.c`: verb dispatch, capability and object behavior.
- `src/core/selftest.c`: syscall and ABI checks.
- `compat` shell command: Linux persona demonstration.
- `procs` / `services`: x86 ring-3 syscall path for IPC, time, sleep and Arena
  mapping.
- No native ABI version or cross-architecture user ABI test yet.
