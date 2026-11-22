# Channel contract

> **Status:** Partial — a fixed-capacity message queue exists with blocking and
> non-blocking operations. Send and receive authority are rights on one object,
> not separate endpoints. Message capabilities are copied/shared, not moved.
>
> **Normative for:** Channel endpoints, message layout, queue behavior,
> blocking, close/EOF, capability attachment and ownership.
>
> **Related:** [Capability](capability.md), [Arena](arena.md),
> [Flow](flow.md), [Kernel boundary](../kernel/kernel-boundary.md).

## Resolved P0 decisions (2026-11-22)

1. **v0.1 keeps one Channel object with per-direction rights.** `SEND`, `RECV`
   and `CONTROL` are rights on the same node. Separate send/receive endpoint
   objects are a v0.2 native-ABI change, not a silent semantic change.
2. **Message data is copied; attached capabilities are shared references.**
   The sender keeps its capability. The receiver gets copies installed in its
   own table. There is no move/transfer operation in v0.1.
3. **Queue capacity is fixed and bounded.** Capacity is clamped to
   `1..FINTE_CHAN_CAP` (32). Full queues block or return `FINTE_ERR_AGAIN` for
   non-blocking sends.
4. **Close is full-close, not half-close.** A successful close wakes all
   waiters; sends fail with `FINTE_ERR_STATE`; receives drain queued messages
   and then return `FINTE_ERR_EOF`.
5. **Queued messages are owned by the Channel.** The Channel retains attached
   node references until delivery or destruction. A sender exit does not
   invalidate queued messages.
6. **There is no timeout or priority wake policy in v0.1.** Waiters are FIFO
   and one waiter is woken per event.
7. **Memory ordering is provided by the global core lock.** The Channel is not
   a lock-free ring in v0.1. Cross-architecture ordering guarantees follow the
   lock and the platform memory model.

## 1. Definition

A **Channel** is a capability-controlled communication mechanism between
components. It carries small fixed-size messages and optional capability
attachments. Bulk data belongs in an Arena; the Channel should carry the
authority to the Arena, not the bulk bytes.

## 2. Current object model

`fchan_t` in `src/include/finte/object.h` contains:

| Field | Meaning |
| --- | --- |
| `capacity`, `count`, `head`, `tail` | Fixed ring state |
| `closed` | Close flag |
| `self` | Node handle |
| `wait_send`, `wait_recv` | Wait lists |
| `sent`, `received` | Counters |
| `ring[]` | `fmsg_t` entries |
| `qcaps[][]`, `qcap_count[]` | Attached capability entries per message |
| `name` | Diagnostic name |

`finte_channel_create(name, capacity)` clamps capacity to `1..FINTE_CHAN_CAP`
(32) and allocates a node.

### Current endpoint model

There is one Channel object. A Flow obtains a capability for it with any subset
of rights:

- `FINTE_R_SEND` — may send;
- `FINTE_R_RECV` — may receive;
- `FINTE_R_CONTROL` — may close.

A sender and receiver may hold different capabilities for the same object, but
there is no separate send-endpoint or receive-endpoint object. Revoking a
sender's capability does not revoke a receiver's capability.

## 3. Message layout

`fmsg_t` is fixed-size:

```c
typedef struct fmsg {
    uint32_t tag;
    uint16_t len;
    uint8_t cap_count;
    uint8_t flags;
    uint16_t caps[FINTE_MSG_CAPS];
    uint32_t reserved;
    uint8_t data[FINTE_MSG_DATA];
} fmsg_t;
```

- `FINTE_MSG_DATA` is 32 bytes.
- `FINTE_MSG_CAPS` is 2.
- `flags` currently supports `FINTE_MSG_NONBLOCK`, but blocking is normally
  selected by the verb/helper wrapper.
- `caps[]` are **capability-table indices in the sender's table**, not raw
  node handles.

## 4. Send semantics

`finte_chan_send(f, cap, msg, flags)`:

1. Validates the message length and attachment count.
2. Checks `FINTE_R_SEND` on the Channel capability.
3. Resolves every attached capability with `rights = 0` (any valid cap).
4. If the queue is full:
   - non-blocking returns `FINTE_ERR_AGAIN`;
   - blocking calls `finte_flow_block(f, ch, 1)`.
5. If the Channel is closed, returns `FINTE_ERR_STATE`.
6. Copies the message into the ring.
7. Takes a node reference for each attached capability and stores a copy of
   the capability entry.
8. Advances the tail, updates counters and wakes one receive waiter.

The sender keeps its own capabilities. Attachment is a **share/copy**, not a
move or transfer.

## 5. Receive semantics

`finte_chan_recv(f, cap, out, flags)`:

1. Checks `FINTE_R_RECV`.
2. If the queue is empty:
   - a closed Channel returns `FINTE_ERR_EOF`;
   - non-blocking returns `FINTE_ERR_AGAIN`;
   - blocking calls `finte_flow_block(f, ch, 0)`.
3. Checks that the receiver has enough free capability slots for the
   attachments. If not, returns `FINTE_ERR_NOMEM` and leaves the message
   queued.
4. Copies the message to `out`.
5. Installs copies of the attached capabilities in the receiver's table and
   replaces `out->caps[]` with the new local indices.
6. Releases the queue's node references.
7. Advances the head, updates counters and wakes one send waiter.

The receiver does not receive the sender's table indices; it receives its own
local indices.

## 6. Close and peer termination

`finte_chan_close(f, cap)` requires `FINTE_R_CONTROL` on the Channel. It:

- sets `closed = 1`;
- wakes all send and receive waiters;
- leaves already-queued messages in the ring.

After close:

- a send returns `FINTE_ERR_STATE`;
- a receive drains remaining messages, then returns `FINTE_ERR_EOF`.

When the Channel node is destroyed, `finte_chan_release_queue()` releases all
attached capability references for queued messages.

If a Flow exits, its capability table is dropped. The Channel remains alive
while other node references exist. Queued messages and their attachments are
owned by the Channel, not by the exiting sender.

## 7. Blocking and wakes

- Wait lists are per Channel and per direction.
- Waiters are appended FIFO and one waiter is woken per successful
  send/receive.
- The wake path uses the Flow `blocking` / `wake_pending` handshake so a wake
  that races a context switch does not run the same context on two CPUs.
- Send and receive operations are serialized by the global core lock; blocking
  verbs release it around the context switch.

There is no fairness guarantee beyond FIFO insertion order and no priority
inheritance.

## 8. Target endpoint model

The target contract separates endpoint authority:

```text
Channel object
 ├── send endpoint capability(ies)
 └── receive endpoint capability(ies)
```

Rules:

- Sending requires send-endpoint authority; receiving requires receive-endpoint
  authority.
- Neither side automatically receives full control over the Channel.
- Closing or revoking one endpoint does not silently grant authority to the
  other.
- Endpoint lifetime and peer-termination behavior are explicit.
- A capability attachment may be a copy, a move, or a reference; the operation
  must say which.

This is a security-model change, not a rename of the current rights.

## 9. Invariants

1. A message is either queued or owned by a receiver, never both.
2. A queued message's attached capabilities retain their nodes.
3. A receiver never gets a sender's local capability index as authority.
4. A closed Channel does not accept new sends.
5. A receive after close drains queued messages before EOF.
6. A blocked sender or receiver is woken at most once per event.
7. The same Flow context is never running on two CPUs because of a wake race.
8. Queue capacity is fixed and bounded.

## 10. Current limitations

- No separate send/receive endpoint objects.
- No move/transfer semantics for attached capabilities.
- No priority or deadline wake policy.
- No per-message timeout.
- No user-to-user Channel creation ABI beyond the current verbs.
- No formal memory-ordering contract beyond the global core lock.
- Close is one-shot; there is no half-close.

## 11. Remaining open questions (post-v0.1)

- Move/transfer semantics for attached capabilities.
- Separate send/receive endpoint objects and the v0.2 ABI change.
- Half-close and peer-termination behavior.
- Timeout and priority wake policy.
- How endpoint revocation affects queued messages and in-flight operations.
- Cross-architecture memory ordering if the ring becomes lock-free.
- Backpressure policy for capacities beyond the current 32-message ring.
- Reference semantics for payloads larger than the inline message.

## 12. Test evidence

- `src/tests/test_main.c`: send/receive, blocking, close, capability attachment.
- `src/core/selftest.c`: Channel behavior in the core suite.
- `ipc.elf` / `procs`: cross-process message passing and capability transfer.
- `demo`: producer/transform/sink pipeline.
- No test yet for endpoint revocation or move semantics.
