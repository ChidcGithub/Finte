# arm64 port

> **Status:** Partial — builds, boots under QEMU `virt`, passes the 147 core
> checks and has UEFI IMG/ISO media. It has no MMU, GIC, timer interrupt, SMP
> or user mode.
>
> **Scope:** AArch64-specific boot, CPU, memory and interrupt mechanisms.
> Shared semantics are in the concept and kernel documents.

## 1. Boot paths

### Direct ELF

```bash
make arm64-test
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 256M -smp 1 \
  -nographic -kernel build/finte-arm64.elf
```

- The ELF is linked at `0x40080000`.
- `_start` reads `MPIDR_EL1`, parks secondary CPUs, installs the vector table,
  sets the stack, zeroes `.bss` and calls `kmain_arm64()`.
- The kernel uses PL011 at `0x09000000`, the generic counter and PSCI
  `SYSTEM_OFF`.
- No Device Tree is parsed. The platform is hard-coded QEMU `virt`.

### UEFI

`src/uefi/loader_arm64.c`:

1. Opens the ESP with `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`.
2. Reads `\EFI\FINTE\KERNEL.ELF`.
3. Validates the ELF64/AArch64 header and program headers.
4. Allocates the kernel's physical range at its link address (`0x40080000`).
5. Zeroes the allocation and copies the `PT_LOAD` segments.
6. Flushes the instruction cache.
7. Calls `ExitBootServices`.
8. Disables interrupts and branches to the ELF entry point.

The loader uses `EFI_LOADER_CODE` for the kernel allocation, which is required
for executable permissions under AAVMF.

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

## 2. CPU and exception state

- Runs at EL1.
- `VBAR_EL1` points to a 16-entry vector table.
- All vectors currently branch to `arm64_fault`, which reports and parks.
- `DAIF` mask/unmask is implemented.
- `CNTPCT_EL0` and `CNTFRQ_EL0` provide a monotonic counter.
- PSCI `SYSTEM_OFF` is used for shutdown.
- No GIC, timer compare interrupt, IPI or secondary CPU startup.

## 3. Memory

- Kernel linked at `0x40080000`.
- 8 MiB static heap in `.bss`.
- 32 KiB Flow stacks.
- Page allocator is a bump allocator with a free-page cache.
- No MMU setup, page tables, ASIDs or user address space.
- The UEFI path relies on the firmware's identity map after
  `ExitBootServices`.

## 4. Current capabilities

| Capability | Status |
| --- | --- |
| PL011 console | Implemented |
| Generic counter time | Implemented |
| PSCI shutdown | Implemented |
| Exception vectors | Partial (faults park) |
| MMU / page tables | Missing |
| GIC / interrupts | Missing |
| Timer interrupt | Missing |
| IPI | Missing |
| SMP startup | Missing |
| User mode | Missing |
| Block devices | Missing |
| ACPI / Device Tree parsing | Missing |

## 5. Phase 1 priorities

1. MMU and page tables, with a documented kernel memory layout.
2. Exception dispatch and recovery (not just park).
3. GICv2/GICv3 initialization and IRQ dispatch.
4. Generic timer compare interrupt.
5. PSCI CPU_ON and per-CPU state.
6. Per-CPU run queues and scheduler parity with x86_64.
7. Block device support for a real root filesystem.
8. User mode and syscall entry.

Do not claim cross-architecture parity until the corresponding tests exist.

## 6. Known gaps

- No MMU, GIC, timer IRQ, IPI or SMP.
- No user mode, syscalls, processes or Arena user mapping.
- No block device or filesystem service.
- QEMU `virt` assumptions are hard-coded in the HAL.
- No ACPI/DT abstraction.
- No physical hardware qualification.

## 7. Test evidence

- `make arm64-test`: QEMU `virt`, 147 core checks, PASS.
- `make images`: arm64 IMG and ISO built.
- Local AAVMF boot of both IMG and ISO reaches the 147 core checks.
- No interrupt, SMP, MMU or user-mode test yet.
