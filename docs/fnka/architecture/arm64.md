# arm64 port

> **Status:** Partial — builds, boots under QEMU `virt` and AAVMF, produces boot
> protocol v1, and now has MMU/page tables, exception dispatch, GICv2, generic
> timer IRQs, PSCI CPU_ON, per-CPU state and 4-CPU scheduling. It still lacks
> user mode, block devices, ACPI/DTB parsing and GICv3.
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
- No Device Tree is parsed. The platform is hard-coded QEMU `virt`; the
  direct-boot path has no boot memory map and uses the static-heap fallback.

### UEFI

`src/uefi/loader_arm64.c`:

1. Opens the ESP with `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`.
2. Reads `\EFI\FINTE\KERNEL.ELF`.
3. Validates the ELF64/AArch64 header and program headers.
4. Allocates the kernel's physical range at its link address (`0x40080000`).
5. Zeroes the allocation and copies the `PT_LOAD` segments.
6. Flushes the instruction cache.
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
  FAR), call C, and `eret` through the same frame.
- Synchronous exceptions print ESR/FAR/ELR/SPSR and panic. SVC/user mode is not
  implemented yet.
- GICv2 distributor/CPU interface: Group 0 control, priority `0xA0`, SGI 0 for
  reschedule IPIs, PPI 30 for the EL1 physical timer.
- The generic timer runs at 100 Hz; the handler rearms `CNTP_TVAL_EL0`, EOIs,
  ticks, then preempts the current Thread if the scheduler is active.
- PSCI HVC: `SYSTEM_OFF` and `CPU_ON` (`0x84000003`). APs enter
  `arm64_ap_entry`, load the BSP MMU state, run `arm64_ap_main`, initialize their
  GIC CPU interface, and join `finte_sched_ap_run()`.
- TPIDR_EL1 holds the per-CPU `fcpu_t`; scheduler contexts and active contexts
  are per-CPU arrays.

## 3. Memory

- Kernel linked at `0x40080000`; 4 GiB identity map with 2 MiB blocks.
- 48-bit VA (L0 root) or 39-bit VA (L1 root), selected from the firmware TCR.
  RAM is Normal WB; GIC/PL011/virtio MMIO is Device-nGnRnE.
- PMM consumes the UEFI boot memory map; direct boot uses the 8 MiB static
  heap fallback.
- 32 KiB Flow stacks, 16 KiB static AP stacks, PMM-backed pages on UEFI.
- No arm64 user address space, ASID allocator or TLB-shootdown protocol yet.

## 4. Current capabilities

| Capability | Status |
| --- | --- |
| PL011 console | Implemented |
| Generic counter time | Implemented |
| PSCI shutdown | Implemented |
| Exception vectors | Implemented (sync panic, IRQ return) |
| MMU / page tables | Implemented (identity map) |
| GIC / interrupts | Implemented (GICv2) |
| Timer interrupt | Implemented (100 Hz, PPI 30) |
| IPI | Implemented (SGI 0) |
| SMP startup | Implemented (PSCI CPU_ON, 4 CPUs) |
| User mode | Missing |
| Block devices | Missing |
| ACPI / Device Tree parsing | Missing (pointers passed only) |

## 5. Next priorities

1. User mode and syscall entry (EL0, SVC, fault policy).
2. Block device support and a real root filesystem.
3. ACPI/DTB parsing for platform discovery.
4. GICv3 support if a target requires it.
5. Cache-maintenance validation for AP startup on real silicon.
6. Physical-hardware qualification.

Do not claim cross-architecture parity until the corresponding tests exist.

## 6. Known gaps

- No user mode, syscalls, processes or Arena user mapping.
- No block device or filesystem service.
- QEMU `virt` assumptions are hard-coded in the HAL.
- ACPI/DTB are passed but not parsed.
- GICv2 only.
- No physical hardware qualification.
- Static-heap fallback remains for direct `-kernel` boot.

## 7. Test evidence

- `make arm64-test`: QEMU `virt`, 4 CPUs, MMU/timer/SMP checks and 147 core
  checks, PASS.
- `make arm64-uefi-test`: AAVMF ISO, boot protocol v1, PMM, MMU, GICv2, timer
  ticks, 4 CPUs and `hits=2/2/2/2` flow distribution, PASS.
- `make images`: arm64 IMG and ISO built; both boot under AAVMF.
- Direct and UEFI boots report `mmu: 48-bit identity map enabled`,
  `arm64 timer: gic=v2 ticks=9` and `arm64 smp: 3 application processor(s)
  online, 4 cpu(s) total`.
- No interrupt, SMP, MMU or user-mode test yet.
