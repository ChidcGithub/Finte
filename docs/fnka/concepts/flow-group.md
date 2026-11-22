# Flow Group contract

> **Status:** Proposed — no Flow Group object exists. The first implementation
> does not require one.
>
> **Normative for:** the optional organization layer for related Flows. It is
> not an authority mechanism.

## 1. Definition

A **Flow Group** is an optional management abstraction for related Flows. It
groups Flows for:

- group-level lifecycle management;
- aggregate resource accounting;
- budget distribution or limits;
- coordinated shutdown;
- service-level observability and statistics.

A Flow Group must not implicitly grant its members access to one another's
capabilities, Channels, Arenas or memory. Membership is an organizational
relationship, not an authority grant.

## 2. Why it is optional

Flow Groups are not needed for:

- one Flow per service;
- a single init/supervisor;
- basic Channel and Arena operation;
- the first native ABI.

They become useful when a service has several Flows that should start, stop,
account or fail together.

## 3. Proposed model

| Field | Meaning |
| --- | --- |
| `group_id` | Stable identity |
| `name` | Diagnostic name |
| `state` | Creating, running, stopping, stopped, failed |
| `parent` | Optional parent group |
| `members` | Flows belonging to the group |
| `fuel_limit` | Optional aggregate Fuel limit |
| `fuel_used` | Aggregate accounting |
| `exit_policy` | What happens when one member fails |
| `stats` | Group-level counters |

Potential operations:

- create/destroy a group;
- add/remove a member;
- start/stop all members;
- wait for group completion;
- read aggregate statistics;
- configure failure policy.

## 4. Invariants

1. Group membership does not create capabilities.
2. Removing a Flow from a group does not revoke its capabilities.
3. A group cannot outlive a member that still holds group-owned resources
   without an explicit transfer or teardown rule.
4. Group-level Fuel limits never create additional Fuel.
5. A member's failure policy is explicit; it is not inferred from group
   membership alone.
6. Destroying a group does not silently destroy unrelated Flows.

## 5. Open questions

- Whether Flow Groups are kernel objects or a userland service convention.
- How group-level Fuel is divided among members.
- Whether groups can nest.
- How coordinated shutdown waits for active Threads.
- Whether a group can own Channels or Arenas, or only reference them.
- How group failure policy interacts with Flow fault policy.
- Whether Flow Groups belong in the first native ABI.

## 6. Current status

Not implemented. The current kernel has Flow-level objects only. A userland
supervisor can approximate a group by holding Flow capabilities and waiting
for them, but there is no kernel-enforced group accounting or lifecycle.
