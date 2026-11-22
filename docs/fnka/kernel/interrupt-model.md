# Interrupt and exception model

> **Status:** Partial — x86_64 has an IDT, exceptions, PIT/LAPIC timers, IPIs
> and UEFI firmware-IDT switching. arm64 has an exception vector table but no
> GIC, timer interrupt, IPI or user-mode exception delivery.
>
> **Normative for:** exception entry, interrupt dispatch, timer and IPI
> mechanisms, and the boundary between the core and platform interrupt
> controllers.
>
> **Related:** [Scheduler](scheduler.md), [x86_64](../architecture/x86_64.md),
> [arm64](../architecture/arm64.md), [Driver model](../drivers/driver-model.md).

## 1. Current x86_64 model

### Exceptions

- The IDT is installed by the x86 fault/IRQ setup.
- Exception entry saves the interrupted context and calls the C fault handler.
- User faults are recognized by CS/DPL and routed to `finte_user_fault()`,
  which kills the user process and records a watchdog strike.
- Kernel faults panic.
- The 3-strike watchdog panics and powers off after repeated abnormal events.

### Interrupts

- The 8259 PIC is remapped and IRQ0 is routed to the timer vector.
- The PIT runs at 100 Hz on the BSP.
- APs use calibrated LAPIC timers at the same vector, with TSC-deadline mode
  when available and a periodic fallback.
- The timer IRQ increments per-CPU ticks, services sleepers and may preempt
  the running Flow.
- `hal_fw_begin()` / `hal_fw_end()` temporarily switch to the firmware IDT so
  UEFI Boot Services calls can run. This exists because the x86 UEFI loader
  does not call `ExitBootServices`.

### IPIs

- `hal_cpu_wake()` sends a wake IPI to a parked CPU.
- IPIs are used when a scheduler batch is active and a CPU has queued work.
- A wake targeting a running CPU waits for its next yield or timer tick.

### Interrupt state

- Flows run with interrupts enabled.
- Scheduler contexts run with interrupts disabled.
- `hal_irq_save()` / `hal_irq_restore()` preserve the caller's state around
  critical sections.
- The global core lock and per-CPU run-queue locks disable interrupts while
  held.

## 2. Current arm64 model

- `src/arch/arm64/entry.S` installs a 16-entry exception vector table in
  `VBAR_EL1`.
- Every vector currently branches to `arm64_fault`, which reports the fault and
  parks the CPU.
- `hal_irq_disable()` / `hal_irq_enable()` manipulate `DAIF`.
- The generic counter is readable (`CNTPCT_EL0`, `CNTFRQ_EL0`), but no timer
  compare interrupt is programmed.
- There is no GIC initialization, no IRQ dispatch and no IPI.
- PSCI `SYSTEM_OFF` is used for shutdown.
- The UEFI loader calls `ExitBootServices` and disables interrupts before
  jumping to the kernel.

arm64 therefore has exception vectors but not a usable interrupt subsystem.

## 3. Target model

The target interrupt core should provide:

- architecture-independent IRQ registration and dispatch;
- per-CPU interrupt state;
- timer and IPI mechanisms;
- interrupt affinity and masking;
- a way to grant an IRQ capability to a user-space driver;
- acknowledgement/EOI ownership rules;
- MSI/MSI-X support where the platform provides it;
- safe teardown when a driver or device is removed.

The HAL should expose CPU-local interrupt mechanisms. The platform layer should
own controller discovery and configuration:

- x86_64: IDT, LAPIC, IOAPIC, MSI.
- arm64: exception vectors, GICv2/GICv3, PPI/SPI routing, MSI.

## 4. Invariants

1. An interrupt handler does not run on a CPU that has not initialized its
   local interrupt state.
2. A timer interrupt cannot preempt a context that is not scheduler-safe.
3. An EOI is issued exactly once per acknowledged interrupt.
4. A masked interrupt is not delivered.
5. A user driver cannot receive an IRQ without a capability for it.
6. An interrupt aimed at a destroyed device or driver is quiesced before
   teardown.
7. Exception entry preserves enough state to resume or terminate safely.
8. A kernel fault never returns to the faulting instruction as if successful.

## 5. Current limitations

- arm64 has no GIC, timer IRQ or IPI.
- No user-space IRQ delivery.
- No IRQ capabilities.
- No MSI/MSI-X framework.
- No interrupt affinity policy beyond per-CPU timers.
- x86 keeps the firmware IDT switch path because UEFI services remain in use.
- No formal nesting/reentrancy policy beyond interrupt disable windows.
- No interrupt storm accounting or rate limiting.

## 6. Open questions

- The common interrupt-core interface and per-architecture implementation.
- Whether IRQ handlers run on the interrupting CPU only or can be migrated.
- How IRQ capabilities are represented and revoked.
- How user-space drivers acknowledge and complete interrupts.
- How timer interrupts interact with Thread states and Fuel accounting.
- How to handle shared interrupts and level/edge semantics.
- How to quiesce DMA and interrupts during device removal.
- What MSI support is required before user-space drivers.

## 7. Test evidence

- x86 `make qemu-test` / `make qemu-uefi-test`: timer, preemption and shell.
- `preempt`: timer preemption against a spinning Flow.
- `smp`: AP startup, per-CPU timers and IPIs.
- `make qemu-disk-test`: ATA/AHCI interrupt-driven or polled block I/O paths.
- arm64 `make arm64-test`: exception vectors and core checks; no timer IRQ,
  GIC or IPI coverage.
