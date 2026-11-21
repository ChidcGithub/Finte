#!/usr/bin/env python3
"""Build a small hybrid ISO9660 image with El Torito BIOS/UEFI boot entries."""
import argparse
import struct
from pathlib import Path

SECTOR = 2048
PVD_LBA = 16
BOOT_RECORD_LBA = 17
TERMINATOR_LBA = 18
PATH_L_LBA = 19
PATH_M_LBA = 20
ROOT_LBA = 21
README_LBA = 22
CATALOG_LBA = 23
BOOT_IMAGE_LBA = 24


def _both16(value: int) -> bytes:
    return struct.pack("<H", value) + struct.pack(">H", value)


def _both32(value: int) -> bytes:
    return struct.pack("<I", value) + struct.pack(">I", value)


def _date7() -> bytes:
    return bytes([126, 11, 21, 0, 0, 0, 0])  # 2026-11-21 UTC


def _date17() -> bytes:
    return bytes([126, 11, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])


def _dir_record(name: bytes, extent: int, size: int, flags: int) -> bytes:
    n = len(name)
    length = 33 + n + (0 if n % 2 else 1)
    rec = bytearray(length)
    rec[0] = length
    rec[1] = 0
    rec[2:10] = _both32(extent)
    rec[10:18] = _both32(size)
    rec[18:25] = _date7()
    rec[25] = flags
    rec[28:32] = _both16(1)
    rec[32] = n
    rec[33:33 + n] = name
    return bytes(rec)


def _boot_entry(media: int, sectors: int, lba: int) -> bytes:
    if sectors <= 0 or sectors > 0xFFFF:
        raise ValueError("El Torito boot image sector count out of range")
    entry = bytearray(32)
    entry[0] = 0x88
    entry[1] = media
    struct.pack_into("<H", entry, 6, sectors)
    struct.pack_into("<I", entry, 8, lba)
    return bytes(entry)


def _validation(platform: int) -> bytes:
    entry = bytearray(32)
    entry[0] = 0x01
    entry[1] = platform
    entry[4:20] = b"FINTE".ljust(16, b"\x00")
    entry[30:32] = b"\x55\xAA"
    words = struct.unpack("<16H", entry)
    checksum = (-sum(words)) & 0xFFFF
    struct.pack_into("<H", entry, 28, checksum)
    return bytes(entry)


def _section(platform: int, entries: int) -> bytes:
    header = bytearray(32)
    header[0] = 0x90
    header[1] = platform
    struct.pack_into("<H", header, 2, entries)
    return bytes(header)


def _padded(data: bytes, align: int) -> bytes:
    return data + b"\x00" * ((-len(data)) % align)


def _readme(arch: str) -> bytes:
    lines = [
        f"Finte {arch} boot media",
        "",
        "x86_64: BIOS and UEFI boot are supported." if arch == "x86_64"
        else "arm64: boot with UEFI firmware (QEMU virt + AAVMF).",
        "",
        "The El Torito boot images contain the kernel and ramdisk.",
        "Source: https://github.com/ChidcGithub/Finte",
    ]
    return ("\n".join(lines) + "\n").encode("ascii")


def build_iso(bios: bytes | None, efi: bytes | None, arch: str,
              bios_media: str = "auto") -> bytes:
    if bios is None and efi is None:
        raise ValueError("at least one boot image is required")
    # Boot images are placed on 2048-byte CD sector boundaries.
    bios_pad = _padded(_padded(bios, 512), SECTOR) if bios else None
    efi_pad = _padded(_padded(efi, 512), SECTOR) if efi else None
    bios_lba = BOOT_IMAGE_LBA if bios_pad else 0
    efi_lba = BOOT_IMAGE_LBA + (len(bios_pad) // SECTOR if bios_pad else 0)

    readme = _readme(arch)
    total_sectors = BOOT_IMAGE_LBA
    total_sectors += len(bios_pad) // SECTOR if bios_pad else 0
    total_sectors += len(efi_pad) // SECTOR if efi_pad else 0
    if total_sectors * SECTOR > 0xFFFFFFFF:
        raise ValueError("ISO is too large")

    catalog = bytearray()
    if bios_pad:
        if bios_media == "auto":
            media = 0x02 if len(bios_pad) == 1474560 else 0x04
        elif bios_media == "floppy":
            media = 0x02
        elif bios_media == "disk":
            media = 0x04
        else:
            media = 0x00
        catalog += _validation(0x00)
        catalog += _boot_entry(media, len(bios_pad) // 512, bios_lba)
        if efi_pad:
            catalog += _section(0xEF, 1)
            catalog += _boot_entry(0x00, len(efi_pad) // 512, efi_lba)
    else:
        catalog += _validation(0xEF)
        catalog += _boot_entry(0x00, len(efi_pad) // 512, efi_lba)
    catalog = _padded(bytes(catalog), SECTOR)

    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"FINTE".ljust(32, b" ")
    label = ("FINTE_" + arch.upper()).encode("ascii")[:32]
    pvd[40:72] = label.ljust(32, b" ")
    pvd[80:88] = _both32(total_sectors)
    pvd[120:124] = _both16(1)
    pvd[124:128] = _both16(1)
    pvd[128:132] = _both16(SECTOR)
    pvd[132:140] = _both32(10)
    pvd[140:144] = struct.pack("<I", PATH_L_LBA)
    pvd[148:152] = struct.pack(">I", PATH_M_LBA)
    pvd[156:190] = _dir_record(b"\x00", ROOT_LBA, SECTOR, 0x02)
    pvd[190:318] = b" ".ljust(128, b" ")
    pvd[318:446] = b" ".ljust(128, b" ")
    pvd[446:574] = b" ".ljust(128, b" ")
    pvd[574:702] = b"FINTE".ljust(128, b" ")
    pvd[702:739] = b" ".ljust(37, b" ")
    pvd[739:776] = b" ".ljust(37, b" ")
    pvd[776:813] = b" ".ljust(37, b" ")
    pvd[813:830] = _date17()
    pvd[830:847] = _date17()
    pvd[847:864] = b"0" * 17
    pvd[864:881] = _date17()
    pvd[881] = 1

    boot_record = bytearray(SECTOR)
    boot_record[0] = 0
    boot_record[1:6] = b"CD001"
    boot_record[6] = 1
    boot_record[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
    struct.pack_into("<I", boot_record, 71, CATALOG_LBA)

    terminator = bytearray(SECTOR)
    terminator[0] = 255
    terminator[1:6] = b"CD001"
    terminator[6] = 1

    path_l = bytearray(SECTOR)
    path_l[0:2] = bytes([1, 0])
    path_l[2:6] = struct.pack("<I", ROOT_LBA)
    path_l[6:8] = struct.pack("<H", 1)
    path_l[8] = 0
    path_m = bytearray(SECTOR)
    path_m[0:2] = bytes([1, 0])
    path_m[2:6] = struct.pack(">I", ROOT_LBA)
    path_m[6:8] = struct.pack(">H", 1)
    path_m[8] = 0

    root = bytearray(SECTOR)
    root_records = (
        _dir_record(b"\x00", ROOT_LBA, SECTOR, 0x02)
        + _dir_record(b"\x01", ROOT_LBA, SECTOR, 0x02)
        + _dir_record(b"README.TXT;1", README_LBA, len(readme), 0x00)
    )
    root[0:len(root_records)] = root_records

    image = bytearray(total_sectors * SECTOR)
    image[PVD_LBA * SECTOR:(PVD_LBA + 1) * SECTOR] = pvd
    image[BOOT_RECORD_LBA * SECTOR:(BOOT_RECORD_LBA + 1) * SECTOR] = boot_record
    image[TERMINATOR_LBA * SECTOR:(TERMINATOR_LBA + 1) * SECTOR] = terminator
    image[PATH_L_LBA * SECTOR:(PATH_L_LBA + 1) * SECTOR] = path_l
    image[PATH_M_LBA * SECTOR:(PATH_M_LBA + 1) * SECTOR] = path_m
    image[ROOT_LBA * SECTOR:(ROOT_LBA + 1) * SECTOR] = root
    image[README_LBA * SECTOR:README_LBA * SECTOR + len(readme)] = readme
    image[CATALOG_LBA * SECTOR:(CATALOG_LBA + 1) * SECTOR] = catalog
    if bios_pad:
        off = bios_lba * SECTOR
        image[off:off + len(bios_pad)] = bios_pad
    if efi_pad:
        off = efi_lba * SECTOR
        image[off:off + len(efi_pad)] = efi_pad
    return bytes(image)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--arch", choices=["x86_64", "arm64"], required=True)
    ap.add_argument("--bios", default="")
    ap.add_argument("--bios-media", choices=["auto", "floppy", "disk", "noemul"],
                    default="auto")
    ap.add_argument("--efi", default="")
    args = ap.parse_args()
    bios = Path(args.bios).read_bytes() if args.bios else None
    efi = Path(args.efi).read_bytes() if args.efi else None
    image = build_iso(bios, efi, args.arch, args.bios_media)
    Path(args.out).write_bytes(image)
    print(f"ISO: {len(image)} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
