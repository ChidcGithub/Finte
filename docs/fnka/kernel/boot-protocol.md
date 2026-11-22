# Boot protocol

> **Status:** Partial — x86_64 has a versioned `fboot_info` handoff used by both
> BIOS and UEFI loaders. arm64 uses a different, argument-less ELF handoff. The
> two paths are not unified, and x86 UEFI does not call `ExitBootServices`.
>
> **Normative for:** the boot-to-kernel interface, ownership of boot-provided
> memory, firmware references and the path to a single cross-architecture
> protocol.
>
> **Related:** [Memory model](memory-model.md), [x86_64](../architecture/x86_64.md),
> [arm64](../architecture/arm64.md), [Bootloader](../../bootloader.md).

## 1. Current x86_64 handoff

The x86_64 kernel entry expects:

- `RDI` = physical address of `fboot_info_t`;
- `RSI` = `FINTE_BOOT_MAGIC` (`0xF17E0001`).

The kernel copies these registers before clearing `.bss`, then passes them to
`kmain(boot_arg, magic)`.

`fboot_info_t` is defined in `src/include/finte/boot.h`:

| Field | Meaning |
| --- | --- |
| `magic` | `FINTE_BOOT_MAGIC` |
| `boot_drive` | BIOS drive number |
| `kernel_size` | Flat kernel size |
| `ramdisk_addr`, `ramdisk_size` | Initrd location |
| `version` | Boot info version (currently 1) |
| `console_type` | 1 = VGA text, 2 = UEFI ConOut |
| `efi_system_table` | UEFI system table pointer |
| `efi_con_in`, `efi_con_out` | UEFI console protocols |
| `fb_addr`, `fb_pitch`, `fb_width`, `fb_height`, `fb_bpp`, `fb_format` | GOP framebuffer |
| `fw_idt_base`, `fw_idt_limit` | Firmware IDT for UEFI Boot Services |
| `blk_count`, `blk[]` | Firmware Block I/O descriptors |
| `input_usb` | USB keyboard hint |

### BIOS path

`src/boot/stage2.S`:

- loads the flat kernel and ramdisk into low memory;
- writes `fboot_info` at physical `0x5000`;
- fills `boot_drive`, `kernel_size`, `ramdisk_addr`, `ramdisk_size`, `version`
  and `console_type = 1`;
- enters long mode and jumps to `0x4000000` with `RDI = 0x5000`,
  `RSI = 0xF17E0001`.

The BIOS path has no UEFI pointers, GOP framebuffer, firmware IDT or firmware
Block I/O list.

### UEFI path

`src/uefi/loader.c`:

- opens the ESP with `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`;
- allocates a 36 MiB kernel region at 64 MiB;
- reads `KERNEL.BIN` and `RAMDISK.BIN`;
- fills `fboot_info`, including GOP, firmware IDT and firmware Block I/O
  descriptors;
- jumps to the kernel with the same registers.

The x86 UEFI loader **does not call `ExitBootServices`**. The kernel keeps
using firmware Block I/O and ConIn, and switches to the firmware IDT through
`hal_fw_begin()` / `hal_fw_end()` when it calls Boot Services.

## 2. Current arm64 handoff

`src/uefi/loader_arm64.c`:

- reads `\EFI\FINTE\KERNEL.ELF`;
- allocates the ELF's physical range at `0x40080000`;
- copies the `PT_LOAD` segments, zeroes the range, flushes the instruction
  cache;
- calls `ExitBootServices`;
- disables interrupts;
- branches to the ELF entry point with zeroed argument registers.

The arm64 kernel does not consume a boot info structure. It uses the fixed
PL011 UART, the generic counter, PSCI shutdown and its static heap. There is no
ramdisk, no firmware Block I/O, no ACPI/DT consumption and no framebuffer
handoff.

## 3. Ownership rules today

- BIOS `fboot_info` and the ramdisk live in low physical memory. The kernel
  identity map makes them readable.
- UEFI `fboot_info` and the kernel/ramdisk region are allocated by the loader.
  They remain valid because boot services are still active and the loader does
  not free them.
- The x86 UEFI firmware pointers are valid only while Boot Services are
  available. The kernel wraps firmware calls in `hal_fw_begin/end`.
- arm64 boot services are gone after `ExitBootServices`; the kernel must not
  call them.
- The arm64 ELF range is owned by the kernel after the jump.

## 4. Target protocol

The target is one versioned handoff structure for both architectures. It should
contain, at minimum:

- protocol version and structure size;
- architecture and platform identifiers;
- kernel image and loaded-segment information, if needed;
- a physical memory map with region types;
- ACPI and/or Device Tree references;
- framebuffer information, if available;
- boot command line or configuration data;
- initrd/module descriptors;
- entropy or random-seed information, if safely available;
- pointer ownership and lifetime rules.

Every pointer must have defined ownership. The kernel must validate all
boot-provided data before using it.

After handoff, the kernel should establish its own memory management and
interrupt configuration. Any use of UEFI runtime services should be a separate,
explicitly designed feature.

## 5. Proposed handoff sequence

1. Firmware loads the bootloader.
2. Bootloader loads the kernel and optional modules.
3. Bootloader collects memory map, ACPI/DT, framebuffer and entropy.
4. Bootloader calls `ExitBootServices`.
5. Bootloader disables interrupts and passes a validated handoff structure.
6. Kernel validates magic, version and every pointer/range.
7. Kernel builds its own page tables and interrupt state.
8. Kernel copies or takes ownership of all boot data it needs.
9. Kernel discards firmware references unless a runtime-services feature is
   explicitly enabled.

## 6. Invariants

1. The kernel never trusts a boot-provided pointer without validation.
2. Every boot-provided memory range has an owner.
3. Firmware services are not called after `ExitBootServices`.
4. The x86 BIOS and UEFI paths produce the same semantic structure.
5. The arm64 path produces the same semantic structure as x86_64.
6. The kernel does not depend on the loader's stack, page tables or code after
   handoff.
7. Unknown protocol versions are rejected safely.
8. A missing optional component (framebuffer, initrd, entropy) is reported,
   not guessed.

## 7. Current limitations

- Two different handoff mechanisms.
- arm64 has no boot info structure.
- x86 UEFI keeps firmware services and the firmware IDT.
- No physical memory map in the handoff.
- No ACPI/DT pointer.
- No module list beyond a single ramdisk.
- No command line, entropy or platform identifier.
- No structure size field beyond `version`.
- No unified validation code.

## 8. Open questions

- The exact v1/v2 structure layout and versioning policy.
- How x86_64 should migrate from firmware Block I/O/ConIn to native drivers.
- Whether `ExitBootServices` happens in the loader or the early kernel.
- How ACPI and Device Tree are represented uniformly.
- How modules/initrds are described and owned.
- How entropy is passed safely.
- How the protocol handles Secure Boot and measured boot.
- How to test the same handoff on both architectures.

## 9. Test evidence

- `make qemu-test`: BIOS handoff, ramdisk and shell.
- `make qemu-uefi-test`: UEFI handoff, firmware Block I/O, GOP and shell.
- `make qemu-disk-test` / `make qemu-uefi-disk-test`: block device handoff and
  filesystems.
- `make images`: x86_64 BIOS/UEFI IMG and ISO; arm64 UEFI IMG and ISO.
- `make arm64-test`: direct ELF boot; no boot info structure.
- No unified boot-protocol conformance test yet.
