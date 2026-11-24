#!/usr/bin/env python3
"""Build a read-optimized lfc1 filesystem image from a directory tree.

Layout:
  sector 0    512-byte superblock
  sector 8    root directory blob (4 KiB aligned)
  ...         child directory blobs
  ...         one contiguous extent per file (4 KiB aligned)

Directory blobs are sorted by name and use fixed 16-byte entries plus a name
pool, so the kernel can binary-search without an inode table or indirect
blocks.
"""
import argparse
import hashlib
import os
import struct
import sys

MAGIC = 0x3143464C          # "LFC1" little-endian
DIR_MAGIC = 0x3144524C      # "LDR1"
VERSION = 2
BLOCK = 4096
SECTOR = 512
NAME_MAX = 255
DIR_MAX = 64 * 1024


class Entry:
    def __init__(self, name, is_dir, size, data=None, child=None):
        self.name = name
        self.is_dir = is_dir
        self.size = size
        self.data = data
        self.child = child
        self.start = 0


class Dir:
    def __init__(self, entries):
        self.entries = entries
        self.start = 0
        self.size = 0
        self.blob = None


def align4k(n):
    return (n + BLOCK - 1) & ~(BLOCK - 1)


def build_tree(path):
    entries = []
    with os.scandir(path) as scan:
        items = sorted(scan, key=lambda e: e.name.encode("utf-8"))
    for item in items:
        name = item.name.encode("utf-8")
        if not name or len(name) > NAME_MAX:
            raise ValueError(f"bad name length: {item.name!r}")
        if item.is_dir(follow_symlinks=False):
            child = build_tree(item.path)
            entries.append(Entry(name, True, child.size, child=child))
        elif item.is_file(follow_symlinks=False):
            with open(item.path, "rb") as f:
                data = f.read()
            if len(data) > 0xFFFFFFFF:
                raise ValueError(f"file too large for lfc1: {item.path}")
            entries.append(Entry(name, False, len(data), data=data))
        else:
            raise ValueError(f"unsupported file type: {item.path}")
    d = Dir(entries)
    d.size = 16 + len(entries) * 16 + sum(len(e.name) for e in entries)
    if d.size > DIR_MAX:
        raise ValueError(f"directory blob exceeds {DIR_MAX} bytes: {path}")
    return d


def assign_dirs(d, cursor):
    """cursor is in 512-byte sectors."""
    d.start = cursor
    cursor = align4k(cursor * SECTOR + d.size) // SECTOR
    for e in d.entries:
        if e.is_dir:
            cursor = assign_dirs(e.child, cursor)
    return cursor


def assign_files(d, cursor):
    cursor = align4k(cursor * SECTOR) // SECTOR
    for e in d.entries:
        if e.is_dir:
            cursor = assign_files(e.child, cursor)
        else:
            e.start = cursor
            if e.size:
                cursor = align4k(cursor * SECTOR + e.size) // SECTOR
    return cursor


def serialize_dir(d):
    names = bytearray()
    recs = []
    for e in d.entries:
        start = e.child.start if e.is_dir else e.start
        recs.append(struct.pack("<IIIBBH", len(names), start, e.size,
                                1 if e.is_dir else 0, len(e.name), 0))
        names += e.name
    names_off = 16 + len(recs) * 16
    head = struct.pack("<IIII", DIR_MAGIC, len(recs), names_off, 0)
    blob = head + b"".join(recs) + bytes(names)
    if len(blob) != d.size:
        raise AssertionError("directory size mismatch")
    d.blob = blob


def serialize_all(d):
    serialize_dir(d)
    for e in d.entries:
        if e.is_dir:
            serialize_all(e.child)


def write_all(out, d):
    out[d.start * SECTOR:d.start * SECTOR + d.size] = d.blob
    for e in d.entries:
        if e.is_dir:
            write_all(out, e.child)
        elif e.size:
            off = e.start * SECTOR
            out[off:off + e.size] = e.data


def fnv1a(buf):
    h = 2166136261
    for b in buf:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def print_tree(d, prefix="", show=True):
    if show:
        print(f"{prefix or '/'}")
    for e in d.entries:
        if e.is_dir:
            print(f"{prefix}  {e.name.decode('utf-8', 'replace')}/")
            print_tree(e.child, prefix + "  ", show=False)
        else:
            print(f"{prefix}  {e.name.decode('utf-8', 'replace')} ({e.size} bytes)")


def pack_image(root, out, label, min_size=0, uuid=None, generation=1):
    """Serialize a built tree to out. Returns (image_size, next_free)."""
    cursor = 8                              # root blob starts at sector 8
    cursor = assign_dirs(root, cursor)
    data_start = align4k(cursor * SECTOR) // SECTOR
    cursor = assign_files(root, data_start)
    used = align4k(cursor * SECTOR)
    JOURNAL_SECTORS = 4
    image_size = max(align4k(used) + JOURNAL_SECTORS * SECTOR, min_size)
    image_size = align4k(image_size)
    if image_size > 0xFFFFFFFF * SECTOR:
        raise ValueError("image exceeds lfc1 32-bit sector limit")
    if image_size < 16 * 1024:
        image_size = 16 * 1024
    journal_start = image_size // SECTOR - JOURNAL_SECTORS
    if journal_start * SECTOR < used:
        raise ValueError("image too small for lfc1 journal")

    serialize_all(root)
    image = bytearray(image_size)
    write_all(image, root)

    if uuid is None:
        uuid = hashlib.sha256(label + b"/lfc1").digest()[:16]
    next_free = align4k(used) // SECTOR
    sb = bytearray(SECTOR)
    struct.pack_into("<IHHIIII", sb, 0, MAGIC, VERSION, 1, BLOCK,
                     image_size // SECTOR, root.start, root.size)
    struct.pack_into("<I", sb, 24, data_start)
    sb[28:28 + len(label)] = label
    sb[60:76] = uuid
    struct.pack_into("<Q", sb, 76, 0)       # reproducible builds
    struct.pack_into("<III", sb, 84, next_free, generation,
                     journal_start - next_free)
    struct.pack_into("<IIII", sb, 96, journal_start, JOURNAL_SECTORS, 0, 0)
    struct.pack_into("<I", sb, 508, fnv1a(sb[:508]))
    image[:SECTOR] = sb

    with open(out, "wb") as f:
        f.write(image)
    return image_size, next_free


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, help="directory to pack")
    ap.add_argument("--out", required=True, help="output image")
    ap.add_argument("--label", default="LFC1", help="volume label (31 chars max)")
    ap.add_argument("--size", type=int, default=0,
                    help="minimum image size in bytes")
    ap.add_argument("--list", action="store_true", help="print the packed tree")
    args = ap.parse_args()

    label = args.label.encode("utf-8")[:31]
    root = build_tree(args.source)
    image_size, next_free = pack_image(root, args.out, label, args.size)

    print(f"lfc1: {args.out} {image_size} bytes, {len(root.entries)} root entries")
    print(f"      root blob sector {root.start}, {root.size} bytes; next free {next_free}")
    if args.list:
        print_tree(root)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as e:
        print(f"mklfc1: {e}", file=sys.stderr)
        sys.exit(1)
