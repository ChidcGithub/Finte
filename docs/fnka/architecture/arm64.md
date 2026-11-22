# arm64 port

> **Status:** Partial — builds, boots under QEMU `virt` and AAVMF, produces boot
> protocol v1, and has MMU/page tables, exception dispatch, GICv2, generic
> timer IRQs, PSCI CPU_ON, per-CPU state, 4-CPU scheduling, EL0 user processes
> with `svc #0`, and a legacy/modern virtio-mmio block driver. The user layer is
> the shared `src/core/process.c` core with fd/mmio/fork/exec/signal support and
> per-process ASIDs. It still lacks full POSIX process semantics, ACPI/DTB
> parsing and GICv3.
>
> **Scope:** AArch64-specific boot, CPU, memory and interrupt mechanisms.
> Shared semantics are in the concept and kernel documents.

## 1. Boot paths

### Direct ELF

```bash
make arm64-test
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 256M -smp 4 \
  -nographic -kernel build/finte-arm64.elf
```

- The ELF is linked at `0x40080000`.
- `_start` preserves the boot arguments, reads `MPIDR_EL1`, parks secondary
  CPUs, installs the vector table, sets the stack, zeroes `.bss` and calls
  `kmain_arm64(boot_arg, magic)`.
- The kernel uses PL011 at `0x09000000`, the generic counter, GICv2 at
  `0x08000000`/`0x08010000`, and PSCI over HVC.
- An arm64 initrd (`init.elf`, `ipc.elf`, plus the ramdisk files) is embedded in
  the kernel image with `tools/mkbinarray.py`, so both direct and UEFI boots
  have the user demo without a loader ramdisk module.
- No Device Tree is parsed. The platform is hard-coded QEMU `virt`; the
  direct-boot path has no boot memory map and uses the static-heap fallback.

### UEFI

`src/uefi/loader_arm64.c`:

1. Opens the ESP with `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`.
2. Reads `\EFI\FINTE\KERNEL.ELF`.
3. Validates the ELF64/AArch64 header and program headers.
4. Allocates the kernel's physical range at its link address (`0x40080000`).
5. Zeroes the allocation and copies the `PT_LOAD` segments.
6. Cleans the loaded image to the point of coherency (`CTR_EL0` line sizes,
   `dc civac`, `dsb ish`) and invalidates the instruction cache (`ic ivau` or
   `ic iallu` when `CTR_EL0.DIC` is set).
7. Converts the UEFI memory map, captures ACPI/Device Tree pointers from the
   configuration table and GOP framebuffer geometry, and fills `fboot_info`.
8. Calls `ExitBootServices`.
9. Disables interrupts and branches to the ELF entry point with the structure
   in `x0` and `FINTE_BOOT_MAGIC` in `x1`.

The loader uses `EFI_LOADER_CODE` for the kernel allocation, which is required
for executable permissions under AAVMF. AAVMF currently exposes ACPI but not a
Device Tree table in the tested configuration; the protocol carries either.

### ISO and IMG

`make images` builds:

- `finte-arm64.img`: an MBR image with a 16 MiB FAT16 ESP containing
  `EFI/BOOT/BOOTAA64.EFI` and `EFI/FINTE/KERNEL.ELF`;
- `finte-arm64.iso`: an El Torito EFI image using the same FAT16 ESP.

Both require UEFI firmware. The local test uses AAVMF:

```bash
cp /usr/share/AAVMF/AAVMF_VARS.fd build/AAVMF_VARS.fd
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 512M -nographic \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,unit=1,file=build/AAVMF_VARS.fd \
  -cdrom build/finte-arm64.iso
```

## 2. CPU, exceptions and interrupts

- Runs at EL1; `VBAR_EL1` points to the 2 KiB vector table.
- IRQ/SERROR/FIQ/sync entries save a 288-byte frame (x0–x30, ELR, SPSR, ESR,
  FAR, SP_EL0), call C, and `eret` through the same frame.
- EL0 uses the lower-EL AArch64 vector slots. SVC (`EC=0x15`) dispatches the
  native syscall table with `x8` as the number and `x0`–`x2` as arguments;
  SVC's preferred return address already points past the instruction. Data,
  instruction, alignment and BRK faults from EL0 call `finte_user_fault()`,
  which kills the process (the expected `procs` fault is contained). EL1 sync
  faults print ESR/FAR/ELR/SPSR and panic.
- `SP_EL0` is saved in both the exception frame and the context-switch frame so
  blocking syscalls and timer preemption preserve the user stack pointer.
- GICv2 distributor/CPU interface: Group 0 control, priority `0xA0`, SGI 0 for
  reschedule IPIs, PPI 30 for the EL1 physical timer.
- The generic timer runs at 100 Hz; the handler rearms `CNTP_TVAL_EL0`, EOIs,
  ticks, then preempts the current Thread if the scheduler is active.
- PSCI HVC: `SYSTEM_OFF` and `CPU_ON` (`0x84000003`). APs enter
  `arm64_ap_entry`, load the BSP MMU state, run `arm64_ap_main`, initialize their
  GIC CPU interface and timer, and join `finte_sched_ap_run()`.
- Before `CPU_ON`, the BSP cleans the page tables, MMU globals, and AP stacks to
  the point of coherency with `CTR_EL0`-based `dc civac`/`dsb`; APs invalidate
  their I-cache after enabling the MMU.
- TPIDR_EL1 holds the per-CPU `fcpu_t`; scheduler contexts and active contexts
  are per-CPU arrays.

## 3. Memory

- Kernel linked at `0x40080000`; 4 GiB identity map with 2 MiB blocks.
- 48-bit VA (L0 root) or 39-bit VA (L1 root), selected from the firmware TCR.
  RAM is Normal WB; GIC/PL011/virtio MMIO is Device-nGnRnE.
- PMM consumes the UEFI boot memory map; direct boot uses the 8 MiB static
  heap fallback.
- 32 KiB Flow stacks, 16 KiB static AP stacks, PMM-backed pages on UEFI.
- User processes live at 4–5 GiB. Each process root copies the supervisor-only
  kernel L1 entries, replaces `L1[4]` with a private L2 table, and allocates L3
  tables on demand for 4 KiB user pages. `TTBR0_EL1` switches per process; the
  kernel stays mapped through the shared low identity map.
- The kernel allocates/tracks process page tables and user pages from the PMM
  (or static heap). The address space is refcounted and Flow-owned; flow death
  queues it for release after switching to the scheduler root, so page tables
  are freed even if `proc_destroy()` is not called.
- An 8-bit ASID allocator reserves 0 for the kernel and tags `TTBR0_EL1` with
  the process ASID. Activation invalidates the incoming ASID; teardown flushes
  the ASID before freeing page tables. ASID rollover broadcasts
  `tlbi vmalle1is`.
- User pages use AP[1]=EL0, PXN, and per-mapping UXN; code is RX, data/stack/
  shared/arena/mmap pages are RW-NX. Read-only code rejects write translation.

## 4. Current capabilities

| Capability | Status |
| --- | --- |
| PL011 console | Implemented |
| Generic counter time | Implemented |
| PSCI shutdown | Implemented |
| Exception vectors | Implemented (user sync faults, IRQ/SVC return) |
| MMU / page tables | Implemented (identity kernel + per-process user roots) |
| GIC / interrupts | Implemented (GICv2) |
| Timer interrupt | Implemented (100 Hz, PPI 30) |
| IPI | Implemented (SGI 0) |
| SMP startup | Implemented (PSCI CPU_ON, 4 CPUs) |
| User mode | Implemented (EL0, `svc #0`, shared process ABI) |
| User address space | Implemented (4–5 GiB, per-process TTBR0 + ASID) |
| Process ABI | Partial (fd table, mmap, COW fork, waitpid, exec, SIGUSR1, cap transfer, service registry, ET_DYN load) |
| Block devices | Implemented (virtio-mmio legacy + modern) |
| ACPI / Device Tree parsing | Missing (pointers passed only) |
| GICv3 | Missing |

## 5. Next priorities

1. Extend the user ABI with a fuller signal set, file-backed mappings and
   general file descriptors for the Linux persona.
2. ACPI/DTB parsing for platform discovery.
3. GICv3 support if a target requires it.
4. Virtio IRQ/feature work and a reusable DMA/block framework.
5. Cache-maintenance validation for AP startup on real silicon.
6. Physical-hardware qualification.

Do not claim cross-architecture parity until the corresponding tests exist.

## 6. Known gaps

- Fork shares pages read-only and copies on the first write (COW). Anonymous
  mappings are capped at 64 pages per mapping. Signals cover SIGUSR1 delivery
  and SIGKILL only.
- There is no job control, per-process signal mask or general Linux fd table.
- Address-space teardown is Flow-owned; `proc_destroy()` only frees the process
  object. A fork child that is still live when its parent is destroyed is
  aborted and left for flow death rather than synchronously reaped.
- The x86 process ABI suite is pinned to the BSP; arm64 runs the same suite on
  4 CPUs. Simultaneous fork/sleep/migration is not stress-tested.
- Virtio-mmio is polled, single-queue and QEMU-focused; there is no generic
  block IRQ/DMA framework.
- QEMU `virt` assumptions are hard-coded in the HAL.
- ACPI/DTB are passed but not parsed.
- GICv2 only.
- No physical hardware qualification.
- Static-heap fallback remains for direct `-kernel` boot.

## 7. Test evidence

- `make arm64-test`: QEMU `virt`, 4 CPUs, MMU identity translations, GICv2
  timer ticks, SMP flow distribution, `init.elf` at EL0, the full
  `procs` IPC/shared-memory/arena/time/preemption/fault demo, and the shared
  process ABI suite (fd table, mmap, COW fork, waitpid, exec, signal, cap transfer,
  service registry, ET_DYN load, churn, blocked-flow abort) plus 147 core
  checks, PASS.
- `make arm64-uefi-test`: AAVMF ISO, boot protocol v1, PMM, MMU, GICv2, timer
  ticks, 4 CPUs, `hits=2/2/2/2`, `init.elf`, `procs` and the full ABI suite,
  PASS.
- `make arm64-disk-test`: direct boot with a virtio-mmio `disk-test.img` in
  both legacy and modern transports; mounts `hd0p1` lfc1, `hd0p2` ext4 and
  `hd0p3` NTFS and reads their files, PASS.
- `make arm64-uefi-disk-test`: the same disk checks through the AAVMF ISO,
  PASS.
- `make images`: arm64 IMG and ISO built; both boot under AAVMF.
- Direct and UEFI boots report `mmu: 48-bit identity map enabled`,
  `arm64 timer: gic=v2 ticks=9`, `arm64 smp: 3 application processor(s)
  online, 4 cpu(s) total`, `arm64 user: init.elf PASS` and
  `arm64 procs: PASS`.
