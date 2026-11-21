#!/usr/bin/env python3
"""Wrap a FAT ESP in a minimal MBR so it can boot as a normal disk image."""
import argparse
import struct
from pathlib import Path

SECTOR = 512
START_LBA = 2048


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--esp", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    esp = Path(args.esp).read_bytes()
    if not esp or len(esp) % SECTOR:
        raise SystemExit("ESP size must be a non-zero multiple of 512")
    mbr = bytearray(START_LBA * SECTOR)
    mbr[446] = 0x80
    mbr[447:450] = bytes([0xFE, 0xFF, 0xFF])
    mbr[450] = 0xEF
    mbr[451:454] = bytes([0xFE, 0xFF, 0xFF])
    struct.pack_into("<I", mbr, 454, START_LBA)
    struct.pack_into("<I", mbr, 458, len(esp) // SECTOR)
    mbr[510:512] = b"\x55\xAA"
    image = bytes(mbr) + esp
    Path(args.out).write_bytes(image)
    print(f"disk: {len(image)} bytes -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
