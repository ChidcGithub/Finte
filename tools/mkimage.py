#!/usr/bin/env python3
"""Pack Finte's hybrid BIOS + UEFI boot image.

Layout (LBA = 512 bytes):
  0        boot sector (BIOS)
  1..32    stage2 (16 KiB)
  33       BIOS image header
  34..     flat kernel (BIOS copy)
  ...      ramdisk (BIOS copy)
  2048..   EFI System Partition (FAT32, UEFI copy)
"""
import argparse
import struct
import sys
from pathlib import Path

import mkfat32

HEADER_MAGIC = 0x46494E48  # "FINH" as a little-endian u32
HEADER_VERSION = 1
STAGE2_SECTORS = 32
HEADER_LBA = 33
KERNEL_LBA = 34
ESP_LBA = 2048
MAX_KERNEL = 0x30000
MAX_RAMDISK = 0x40000


def align512(data: bytes) -> bytes:
    return data + b"\x00" * ((-len(data)) % 512)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--boot", required=True)
    ap.add_argument("--stage2", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--ramdisk", default="")
    ap.add_argument("--efi", default="")
    ap.add_argument("--esp-size-mb", type=int, default=64)
    ap.add_argument("--pad-to", type=int, default=0,
                    help="pad the finished image to this many bytes")
    ap.add_argument("--mbr-chs", default="",
                    help="write an MBR partition with START_H,START_S,START_C,"
                         "END_H,END_S,END_C")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    boot = Path(args.boot).read_bytes()
    stage2 = Path(args.stage2).read_bytes()
    kernel = Path(args.kernel).read_bytes()
    ramdisk = Path(args.ramdisk).read_bytes() if args.ramdisk else b""
    efi = Path(args.efi).read_bytes() if args.efi else b""

    if len(boot) != 512:
        print(f"boot sector is {len(boot)} bytes, expected 512", file=sys.stderr)
        return 1
    if len(stage2) > STAGE2_SECTORS * 512:
        print(f"stage2 is {len(stage2)} bytes, limit {STAGE2_SECTORS * 512}",
              file=sys.stderr)
        return 1
    if not kernel or len(kernel) > MAX_KERNEL:
        print("kernel missing or too large", file=sys.stderr)
        return 1
    if len(ramdisk) > MAX_RAMDISK:
        print("ramdisk too large", file=sys.stderr)
        return 1
    if not efi:
        print("warning: no UEFI loader; image will be BIOS-only", file=sys.stderr)

    kernel_pad = align512(kernel)
    ramdisk_pad = align512(ramdisk)
    kernel_sectors = len(kernel_pad) // 512
    ramdisk_sectors = len(ramdisk_pad) // 512
    ramdisk_lba = KERNEL_LBA + kernel_sectors

    header = struct.pack(
        "<IIIIIIII",
        HEADER_MAGIC, HEADER_VERSION,
        KERNEL_LBA, kernel_sectors,
        ramdisk_lba, ramdisk_sectors,
        len(kernel), len(ramdisk),
    ).ljust(512, b"\x00")

    bios_end = (HEADER_LBA + 1 + kernel_sectors + ramdisk_sectors) * 512
    if bios_end > ESP_LBA * 512:
        print("BIOS payload overlaps the ESP start", file=sys.stderr)
        return 1

    esp_size = args.esp_size_mb * 1024 * 1024
    esp_sectors = esp_size // 512
    if efi:
        esp = mkfat32.build_esp({
            "EFI/BOOT/BOOTX64.EFI": efi,
            "EFI/FINTE/KERNEL.BIN": kernel,
            "EFI/FINTE/RAMDISK.BIN": ramdisk,
        }, esp_size, ESP_LBA)
    else:
        esp = b"\x00" * esp_size

    image = bytearray()
    image += boot
    image += stage2.ljust(STAGE2_SECTORS * 512, b"\x00")
    image += header
    image += kernel_pad
    image += ramdisk_pad
    if efi:
        image += b"\x00" * (ESP_LBA * 512 - len(image))
        image += esp

    data = bytearray(image)
    if args.pad_to:
        if len(data) > args.pad_to:
            print("image is larger than --pad-to", file=sys.stderr)
            return 1
        data = bytearray(bytes(data).ljust(args.pad_to, b"\x00"))

    # MBR partition table: one EFI System Partition covering the FAT32 image.
    # A BIOS-only image has no partition table unless --mbr-chs is given.
    if efi and len(data) >= 512:
        data[446] = 0x80
        data[447:450] = bytes([0xFE, 0xFF, 0xFF])
        data[450] = 0xEF
        data[451:454] = bytes([0xFE, 0xFF, 0xFF])
        struct.pack_into("<I", data, 454, ESP_LBA)
        struct.pack_into("<I", data, 458, esp_sectors)
    if args.mbr_chs and len(data) >= 512:
        vals = [int(x, 0) for x in args.mbr_chs.split(",")]
        if len(vals) != 6:
            print("--mbr-chs expects six comma-separated values", file=sys.stderr)
            return 1
        sh, ss, sc, eh, es, ec = vals
        data[446] = 0x80
        data[447:450] = bytes([sh, (ss & 0x3F) | (((sc >> 8) & 0x3) << 6), sc & 0xFF])
        data[450] = 0x83
        data[451:454] = bytes([eh, (es & 0x3F) | (((ec >> 8) & 0x3) << 6), ec & 0xFF])
        struct.pack_into("<I", data, 454, 0)
        struct.pack_into("<I", data, 458, len(data) // 512)
    if len(data) >= 512:
        data[510:512] = b"\x55\xAA"

    Path(args.out).write_bytes(data)
    print(f"stage2:  {len(stage2)} bytes ({STAGE2_SECTORS} sectors reserved)")
    print(f"kernel:  {len(kernel)} bytes at LBA {KERNEL_LBA}")
    print(f"ramdisk: {len(ramdisk)} bytes at LBA {ramdisk_lba}")
    print(f"ESP:     LBA {ESP_LBA}, {args.esp_size_mb} MiB FAT32")
    print(f"image:   {len(data)} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
