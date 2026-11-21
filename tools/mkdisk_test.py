#!/usr/bin/env python3
"""Build a 64 MiB MBR test disk with lfc1, ext4 and NTFS partitions.

Used by the host and QEMU disk tests. Requires mkfs.ext4 and ntfs-3g tools
(mkntfs/ntfscp), which are standard on WSL/Linux.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
MIB = 1024 * 1024
TOTAL = 64 * MIB

P1_START = 2048
P1_SIZE = 8 * MIB
P2_START = P1_START + P1_SIZE // SECTOR
P2_SIZE = 20 * MIB
P3_START = P2_START + P2_SIZE // SECTOR
P3_SIZE = 20 * MIB


def find_tool(name, *fallbacks):
    path = shutil.which(name)
    if path:
        return path
    for p in fallbacks:
        if os.path.exists(p):
            return p
    raise RuntimeError(f"missing tool: {name}")


def run(cmd):
    subprocess.run(cmd, check=True)


def make_sources(root):
    for fs, text in (("lfc1", "hello from lfc1\n"),
                     ("ext4", "hello from ext4\n")):
        d = os.path.join(root, fs)
        os.makedirs(os.path.join(d, "docs"), exist_ok=True)
        with open(os.path.join(d, "hello.txt"), "w", encoding="ascii") as f:
            f.write(text)
        with open(os.path.join(d, "docs", "readme.txt"), "w",
                  encoding="ascii") as f:
            f.write(f"nested {fs} file\n")
        with open(os.path.join(d, "big.bin"), "wb") as f:
            f.write(bytes(range(256)) * 80)  # 20480 bytes
    d = os.path.join(root, "ntfs")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "hello.txt"), "w", encoding="ascii") as f:
        f.write("hello from ntfs\n")
    with open(os.path.join(d, "big.bin"), "wb") as f:
        f.write(bytes(range(256)) * 160)  # 40960 bytes


def write_mbr(disk):
    def entry(index, ptype, start, sectors):
        off = 446 + index * 16
        disk[off + 0] = 0x00
        disk[off + 1] = 0xFE
        disk[off + 2] = 0xFF
        disk[off + 3] = 0xFF
        disk[off + 4] = ptype
        disk[off + 5] = 0xFE
        disk[off + 6] = 0xFF
        disk[off + 7] = 0xFF
        struct.pack_into("<I", disk, off + 8, start)
        struct.pack_into("<I", disk, off + 12, sectors)

    entry(0, 0x4C, P1_START, P1_SIZE // SECTOR)  # lfc1
    entry(1, 0x83, P2_START, P2_SIZE // SECTOR)  # ext4
    entry(2, 0x07, P3_START, P3_SIZE // SECTOR)  # NTFS
    disk[510] = 0x55
    disk[511] = 0xAA


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="build/disk-test.img")
    ap.add_argument("--work", default="build/disk-work")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = args.out if os.path.isabs(args.out) else os.path.join(root, args.out)
    work = args.work if os.path.isabs(args.work) else os.path.join(root, args.work)
    src = os.path.join(work, "src")
    os.makedirs(src, exist_ok=True)
    make_sources(src)

    py = sys.executable
    lfc_img = os.path.join(work, "lfc1.img")
    run([py, os.path.join(root, "tools", "mklfc1.py"),
         "--source", os.path.join(src, "lfc1"), "--out", lfc_img,
         "--label", "FINTE_LFC1", "--size", str(P1_SIZE)])

    ext_img = os.path.join(work, "ext4.img")
    mkfs_ext4 = find_tool("mkfs.ext4", "/usr/sbin/mkfs.ext4", "/sbin/mkfs.ext4")
    run([mkfs_ext4, "-F", "-q", "-b", "4096", "-L", "FINTE_EXT4",
         "-d", os.path.join(src, "ext4"), ext_img, "20M"])

    nt_img = os.path.join(work, "ntfs.img")
    with open(nt_img, "wb") as f:
        f.truncate(P3_SIZE)
    mkntfs = find_tool("mkntfs", "/usr/sbin/mkntfs", "/sbin/mkntfs")
    ntfscp = find_tool("ntfscp", "/usr/sbin/ntfscp", "/sbin/ntfscp")
    run([mkntfs, "-F", "-Q", "-L", "FINTE_NTFS", nt_img])
    run([ntfscp, "-f", nt_img, os.path.join(src, "ntfs", "hello.txt"),
         "/hello.txt"])
    run([ntfscp, "-f", nt_img, os.path.join(src, "ntfs", "big.bin"),
         "/big.bin"])

    disk = bytearray(TOTAL)
    write_mbr(disk)
    for path, start, size in ((lfc_img, P1_START, P1_SIZE),
                              (ext_img, P2_START, P2_SIZE),
                              (nt_img, P3_START, P3_SIZE)):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) > size:
            raise RuntimeError(f"{path} is larger than its partition")
        off = start * SECTOR
        disk[off:off + len(data)] = data

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(disk)
    print(f"test disk: {out} ({TOTAL} bytes)")
    print(f"  hd0p1 lfc1  start={P1_START} sectors={P1_SIZE // SECTOR}")
    print(f"  hd0p2 ext4  start={P2_START} sectors={P2_SIZE // SECTOR}")
    print(f"  hd0p3 ntfs  start={P3_START} sectors={P3_SIZE // SECTOR}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as e:
        print(f"mkdisk_test: {e}", file=sys.stderr)
        sys.exit(1)
