#!/usr/bin/env python3
"""Pack a directory into Finte's read-only initrd format."""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0x46494E52  # FINR


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--extra", action="append", default=[],
                    help="extra NAME=PATH entry")
    args = ap.parse_args()
    root = Path(args.dir)
    if not root.is_dir():
        print("not a directory: " + str(root), file=sys.stderr)
        return 1
    entries = []
    for p in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = p.relative_to(root).as_posix()
        entries.append((rel, p.read_bytes()))
    for spec in args.extra:
        if "=" not in spec:
            print("bad --extra (want NAME=PATH): " + spec, file=sys.stderr)
            return 1
        name, path = spec.split("=", 1)
        entries.append((name, Path(path).read_bytes()))
    entries.sort(key=lambda e: e[0])
    if len(entries) > 32:
        print("too many files (max 32)", file=sys.stderr)
        return 1
    out = bytearray()
    out += struct.pack("<II", MAGIC, len(entries))
    for rel, data in entries:
        name = rel.encode("ascii", "strict")
        if len(name) >= 32:
            print("name too long: " + rel, file=sys.stderr)
            return 1
        padded = name + b"\x00" * (32 - len(name))
        out += padded
        out += struct.pack("<I", len(data))
        out += data
        out += b"\x00" * ((-len(data)) % 4)
    Path(args.out).write_bytes(out)
    print(f"initrd: {len(entries)} files, {len(out)} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
