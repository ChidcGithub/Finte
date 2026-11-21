# Finte bootloader and USB image

Finte does **not** use GRUB. The boot path is our own code.

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
  ramdisk to `0x40000`, copies the kernel to **64 MiB**, builds a 1 GiB identity
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
- It obtains GOP framebuffer geometry, fills `fboot_info`, and jumps to the flat
  kernel (`RDI=boot_info`, `RSI=0xF17E0001`).
- It does **not** call `ExitBootServices`; the kernel does not call firmware
  services afterward. Output uses the GOP linear framebuffer + COM1, input uses
  PS/2 + COM1.
- This avoids a UEFI ConOut dependency after handoff (tested ConOut calls can
  block once interrupts are disabled).

Verified in QEMU/OVMF from a QEMU USB mass-storage device and in the Windows
QEMU GTK GUI: 1280x800 GOP framebuffer, shell via serial and PS/2 keyboard,
screenshots checked.

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
make qemu-test    # BIOS CLI test
make qemu-uefi-test
```

Physical hardware is not yet validated; QEMU SeaBIOS, QEMU/OVMF, USB
mass-storage boot, NVMe firmware Block I/O, USB HID ConIn, PS/2 keyboard input,
timer preemption, ring-3 entry and the framebuffer screenshot are the current
verification. Under UEFI, USB-HID keyboards work through firmware ConIn; BIOS
uses PS/2 and serial.
