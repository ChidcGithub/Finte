#!/usr/bin/env python3
"""Small deterministic FAT32 ESP builder for Finte.

Only the fixed files required by the UEFI loader are used. Short 8.3 names
avoid needing a VFAT long-name implementation.
"""
import math
import struct

SECTOR = 512
RESERVED = 32
FATS = 2
SPC = 1
EOC = 0x0FFFFFFF
FAT32_MIN_CLUSTERS = 65525


def _entry(name: str, attr: int, cluster: int, size: int) -> bytes:
    raw = bytearray(32)
    n = name.upper().encode("ascii")
    if len(n) > 11:
        raise ValueError("8.3 name too long: " + name)
    raw[0:len(n)] = n
    raw[11] = attr
    # timestamps intentionally zero (1980-00-00; accepted by UEFI)
    raw[20:22] = struct.pack("<H", (cluster >> 16) & 0xFFFF)
    raw[26:28] = struct.pack("<H", cluster & 0xFFFF)
    raw[28:32] = struct.pack("<I", size)
    return bytes(raw)


def _dot(name: str, cluster: int) -> bytes:
    return _entry(name, 0x10, cluster, 0)


def build_esp(files: dict[str, bytes], size_bytes: int, hidden_lba: int) -> bytes:
    """files keys are EFI paths such as 'EFI/BOOT/BOOTX64.EFI'."""
    total_sectors = size_bytes // SECTOR
    fat_size = 1
    while True:
        data_sectors = total_sectors - RESERVED - FATS * fat_size
        clusters = data_sectors // SPC
        if clusters < FAT32_MIN_CLUSTERS:
            raise ValueError("ESP too small for FAT32")
        if fat_size * (SECTOR // 4) >= clusters + 2:
            break
        fat_size += 1

    data_start = RESERVED + FATS * fat_size
    cluster_count = clusters
    fat = [0] * (cluster_count + 2)
    fat[0] = 0x0FFFFFF8
    fat[1] = 0x0FFFFFFF

    cluster_data = {}

    def set_chain(start: int, count: int) -> None:
        for c in range(start, start + count):
            fat[c] = c + 1 if c != start + count - 1 else EOC

    def alloc(data: bytes, start: int) -> int:
        count = max(1, math.ceil(len(data) / (SPC * SECTOR)))
        set_chain(start, count)
        for i in range(count):
            chunk = data[i * SECTOR:(i + 1) * SECTOR]
            cluster_data[start + i] = chunk.ljust(SECTOR, b"\x00")
        return start + count

    root_cluster = 2
    efi_cluster = 3
    boot_cluster = 4
    finte_cluster = 5
    for c in (root_cluster, efi_cluster, boot_cluster, finte_cluster):
        fat[c] = EOC

    next_cluster = 6

    def add_file(data: bytes) -> int:
        nonlocal next_cluster
        start = next_cluster
        next_cluster = alloc(data, start)
        return start

    names = {k.upper(): v for k, v in files.items()}
    efi = names.get("EFI/BOOT/BOOTX64.EFI")
    kernel = names.get("EFI/FINTE/KERNEL.BIN")
    ramdisk = names.get("EFI/FINTE/RAMDISK.BIN")
    if efi is None or kernel is None or ramdisk is None:
        raise ValueError("required ESP files missing")

    efi_start = add_file(efi)
    kernel_start = add_file(kernel)
    ramdisk_start = add_file(ramdisk)

    def dir_cluster(entries: list[bytes]) -> bytes:
        out = bytearray(SECTOR)
        off = 0
        for e in entries:
            out[off:off + 32] = e
            off += 32
        return bytes(out)

    cluster_data[root_cluster] = dir_cluster([
        _entry("FINTE      ", 0x08, 0, 0),
        _entry("EFI        ", 0x10, efi_cluster, 0),
    ])
    cluster_data[efi_cluster] = dir_cluster([
        _dot(".          ", efi_cluster),
        _dot("..         ", root_cluster),
        _entry("BOOT       ", 0x10, boot_cluster, 0),
        _entry("FINTE      ", 0x10, finte_cluster, 0),
    ])
    cluster_data[boot_cluster] = dir_cluster([
        _dot(".          ", boot_cluster),
        _dot("..         ", efi_cluster),
        _entry("BOOTX64 EFI", 0x20, efi_start, len(efi)),
    ])
    cluster_data[finte_cluster] = dir_cluster([
        _dot(".          ", finte_cluster),
        _dot("..         ", efi_cluster),
        _entry("KERNEL  BIN", 0x20, kernel_start, len(kernel)),
        _entry("RAMDISK BIN", 0x20, ramdisk_start, len(ramdisk)),
    ])

    bootsect = bytearray(SECTOR)
    bootsect[0:3] = b"\xEB\x3C\x90"
    bootsect[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bootsect, 11, SECTOR)
    bootsect[13] = SPC
    struct.pack_into("<H", bootsect, 14, RESERVED)
    bootsect[16] = FATS
    struct.pack_into("<H", bootsect, 17, 0)
    struct.pack_into("<H", bootsect, 19, 0)
    bootsect[21] = 0xF8
    struct.pack_into("<H", bootsect, 22, 0)
    struct.pack_into("<H", bootsect, 24, 63)
    struct.pack_into("<H", bootsect, 26, 255)
    struct.pack_into("<I", bootsect, 28, hidden_lba)
    struct.pack_into("<I", bootsect, 32, total_sectors)
    struct.pack_into("<I", bootsect, 36, fat_size)
    struct.pack_into("<H", bootsect, 40, 0)
    struct.pack_into("<H", bootsect, 42, 0)
    struct.pack_into("<I", bootsect, 44, root_cluster)
    struct.pack_into("<H", bootsect, 48, 1)  # FSInfo
    struct.pack_into("<H", bootsect, 50, 6)  # backup boot
    bootsect[64] = 0x80
    bootsect[66] = 0x29
    struct.pack_into("<I", bootsect, 67, 0x12345678)
    bootsect[71:82] = b"FINTE      "
    bootsect[82:90] = b"FAT32   "
    bootsect[510:512] = b"\x55\xAA"

    fsinfo = bytearray(SECTOR)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 492, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)

    img = bytearray(total_sectors * SECTOR)
    img[0:SECTOR] = bootsect
    img[SECTOR:2 * SECTOR] = fsinfo
    img[6 * SECTOR:7 * SECTOR] = bootsect
    fat_bytes = bytearray(fat_size * SECTOR)
    for i in range(cluster_count + 2):
        struct.pack_into("<I", fat_bytes, i * 4, fat[i] & 0x0FFFFFFF)
    fat0 = RESERVED * SECTOR
    img[fat0:fat0 + len(fat_bytes)] = fat_bytes
    fat1 = (RESERVED + fat_size) * SECTOR
    img[fat1:fat1 + len(fat_bytes)] = fat_bytes
    for c, data in cluster_data.items():
        off = (data_start + (c - 2) * SPC) * SECTOR
        img[off:off + len(data)] = data
    return bytes(img)
