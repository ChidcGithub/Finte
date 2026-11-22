# Boot protocol

> **Status:** Implemented for v1 on x86_64 BIOS, x86_64 UEFI and arm64 UEFI.
> The kernel validates the structure and every described range. The memory map
> and ACPI/DTB pointers are passed through, but the kernel does not yet use
> them to replace the fixed heap/page allocator.
>
> **Normative for:** the boot-to-kernel interface, ownership of boot-provided
> memory, firmware references and the data every producer must supply.
>
> **Related:** [Memory model](memory-model.md), [x86_64](../architecture/x86_64.md),
> [arm64](../architecture/arm64.md), [Bootloader](../../bootloader.md).

## 1. Entry convention

The struct is `fboot_info_t` in `src/include/finte/boot.h`:

- `magic = FINTE_BOOT_MAGIC` (`0xF17E0001`);
- `version = FINTE_BOOT_VERSION` (1);
- `header_size` and `total_size` = `FINTE_BOOT_INFO_BYTES` (568);
- `arch` = `FINTE_BOOT_ARCH_X86_64` or `FINTE_BOOT_ARCH_ARM64`;
- `platform` = `FINTE_BOOT_PLATFORM_BIOS` or `FINTE_BOOT_PLATFORM_UEFI`.

x86_64 passes the physical struct address in `RDI` and the magic in `RSI`.
AArch64 passes the struct in `x0` and the magic in `x1`. The arm64 `_start`
preserves the arguments across `.bss` clearing and forwards them to
`kmain_arm64(boot_arg, magic)`.

The kernel calls `finte_boot_validate(bi, expected_arch)` before using any
field. On failure it reports the validation code and falls back to the
Multiboot path if that magic is present.

## 2. Structure

| Field group | Contents |
| --- | --- |
| Identity | `magic`, `version`, `header_size`, `total_size`, `arch`, `platform` |
| Kernel | `kernel_base`, `kernel_size` |
| Ramdisk | `ramdisk_addr`, `ramdisk_size` |
| Console | `console_type` (`VGA`, `UEFI`, `PL011`, `NONE`) |
| Framebuffer | `fb_addr`, `fb_pitch`, `fb_width`, `fb_height`, `fb_bpp`, `fb_format` |
| Memory map | `memmap_addr`, `memmap_count`, `memmap_desc_size` |
| Modules | `modules_addr`, `module_count`, `module_desc_size` |
| Firmware | `efi_system_table`, `efi_con_in`, `efi_con_out`, `fw_idt_base`, `fw_idt_limit`, `fw_services` |
| Firmware disk/input | `blk_count`, `blk[]`, `input_usb` |
| Platform data | `acpi_addr`, `dtb_addr` |
| Optional data | `cmdline_addr/size`, `entropy_addr/size` |

Flags:

| Flag | Meaning |
| --- | --- |
| `FINTE_BOOT_FLAG_MEMMAP` | `memmap_addr/count/desc_size` are valid |
| `FINTE_BOOT_FLAG_ACPI` | `acpi_addr` points to ACPI tables |
| `FINTE_BOOT_FLAG_DTB` | `dtb_addr` points to a Device Tree |
| `FINTE_BOOT_FLAG_FRAMEBUFFER` | framebuffer fields are valid |
| `FINTE_BOOT_FLAG_MODULES` | module array is valid |
| `FINTE_BOOT_FLAG_FW_SERVICES` | firmware pointers remain callable |

### Memory map

`fboot_mmap_entry_t` is a normalized 24-byte entry:

```c
typedef struct fboot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;   /* FINTE_BOOT_MMAP_USABLE, RESERVED, ACPI_*, ... */
    uint32_t flags;
} fboot_mmap_entry_t;
```

UEFI producers translate `EFI_MEMORY_DESCRIPTOR` types into this vocabulary.
The x86 BIOS producer stores the 24-byte E820 entries directly; their layout
matches (`base`, `length`, `type`, `attributes`).

### Modules

`fboot_module_t` describes a ramdisk, initrd or payload. x86 BIOS/UEFI describe
the ramdisk as module 0. arm64 UEFI has no ramdisk in v1.

## 3. Producers

### x86_64 BIOS

`src/boot/stage2.S`:

- zeroes a 2 KiB boot-info area at physical `0x5000`;
- writes v1 identity, kernel base and console fields;
- writes a ramdisk module descriptor at `0x5700`;
- probes E820 into `0x5800`, filters zero-length/type-0 entries and stores up
  to 64 entries;
- fills `memmap_addr`, `memmap_count`, `memmap_desc_size = 24` and the
  `MEMMAP` flag;
- enters long mode and jumps with `RDI=0x5000`, `RSI=FINTE_BOOT_MAGIC`.

The map is accessible through the kernel's low identity map.

### x86_64 UEFI

`src/uefi/loader.c`:

- allocates the kernel at 64 MiB and the ramdisk below `0x3F000000`;
- fills the v1 structure at a loader-allocated page;
- converts the UEFI memory map and copies ACPI table pointers from the UEFI
  configuration table;
- describes the ramdisk as module 0 and the framebuffer from GOP;
- calls `GetMemoryMap` and `ExitBootServices` in a small retry loop;
- clears `fw_services`, firmware pointers, Block I/O and ConIn after exit;
- jumps to the kernel with the validated structure.

After exit, storage is provided by native ATA/AHCI and the ramdisk; input is
serial and PS/2. Firmware Block I/O and ConIn are no longer used.

### arm64 UEFI

`src/uefi/loader_arm64.c`:

- loads the ELF at `0x40080000`;
- converts the UEFI memory map into `boot_mmap[]`;
- copies ACPI and Device Tree pointers from the UEFI configuration table when
  present;
- captures GOP framebuffer geometry when available;
- fills the same `fboot_info_t`;
- calls `ExitBootServices` and passes the structure in `x0`, magic in `x1`.

## 4. Validation

`src/core/bootinfo.c` checks:

- magic and version;
- `header_size >= FINTE_BOOT_INFO_BYTES` and `total_size >= header_size`;
- architecture and platform identifiers;
- console type;
- kernel base and kernel size;
- ramdisk, command-line and entropy ranges (non-zero address when size > 0,
  no overflow);
- memory-map count, descriptor size and each entry's type/length/overflow;
- module count, descriptor size and each module range;
- framebuffer address, dimensions, bpp and pitch;
- firmware flags: if `FW_SERVICES` is set, the firmware pointers must be
  present; if not set, `fw_services` must be zero.

`finte_boot_mmap_usable()` can check whether a range lies in a usable map
entry. It is available for future allocator integration.

## 5. Ownership and lifetime

- The producer owns the structure until the jump.
- The kernel treats all pointers as physical addresses in the boot address
  space and validates ranges before use.
- x86 BIOS data lives below `0x8000` and in the low identity map.
- UEFI memory-map and module arrays live in loader-owned static memory; after
  `ExitBootServices` they remain valid until the kernel copies or abandons
  them.
- ACPI/DTB tables remain firmware-owned memory; the kernel must not write them.
- The kernel does not call firmware services after x86/arm64 UEFI exit.
- The kernel parses the ramdisk in place; the producer's memory remains
  reserved for the kernel lifetime. It does not own the boot-info page.

## 6. Current limitations

- The x86 kernel still uses a fixed 1 GiB identity map, so the PMM only hands
  out pages inside that window; the static heap remains the no-map fallback.
- The arm64 kernel does not parse the DTB or ACPI tables yet.
- The x86 UEFI path no longer has firmware ConIn; USB-only keyboards need a
  future native HID driver.
- No command-line, entropy or additional module producers yet.
- ACPI/DTB pointers are passed through but not validated beyond non-zero.
- The memory map is consumed by the shared PMM, which reserves the kernel,
  ramdisk/modules, boot info, descriptor arrays, ACPI/DTB, framebuffer and
  page tables before handing out pages.

## 7. Test evidence

- `src/tests/test_main.c`: `boot protocol` validates a good structure, rejects
  bad magic/version/map entries and checks usable-range lookup.
- `make test`: 24 host tests pass.
- `make qemu-test`: x86 BIOS, E820 map, ramdisk module, 147 checks.
- `make qemu-uefi-test`: x86 UEFI, memory map, modules, `fw=0`, 147 checks.
- `make qemu-uefi-disk-test`: AHCI after `ExitBootServices`.
- `make arm64-test`: direct ELF boot without a boot structure; embedded arm64
  initrd, MMU/timer/SMP probes, EL0 `init.elf`, the `procs` demo and 147 core
  checks.
- arm64 AAVMF ISO/IMG: protocol v1, memory map, ACPI pointer, PMM counts,
  MMU/GIC/timer/SMP, EL0 user/procs and 147 checks.
- x86_64 BIOS/UEFI ISO and arm64 ISO/IMG boot checks pass locally; arm64 also
  boots the virtio disk-test image in legacy, modern and UEFI modes.
