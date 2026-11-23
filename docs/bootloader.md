# Finte bootloader and USB image

Finte does **not** use GRUB. The boot path is our own code.

For the target boot handoff contract, see the [FNKA boot protocol](fnka/kernel/boot-protocol.md).

## Hybrid image layout

`build/finte.img` is a raw disk/USB image:

```
LBA 0        BIOS boot sector (512-byte MBR)
LBA 1..32    BIOS stage2
LBA 33       BIOS image header
LBA 34..     flat kernel (BIOS copy)
...          ramdisk (BIOS copy)
LBA 2048..   EFI System Partition (FAT32)
               EFI/BOOT/BOOTX64.EFI
               EFI/FINTE/KERNEL.BIN
               EFI/FINTE/RAMDISK.BIN
```

The MBR partition table contains one type-`0xEF` entry at LBA 2048. The BIOS
payload lives before that partition; the UEFI files are inside it.

## BIOS/CSM path

- Boot sector: reads CHS geometry with INT 13h AH=08h, then loads 32 stage2
  sectors to `0x0000:0x8000`, one sector per call.
- Stage2: reads the header at LBA 33, loads the kernel to `0x10000` and the
  ramdisk to `0x60000`, copies the kernel to **64 MiB**, builds a 1 GiB identity
  map with 2 MiB pages, enters long mode, and jumps to `0x4000000`.
- CHS is used instead of EDD LBA because EDD failed on the tested QEMU BIOS,
  while CHS worked. The kernel/ramdisk are in the first ~1 MiB, so CHS is safe
  for standard USB hard-disk geometry.
- The boot sector leaves the partition table at offset 446..509 free.

Verified in QEMU SeaBIOS from a QEMU USB mass-storage device. Console: VGA
text + COM1.

## UEFI path

- Firmware loads `EFI/BOOT/BOOTX64.EFI` from the FAT32 ESP.
- The loader opens the ESP with `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, reads the
  kernel and ramdisk, and allocates 36 MiB for kernel code + `.bss` + heap at
  **64 MiB**.
- It obtains GOP framebuffer geometry, converts the UEFI memory map, captures
  the ACPI table pointer, describes the ramdisk as module 0, fills
  `fboot_info` v1, and calls `ExitBootServices`.
- It clears the firmware pointers, Block I/O and ConIn fields, then jumps to
  the flat kernel (`RDI=boot_info`, `RSI=0xF17E0001`).
- Output uses the GOP linear framebuffer + COM1, input uses PS/2 + COM1. There
  is no firmware ConOut/ConIn dependency after handoff. A USB-only keyboard
  needs a future native HID driver.

Verified in QEMU/OVMF: boot protocol v1, `fw=0`, GOP framebuffer, shell via
serial and PS/2, and AHCI disk access after `ExitBootServices`.

## ISO and arm64 media

`make images` builds four release files:

| file | boot path |
| --- | --- |
| `finte-x86_64.img` | MBR + BIOS stage2 + FAT32 ESP with `BOOTX64.EFI` |
| `finte-x86_64.iso` | El Torito BIOS hard-disk image + El Torito EFI FAT16 ESP |
| `finte-arm64.img` | MBR + FAT16 ESP with `BOOTAA64.EFI` + `KERNEL.ELF` |
| `finte-arm64.iso` | El Torito EFI FAT16 ESP |

The x86_64 BIOS ISO entry uses a 288 KiB hard-disk-emulation image with an MBR
partition whose CHS geometry fits below the EBDA; the firmware loads it at
`0x7C00` and the normal stage-1/stage-2 path takes over. The EFI entry is a
separate FAT16 ESP because El Torito stores the boot-image sector count in 16
bits, which is too small for the 64 MiB FAT32 USB image.

The arm64 loader (`src/uefi/loader_arm64.c`) reads `\EFI\FINTE\KERNEL.ELF`,
allocates its physical range at `0x40080000`, copies the `PT_LOAD` segments,
flushes the instruction cache, calls `ExitBootServices`, disables interrupts and
branches to the ELF entry point. It requires UEFI firmware such as AAVMF; there
is no arm64 BIOS path.

## Boot info

`src/include/finte/boot.h` includes kernel/ramdisk addresses, console type, UEFI
table pointers, and GOP framebuffer geometry (address, pitch, width, height,
bpp, pixel format). BIOS fills the text-console fields; UEFI fills GOP fields.

## Rufus

Use **DD Image mode**, not ISO mode. The image contains its own MBR, partition
table and FAT32 ESP. ISO mode would repartition/reformat and destroy the
embedded boot layout.

- Disable Secure Boot for the unsigned UEFI loader, or sign `BOOTX64.EFI`.
- On a UEFI-only laptop, select the USB's UEFI boot entry.
- On a CSM/legacy laptop, select the USB's legacy/BIOS boot entry.
- **Do not write the image to the internal disk.** Target the USB device only.
  Finte has no installer; it only writes to mounted lfc1 filesystems when you
  explicitly ask. NTFS and ext4 are read-only, and the boot USB image itself is
  never modified. The OS is self-contained in the image/ramdisk.

## Build

```bash
make image        # build/finte.img
make images       # x86_64 + arm64 ISO/IMG release media
make qemu-test    # BIOS CLI test
make qemu-uefi-test
```

Physical hardware is not yet validated. QEMU SeaBIOS, QEMU/OVMF, BIOS E820,
UEFI memory map/ACPI, PS/2 and serial input, timer preemption, ring-3 entry,
AHCI after `ExitBootServices` and arm64 AAVMF boot are the current
verification. USB-HID input still needs a native driver.
