# Finte ABI v0.0.0

The ABI has one purpose: make authority and data movement explicit without turning every operation into a path lookup or an `ioctl` multiplexer.

## Handles

```c
typedef uint64_t fhandle_t;
```

Layout:

```
bits 63..32   generation
bits 31..0    slot + 1
```

- `0` is invalid.
- `finte_handle_make(slot, gen)` constructs a handle.
- A stale handle fails `finte_node_get` because the generation no longer matches.
- Handles are not capabilities. A flow that somehow obtains a handle value still cannot operate on it without a cap.

## Status and value

```c
typedef struct fret {
    int32_t status;    // 0 or negative FINTE_ERR_*
    uint32_t detail;   // reserved
    uint64_t value;    // result, or 0 on error
} fret_t;
```

There is no global `errno`. Status values:

| status | value | meaning |
| --- | ---: | --- |
| `FINTE_OK` | 0 | success |
| `FINTE_ERR_INVAL` | -1 | bad argument |
| `FINTE_ERR_NOENT` | -2 | no such object/cap |
| `FINTE_ERR_NOMEM` | -3 | out of memory/table space |
| `FINTE_ERR_PERM` | -4 | capability lacks the required right |
| `FINTE_ERR_BUSY` | -5 | object busy |
| `FINTE_ERR_AGAIN` | -6 | would block (nonblocking call) |
| `FINTE_ERR_LIMIT` | -7 | table limit reached |
| `FINTE_ERR_IO` | -8 | I/O failure |
| `FINTE_ERR_DEADLOCK` | -9 | all flows blocked, no sleepers |
| `FINTE_ERR_NOSYS` | -10 | verb not implemented |
| `FINTE_ERR_FAULT` | -11 | invalid pointer |
| `FINTE_ERR_RANGE` | -12 | offset/size out of range |
| `FINTE_ERR_EXIST` | -13 | name already exists |
| `FINTE_ERR_TYPE` | -14 | node type does not match the verb |
| `FINTE_ERR_STATE` | -15 | operation invalid in this state |
| `FINTE_ERR_EOF` | -16 | channel is closed and drained |

`FINTE_ERR_*` constants are already negative. Functions that return an `int` use the same negative values on failure and `>= 0` on success (a cap index or a count).

## Capability rights

```c
#define FINTE_R_READ    (1u << 0)
#define FINTE_R_WRITE   (1u << 1)
#define FINTE_R_SEND    (1u << 2)
#define FINTE_R_RECV    (1u << 3)
#define FINTE_R_EXEC    (1u << 4)   // reserved
#define FINTE_R_GRANT   (1u << 5)
#define FINTE_R_CONTROL (1u << 6)   // FLOW_WAIT, CHAN_CLOSE
#define FINTE_R_SIGNAL  (1u << 7)   // reserved
```

A cap with rights `0` is legal and useful for tests: it proves that possessing a reference is not the same as possessing authority.

## Verbs

```c
fret_t finte_syscall(fflow_t *flow, uint32_t verb, const uint64_t args[6]);
```

| # | verb | args | result |
| ---: | --- | --- | --- |
| 0 | `NOP` | — | 0 |
| 1 | `SYS_INFO` | — | `major<<16 \| minor<<8 \| patch` |
| 2 | `CAP_INFO` | `cap`, `out*` | 0; fills `fcap_info_t` |
| 3 | `CAP_DUP` | `cap`, `rights` | new cap index |
| 4 | `CAP_DROP` | `cap` | 0 |
| 5 | `CAP_GRANT` | `flow_cap`, `src_cap`, `rights` | cap index in target flow |
| 6 | `CHAN_SEND` | `cap`, `msg*`, `flags` | 0 |
| 7 | `CHAN_RECV` | `cap`, `msg*`, `flags` | 0 |
| 8 | `CHAN_INFO` | `cap`, `out*` | 0; fills `fchan_info_t` |
| 9 | `FLOW_YIELD` | — | 0 (after resume) |
| 10 | `FLOW_EXIT` | `code` | does not return |
| 11 | `FLOW_INFO` | `out*` | 0; fills `fflow_info_t` |
| 12 | `FLOW_FUEL` | `out*` | 0; writes remaining fuel |
| 13 | `ARENA_ALLOC` | `cap`, `size`, `align`, `out*` | 0; writes offset |
| 14 | `ARENA_FREE` | `cap`, `offset` | 0 |
| 15 | `CONSOLE_WRITE` | `cap`, `buf`, `len` | bytes written |
| 16 | `FILE_READ` | `cap`, `offset`, `buf`, `len` | bytes read |
| 17 | `CLOCK_NOW` | `out*` | 0; writes nanoseconds |
| 18 | `SYS_STATS` | `out*` | 0; fills `fstats_t` |
| 19 | `SYS_DUMP` | — | 0 (prints to console) |
| 20 | `FLOW_WAIT` | `flow_cap` | 0 after target exit |
| 21 | `CHAN_CLOSE` | `cap` | 0; remaining messages drain, then `EOF` |
| 22 | `FILE_WRITE` | `cap`, `offset`, `buf`, `len` | bytes written |

### Required rights

| verb | required right | node type |
| --- | --- | --- |
| `CHAN_SEND` | `SEND` | channel |
| `CHAN_RECV` | `RECV` | channel |
| `CHAN_INFO` | any valid cap | channel |
| `CONSOLE_WRITE` | `WRITE` | console |
| `FILE_READ` | `READ` | file |
| `FILE_WRITE` | `WRITE` | file |
| `ARENA_ALLOC/FREE` | `WRITE` | arena; scoped caps stay inside their page range |
| `CAP_GRANT` (on target) | `GRANT` | flow |
| `FLOW_WAIT` | `CONTROL` | flow |
| `CHAN_CLOSE` | `CONTROL` | channel |
| `CAP_DUP` | any valid cap | any |

A grant may not add rights the source cap does not have. `CAP_DUP` follows the same rule.

Arena caps may be **page-scoped**: the core installs one with a page-aligned
`offset` and `length`, and `CAP_INFO` reports `FINTE_CAP_SCOPED` plus the
window. Scoped `ARENA_ALLOC` stays inside that window; a whole-arena cap
cannot allocate or free inside it.

## Structures

```c
typedef struct fcap_info {
    uint32_t type;
    uint32_t rights;
    uint64_t handle;
    uint64_t aux;
    uint64_t offset;   /* scoped arena window */
    uint64_t length;
    uint32_t flags;    /* FINTE_CAP_SCOPED */
    uint32_t pad;
} fcap_info_t;

typedef struct fflow_info {
    uint64_t handle;
    uint32_t state;
    uint32_t persona;
    uint32_t fuel;
    uint32_t fuel_max;
    uint32_t caps;
    uint32_t pad;
    char name[24];
} fflow_info_t;

typedef struct fchan_info {
    uint64_t handle;
    uint32_t capacity;
    uint32_t count;
    uint32_t closed;
    uint32_t pad;
    uint64_t sent;
    uint64_t received;
    uint64_t waiting_send;
    uint64_t waiting_recv;
    char name[24];
} fchan_info_t;
```

`fstats_t` contains global counters (syscalls, flows, context switches, messages, caps, fuel, epochs, console bytes, deadlocks, live objects). `fflow_stats_t` contains the per-flow subset.

## Messages

```c
typedef struct fmsg {
    uint32_t tag;
    uint16_t len;          // 0..32
    uint8_t cap_count;     // 0..2
    uint8_t flags;
    uint16_t caps[2];      // sender-local cap indices
    uint32_t reserved;
    uint8_t data[32];
} fmsg_t;
```

- `flags` currently only carries `FINTE_MSG_NONBLOCK` for the operation.
- `len` is validated against 32.
- `cap_count` is validated against 2.
- On send, each referenced cap is validated and the queue takes a duplicate reference. The sender keeps its cap.
- On receive, each queued cap is installed into the receiver's table and `out->caps[i]` is replaced with the receiver-local index.
- If the receiver has fewer than `cap_count` free slots, receive returns `FINTE_ERR_NOMEM` without consuming the message.
- Data bytes are copied into the 48-byte ring entry. Bulk data should use an arena cap.

## Blocking and nonblocking

- `fx_send`, `fx_recv`, `fx_send_msg`, `fx_recv_msg` block.
- `fx_try_send`, `fx_try_recv` set `FINTE_MSG_NONBLOCK` and return `FINTE_ERR_AGAIN` when the operation would block.
- Blocking is cooperative: the flow is marked `BLOCKED`, placed on the channel's wait list, and switches to the scheduler.
- A successful send/receive wakes one waiter.
- `CHAN_CLOSE` marks the channel closed and wakes every waiter. Already-queued messages can still be received; once the ring is empty, receive returns `FINTE_ERR_EOF`. Send returns `FINTE_ERR_STATE`.
- If all flows are blocked and there are no sleepers, the scheduler returns `FINTE_ERR_DEADLOCK`.

## Flow and scheduling ABI

`finte_flow_create(name, program, fuel, persona)` creates a flow with an empty cap table. `finte_flow_start(handle)` allocates the stack/context, takes a scheduler reference, and enqueues it.

- Default quantum: `FINTE_FUEL_QUANTUM` (24).
- `fuel_max` is the refill amount at the start of an epoch.
- Priority 0 is the latency band; priority 1 is the batch band.
- `FLOW_YIELD` returns after the scheduler runs the flow again.
- `FLOW_EXIT` drops all caps and never returns.
- `FLOW_WAIT` blocks until the target flow exits. It requires a `CONTROL` cap,
  returns immediately if the target is already dead, and rejects a target that
  has not started. It is a join, not a process wait: no pid, no signal state.

The kernel keeps the flow's program image and stack out of the application ABI; userspace programs will eventually refer to program IDs rather than function pointers.

## Pointers and copy-in/out

Native flows trust pointers passed to verbs because they share the kernel
address space. `NULL` is rejected where a buffer is required. The ring-3 demo
uses a separate minimal gate that range-checks pointers against the 64 MiB user
limit; a general user ABI still needs per-flow address spaces and a full
copy-in/copy-out user-access layer. The verb numbers and structures stay the
same.

## Example: a flow

```c
static void worker(fflow_t *f)
{
    fmsg_t msg;
    fret_t r = fx_recv(f, 0, &msg);          // cap 0: channel RECV
    if (finte_failed(r)) {
        return;
    }
    uint64_t value;
    finte_memcpy(&value, msg.data, sizeof(value));

    fmsg_t out = { .tag = 0x1234, .len = sizeof(value) };
    finte_memcpy(out.data, &value, sizeof(value));
    fx_send(f, 1, &out);                     // cap 1: channel SEND
}
```

Wiring is explicit:

```c
fhandle_t ch = finte_channel_create("work", 8);
fhandle_t fh = finte_flow_create("worker", &worker_program, 4096,
                                 FINTE_PERSONA_NATIVE);
fflow_t *f = finte_flow_lookup(fh);
finte_cap_install(f, ch, FINTE_R_RECV);
finte_flow_start(fh);
```

There is no `fork`, no inherited fd, and no ambient permission.

## Compatibility contract

The native ABI is versioned with `FINTE_VERSION_*`. The Linux personality has its own syscall-number contract and maps errors to Linux errno values; see [compat.md](compat.md). Adding a native verb is a core ABI change; adding a Linux syscall is a persona change.
