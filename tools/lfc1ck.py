#!/usr/bin/env python3
"""Check (and optionally compact) an lfc1 filesystem image."""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mklfc1

SECTOR = 512
MAGIC = 0x3143464C
DIR_MAGIC = 0x3144524C


def rd32(b, off):
    return struct.unpack_from("<I", b, off)[0]


def fnv1a(buf):
    h = 2166136261
    for c in buf:
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def parse_dir(img, start, size, depth, ranges, errors):
    if depth > 24:
        errors.append("directory nesting too deep")
        return None
    if size < 16 or size > 65536:
        errors.append(f"bad directory size {size}")
        return None
    off = start * SECTOR
    if off + size > len(img):
        errors.append("directory outside image")
        return None
    blob = bytes(img[off:off + size])
    if rd32(blob, 0) != DIR_MAGIC:
        errors.append(f"bad directory magic at sector {start}")
        return None
    count = rd32(blob, 4)
    names_off = rd32(blob, 8)
    entries_end = 16 + count * 16
    if entries_end > size or names_off < entries_end or names_off > size:
        errors.append(f"bad directory header at sector {start}")
        return None
    ranges.append((start, start + (size + SECTOR - 1) // SECTOR,
                   "directory"))
    children = []
    names = []
    for i in range(count):
        name_off, fstart, fsize = struct.unpack_from("<III", blob, 16 + i * 16)
        flags = blob[16 + i * 16 + 12]
        nlen = blob[16 + i * 16 + 13]
        if nlen == 0 or names_off + name_off + nlen > size:
            errors.append(f"bad name in directory at sector {start}")
            return None
        name = blob[names_off + name_off:names_off + name_off + nlen].decode(
            "utf-8", "replace")
        names.append(name)
        is_dir = bool(flags & 1)
        if fsize and fstart + (fsize + SECTOR - 1) // SECTOR > len(img) // SECTOR:
            errors.append(f"extent outside image: {name}")
        if is_dir:
            child = parse_dir(img, fstart, fsize, depth + 1, ranges, errors)
            if child is None:
                return None
            ent = mklfc1.Entry(name.encode(), True, child.size, child=child)
        else:
            if fsize:
                ranges.append((fstart, fstart + (fsize + SECTOR - 1) // SECTOR,
                               name))
            data = bytes(img[fstart * SECTOR:fstart * SECTOR + fsize])
            ent = mklfc1.Entry(name.encode(), False, fsize, data=data)
        children.append(ent)
    if names != sorted(names):
        errors.append(f"directory not sorted at sector {start}")
    d = mklfc1.Dir(children)
    d.size = size
    return d


def check_ranges(ranges, errors):
    ranges = sorted(ranges)
    for (a_start, a_end, a_name), (b_start, b_end, b_name) in zip(
            ranges, ranges[1:]):
        if a_start < a_end and b_start < a_end and b_start >= a_start:
            errors.append(f"overlap: {a_name} and {b_name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--compact", metavar="OUT",
                    help="write a compacted copy to OUT")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        img = bytearray(f.read())
    errors = []
    if len(img) < SECTOR:
        print("lfc1ck: image too small")
        return 1
    sb = bytes(img[:SECTOR])
    if rd32(sb, 0) != MAGIC:
        print("lfc1ck: bad magic")
        return 1
    version, flags = struct.unpack_from("<HH", sb, 4)
    if version not in (1, 2):
        errors.append(f"unsupported version {version}")
    if flags & 1 and fnv1a(sb[:508]) != rd32(sb, 508):
        errors.append("bad superblock checksum")
    total = rd32(sb, 12)
    root_start = rd32(sb, 16)
    root_size = rd32(sb, 20)
    next_free = rd32(sb, 84)
    journal_start = rd32(sb, 96)
    journal_sectors = rd32(sb, 100)
    label = sb[28:60].split(b"\0", 1)[0]
    uuid = sb[60:76]
    if total > len(img) // SECTOR:
        errors.append("filesystem is larger than the image")
    if journal_start:
        if journal_sectors < 4 or journal_start + journal_sectors > total:
            errors.append("bad lfc1 journal extent")
        elif journal_start * SECTOR < 16 * 1024:
            errors.append("lfc1 journal overlaps metadata")
    ranges = []
    root = parse_dir(img, root_start, root_size, 0, ranges, errors)
    if root is not None:
        used_end = max((b for _, b, _ in ranges), default=0)
        if version >= 2 and next_free < used_end:
            errors.append("next_free points into the live tree")
        if journal_start and used_end > journal_start:
            errors.append("live tree overlaps lfc1 journal")
        if journal_start and next_free > journal_start:
            errors.append("next_free overlaps lfc1 journal")
        check_ranges(ranges, errors)
    if args.list and root is not None:
        mklfc1.print_tree(root)
    if errors:
        print(f"lfc1ck: {len(errors)} problem(s)")
        for e in errors:
            print("  " + e)
        return 1
    print(f"lfc1ck: OK  version={version} label={label.decode('utf-8','replace')} "
          f"files={len(ranges)} size={len(img)} next_free={next_free}")
    if args.compact:
        generation = rd32(sb, 88) + 1
        size, nf = mklfc1.pack_image(root, args.compact, label, len(img),
                                     uuid, generation)
        print(f"lfc1ck: compacted -> {args.compact} ({size} bytes, "
              f"next free {nf})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
