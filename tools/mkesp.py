#!/usr/bin/env python3
"""Build a small FAT16 EFI System Partition image.

Used for the ISO El Torito EFI image and the arm64 UEFI disk image.  FAT16
keeps the image small enough for the 16-bit El Torito sector count.
"""
import argparse
import math
import struct
from pathlib import Path

SECTOR = 512
RESERVED = 1
FATS = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
EOC = 0xFFFF
MAX_CLUSTERS = 0xFFF4


def _name83(name: str) -> bytes:
    if name == ".":
        return b".          "
    if name == "..":
        return b"..         "
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    base = base.upper()
    ext = ext.upper()
    if not base or len(base) > 8 or len(ext) > 3:
        raise ValueError("not an 8.3 name: " + name)
    if any(c in base + ext for c in ' "*/:<>?\\|'):
        raise ValueError("invalid 8.3 name: " + name)
    return base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def _entry(name: str, attr: int, cluster: int, size: int) -> bytes:
    raw = bytearray(32)
    raw[0:11] = _name83(name)
    raw[11] = attr
    struct.pack_into("<H", raw, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", raw, 26, cluster & 0xFFFF)
    struct.pack_into("<I", raw, 28, size)
    return bytes(raw)


def _dir(name: str, cluster: int) -> bytes:
    return _entry(name, 0x10, cluster, 0)


def build_esp(files: dict[str, bytes], size_bytes: int) -> bytes:
    if size_bytes % SECTOR:
        raise ValueError("ESP size must be a multiple of 512")
    total_sectors = size_bytes // SECTOR
    if total_sectors > 0xFFFF:
        raise ValueError("FAT16 ESP must fit in 65535 sectors")

    fat_sectors = 1
    while True:
        data_sectors = total_sectors - RESERVED - FATS * fat_sectors - ROOT_SECTORS
        clusters = data_sectors // 1
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if need <= fat_sectors:
            break
        fat_sectors = need
    if clusters > MAX_CLUSTERS:
        raise ValueError("ESP has too many clusters for FAT16")

    root_lba = RESERVED + FATS * fat_sectors
    data_lba = root_lba + ROOT_SECTORS
    fat = [0] * (clusters + 2)
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF
    cluster_data: dict[int, bytes] = {}
    next_cluster = 2

    def alloc(count: int) -> int:
        nonlocal next_cluster
        if count <= 0:
            count = 1
        start = next_cluster
        if start + count > clusters + 2:
            raise ValueError("ESP is too small for its files")
        for c in range(start, start + count):
            fat[c] = c + 1 if c != start + count - 1 else EOC
        next_cluster += count
        return start

    # Build the directory tree.
    root: dict = {"dirs": {}, "files": []}
    for path, data in files.items():
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        if not parts:
            continue
        node = root
        for part in parts[:-1]:
            node = node["dirs"].setdefault(part, {"dirs": {}, "files": []})
        node["files"].append((parts[-1], data))

    dir_cluster: dict[int, int] = {}

    def assign_dirs(node: dict, parent_cluster: int) -> None:
        for name, child in node["dirs"].items():
            count = max(1, math.ceil((len(child["dirs"]) + len(child["files"]) + 2) * 32 / SECTOR))
            cluster = alloc(count)
            dir_cluster[id(child)] = cluster
            child["_parent"] = parent_cluster
            child["_cluster"] = cluster
            assign_dirs(child, cluster)

    assign_dirs(root, 0)

    for name, child in root["dirs"].items():
        child["_parent"] = 0

    # Allocate file chains and remember their starting clusters.
    for name, child in root["dirs"].items():
        stack = [child]
        while stack:
            node = stack.pop()
            for fname, data in node["files"]:
                count = max(1, math.ceil(len(data) / SECTOR))
                cluster = alloc(count)
                for i in range(count):
                    chunk = data[i * SECTOR:(i + 1) * SECTOR]
                    cluster_data[cluster + i] = chunk.ljust(SECTOR, b"\x00")
                node.setdefault("_file_clusters", {})[fname] = (cluster, len(data))
            for sub in node["dirs"].values():
                stack.append(sub)

    def dir_bytes(node: dict, cluster: int, parent: int, is_root: bool) -> bytes:
        entries = []
        if not is_root:
            entries.append(_dir(".", cluster))
            entries.append(_dir("..", parent))
        for dname, child in node["dirs"].items():
            entries.append(_dir(dname, child["_cluster"]))
        for fname, data in node["files"]:
            fcluster, fsize = node["_file_clusters"][fname]
            entries.append(_entry(fname, 0x20, fcluster, fsize))
        need = max(1, math.ceil(len(entries) * 32 / SECTOR))
        out = bytearray(need * SECTOR)
        off = 0
        for e in entries:
            out[off:off + 32] = e
            off += 32
        return bytes(out)

    root_entries = [_entry("FINTE", 0x08, 0, 0)]
    for dname, child in root["dirs"].items():
        root_entries.append(_dir(dname, child["_cluster"]))
    root_data = bytearray(ROOT_SECTORS * SECTOR)
    off = 0
    for e in root_entries:
        root_data[off:off + 32] = e
        off += 32

    for name, child in root["dirs"].items():
        stack = [child]
        while stack:
            node = stack.pop()
            data = dir_bytes(node, node["_cluster"], node["_parent"], False)
            for i in range(0, len(data), SECTOR):
                cluster_data[node["_cluster"] + i // SECTOR] = data[i:i + SECTOR]
            for sub in node["dirs"].values():
                stack.append(sub)

    img = bytearray(total_sectors * SECTOR)

    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", boot, 11, SECTOR)
    boot[13] = 1
    struct.pack_into("<H", boot, 14, RESERVED)
    boot[16] = FATS
    struct.pack_into("<H", boot, 17, ROOT_ENTRIES)
    struct.pack_into("<H", boot, 19, total_sectors)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, fat_sectors)
    struct.pack_into("<H", boot, 24, 63)
    struct.pack_into("<H", boot, 26, 255)
    struct.pack_into("<I", boot, 28, 0)
    struct.pack_into("<H", boot, 38, 0)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x12345678)
    boot[43:54] = b"FINTE      "
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"
    img[0:SECTOR] = boot

    fat_bytes = bytearray(fat_sectors * SECTOR)
    for i, value in enumerate(fat):
        struct.pack_into("<H", fat_bytes, i * 2, value)
    for i in range(FATS):
        start = (RESERVED + i * fat_sectors) * SECTOR
        img[start:start + len(fat_bytes)] = fat_bytes

    root_off = root_lba * SECTOR
    img[root_off:root_off + len(root_data)] = root_data

    for cluster, data in cluster_data.items():
        off = (data_lba + (cluster - 2)) * SECTOR
        img[off:off + SECTOR] = data

    return bytes(img)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-mb", type=int, default=16)
    ap.add_argument("--add", action="append", default=[],
                    metavar="ESP_PATH=HOST_PATH")
    args = ap.parse_args()
    files = {}
    for item in args.add:
        if "=" not in item:
            ap.error("--add expects ESP_PATH=HOST_PATH")
        name, path = item.split("=", 1)
        files[name] = Path(path).read_bytes()
    img = build_esp(files, args.size_mb * 1024 * 1024)
    Path(args.out).write_bytes(img)
    print(f"ESP: {len(img)} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
