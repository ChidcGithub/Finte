# Disk manager

Finte has a small disk manager. It enumerates block devices, parses MBR and GPT
partition tables, mounts NTFS, ext4 and the native **lfc1** format, and routes
absolute shell paths to mounted filesystems. lfc1 is writable; NTFS and ext4 are
read-only.

The design goal is not to replace Linux's block layer. It is to make the
filesystems that carry real data usable from a from-scratch kernel, safely and
with very little code.

```text
shell path (/mnt/docs/file)
        |
   mount table  (longest prefix)
        |
  fs driver: ntfs | ext4 | lfc1
        |
  partition: hd0p1 ... hd0pN
        |
  block HAL: ATA PIO (BIOS) | AHCI DMA (UEFI)
             | firmware Block I/O (NVMe/USB) | host image files
```

## Commands

```text
disks                     list block devices
parts                     list partitions
mount                     list active mounts
mount <dev> <dir> [fs]    mount; fs is auto-detected when omitted
umount <dir>              unmount (flushes lfc1)
fsinfo <dir>              filesystem type, label, size, usage
df [dir]                  usage for one mount or all mounts
cd <dir>                  change the disk working directory
pwd                       print the disk working directory
ls [path]                 ramdisk in /; disk path otherwise
cat <path>                ramdisk or mounted file
stat <path>               size and type
write <path> <text...>    create/truncate and write a file
mkdir <dir>               create a directory
rm <path>                 remove a file or empty directory
touch <path>              create an empty file
cp <src> <dst>            copy ramdisk/disk to disk
mv <src> <dst>            rename on one filesystem
head <file> [lines]       first lines
tail <file> [lines]       last lines
hexdump <file>            hex and ASCII dump
tree [dir]                recursive listing
sync                      flush every writable mount
```

Absolute paths always address the disk. Relative paths use the current disk
directory once `cd` has left `/`; at `/`, bare names address the boot ramdisk.
This keeps `ls`, `cat hello.txt` and `stat` working against the ramdisk without
a second namespace.

Example with a USB test disk:

```text
finte> disks
hd0  65.0 MiB  sector=512  MBR  partitions=1
hd1  64.0 MiB  sector=512  MBR  partitions=3

finte> parts
hd0p1  64.0 MiB  start=2048  efi system  "partition 1"
hd1p1   8.0 MiB  start=2048  lfc1        "partition 1"
hd1p2  20.0 MiB  start=18432 linux       "partition 2"
hd1p3  20.0 MiB  start=59392 ntfs/exfat  "partition 3"

finte> mount hd1p1 /mnt
mounted hd1p1 on /mnt type=lfc1 label=FINTE_LFC1

finte> cd /mnt
finte> write notes.txt lfc1 is writable
finte> cat notes.txt
lfc1 is writable
finte> df
hd1p1 on /mnt: lfc1 8.0 MiB free=7.9 MiB

finte> umount /mnt
unmounted /mnt
```

Names are assigned by discovery order: `hd0`, `hd1`, ...; partitions are
`hd0p1` through `hd0pN`. A whole-disk filesystem with no partition table is
mounted as `hd0`. Mount points are longest-prefix matched, so nested mounts are
possible.

The first `disks`, `parts` or `mount` call scans the hardware. Boot does not
block on disk probing.

## Block devices

| Platform | Driver | Notes |
| --- | --- | --- |
| BIOS / SeaBIOS | ATA PIO | primary/secondary, master/slave; LBA28 and LBA48; up to 4 drives |
| UEFI / q35 | AHCI DMA | PCI class `0x010601`; SATA disks; up to 8 devices |
| UEFI firmware | Block I/O protocol | NVMe and USB mass storage exposed by firmware; 512-byte blocks |
| Host simulator | raw image files | `finte disk <image> ...` and the test scripts |

BIOS builds identity-map the first 1 GiB, so AHCI BARs above that range are not
probed there. AHCI is used on the UEFI path, which keeps OVMF's mappings.

Firmware block devices are appended after native ATA/AHCI devices. Devices whose
capacity matches a native disk are filtered out so the usual `hd0`/`hd1` names
do not change on OVMF. A firmware device that is not a duplicate (NVMe, USB
storage) appears as the next `hdN` and can be mounted directly, including
whole-disk lfc1.

Firmware calls require the firmware IDT and interrupts, while the kernel runs on
its own IDT. `hal_fw_begin()` / `hal_fw_end()` temporarily switch IDTs, save and
restore the caller's interrupt state, and let the firmware service its own timer
and device interrupts. Firmware disk operations are slow: about **2 seconds per
operation** under QEMU, so `mount`, `sync` and `umount` are visibly deliberate.

## Partition tables

- **MBR**: four primary entries, extended partitions/EBR chains, LBA starts and
  sizes. Type `0x4C` is the suggested lfc1 type.
- **GPT**: protective MBR, header at LBA 1, 128-byte entries, 64-bit start/end
  LBAs, partition names (ASCII display). Common type GUIDs are labelled:
  EFI system, Microsoft basic data, Linux filesystem, and lfc1.
- **Raw**: if no valid partition table is found, the whole device is exposed as
  one partition starting at LBA 0.

Sector size is currently 512 bytes for partition parsing. Filesystem block
sizes are independent (lfc1 4 KiB, ext4 4 KiB in the tests, NTFS 4 KiB
clusters).

## NTFS (read-only)

The driver implements the parts needed to walk and read a normal NTFS volume:

- boot sector geometry, cluster size, MFT location;
- `$MFT` record 0 and its non-resident `$DATA` runlist;
- FILE records with update-sequence-array fixups;
- resident and non-resident unnamed `$DATA` attributes;
- runlist decoding, including sparse runs;
- `$INDEX_ROOT` and `$INDEX_ALLOCATION` for `$I30` directories;
- FILE_NAME keys, directory entry deduplication by MFT reference;
- volume label from `$VOLUME_NAME`.

Unsupported: compressed or encrypted files, alternate data streams, attribute
lists that move attributes to extension records, journal replay, and NTFS
transactions. Encountering one of those returns a clean error rather than
guessing. Non-ASCII names are displayed as `?`; ASCII names round-trip.

## ext4 (read-only)

- superblock, feature checks, 64-bit block numbers;
- group descriptors, including 64-byte descriptors with `bg_inode_table_hi`;
- inode table and inode fields used for size/mode/extents;
- extent trees of any depth, unwritten extents, sparse holes;
- direct and single-indirect block maps as a fallback for ext2/3-style inodes;
- linear directory entries in every data block, including htree leaf blocks
  and the ext4 directory checksum tail.

Journal replay, inline data, `meta_bg`, encrypted/casefolded directories and
bigalloc are not implemented. Typical `mke2fs` volumes with `extent`, `64bit`,
`flex_bg`, `dir_index` and `metadata_csum` work; unsupported incompatible
features are rejected at mount time.

## lfc1 (writable)

**lfc1** is Finte's own format. It is deliberately tiny and optimized for
sequential read speed:

- no inode table, journal, allocation bitmap or indirect blocks;
- one contiguous extent per file;
- 16-byte directory entries sorted by name;
- 4 KiB metadata alignment; 512-byte sector addressing;
- a single FNV-1a checksum over the superblock.

Version 1 is read-only. Version 2 adds copy-on-write updates. Mounting loads the
whole tree into RAM: at most **512 nodes**, **8 MiB of file data**, **64 KiB per
directory blob**, depth 24, and 255-byte names.

### On-disk layout

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 512 | superblock |
| 4096 | variable | root directory blob (4 KiB aligned) |
| ... | variable | child directory blobs |
| aligned | variable | one contiguous extent per file |

Superblock (little-endian):

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | magic `LFC1` |
| 0x04 | 2 | version (1 or 2) |
| 0x06 | 2 | flags (bit 0 = checksum present) |
| 0x08 | 4 | block size (4096) |
| 0x0C | 4 | total 512-byte sectors |
| 0x10 | 4 | root directory start sector |
| 0x14 | 4 | root directory size in bytes |
| 0x18 | 4 | first data sector from the builder |
| 0x1C | 32 | label, NUL padded |
| 0x3C | 16 | UUID |
| 0x4C | 8 | creation time (0 for reproducible images) |
| 0x54 | 4 | next free sector (v2 append cursor) |
| 0x58 | 4 | generation (incremented on each sync) |
| 0x5C | 4 | free sectors at the last sync |
| 0x60 | 412 | reserved |
| 0x1FC | 4 | FNV-1a checksum of bytes 0..507 |

Directory blob:

```text
header: magic "LDR1" (4), count (4), names_off (4), flags (4)
entries: count * 16 bytes
names:   concatenated, no NUL terminators
```

Each entry:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | name offset in the name pool |
| 4 | 4 | file/directory start sector |
| 8 | 4 | size in bytes |
| 12 | 1 | flags (bit 0 = directory) |
| 13 | 1 | name length |
| 14 | 2 | reserved |

Entries are sorted by byte-wise name order on disk. The kernel loads the tree
into RAM on mount, so lookups walk the in-memory child list and file reads copy
from the cached extent.

### Writes and sync

Writes are copy-on-write and append-only:

1. `create`, `write`, `truncate`, `mkdir`, `unlink` and `rename` update the
   in-memory tree only.
2. `sync` assigns fresh extents starting at `next_free`, writes every directory
   blob and file extent, then writes a new superblock with a higher generation
   and the new `next_free`.
3. The superblock write is last. If a sync fails before it, the previous tree
   and superblock remain valid; the partial append is simply ignored.
4. Mount recomputes the append cursor from the live tree, so it never overwrites
   data it just loaded.

Old trees are not reclaimed automatically. Every sync consumes space until the
image is compacted on the host:

```bash
python3 tools/lfc1ck.py build/lfc1.img --compact build/lfc1-compact.img
```

`lfc1ck.py` validates the superblock checksum, directory magic and sorting,
extents inside the image, overlaps, and `next_free`. `--list` prints the tree.

Writes go through the block layer, which keeps an **8-entry, 4 KiB read-through
cache**. Any write invalidates all cache entries for that mount. The cache turns
directory and extent reads into single sector reads; it does not cache file data
after the mount has loaded it into RAM.

### Capability file handles

Opened files are nodes in the core table and are handed to callers as
capabilities:

- `finte_disk_file_open(path, flags, &handle)` with READ, WRITE, CREATE and
  TRUNC flags;
- the handle carries READ/WRITE rights in its capability;
- `FINTE_VERB_FILE_READ` and `FINTE_VERB_FILE_WRITE` check the cap before
  touching the filesystem;
- the shell opens disk files, installs a cap in its flow, reads/writes through
  the verbs, then drops the cap and closes the handle.

A read-only cap cannot write even if the underlying file is writable. A write
cap cannot escape the mounted path stored in the handle.

### Host tools

`tools/mklfc1.py` builds a writable v2 image from a host directory:

```bash
python3 tools/mklfc1.py --source rootfs/ --out lfc1.img \
    --label FINTE_LFC1 --size 8388608 --list
```

The host simulator can inspect and mutate an image directly:

```bash
./build/finte disk build/lfc1.img list
./build/finte disk build/lfc1.img ls hd0
./build/finte disk build/lfc1.img cat hd0 hello.txt
./build/finte disk build/lfc1.img info hd0
./build/finte disk build/lfc1.img write hd0 notes.txt "hello"
./build/finte disk build/lfc1.img mkdir hd0 docs
./build/finte disk build/lfc1.img rm hd0 notes.txt
./build/finte disk build/lfc1.img sync hd0
```

Backslashes in paths are normalized to `/`. A partition argument may be
`hd0p1` or simply `1`.

## Tests

```bash
make disk-test             # host: build three partitions, read all
make qemu-disk-test        # BIOS/ATA + MBR + lfc1/ext4/NTFS
make qemu-uefi-disk-test   # UEFI/AHCI + MBR + lfc1/ext4/NTFS
```

Writable lfc1 is exercised by the shell tests: create, write, mkdir, cp, sync,
umount, remount and read back. The UEFI firmware path is tested with an NVMe
image attached to QEMU:

```bash
qemu-system-x86_64 -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
  -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
  -drive format=raw,file=build/nvme-test.img,if=none,id=nvme0 \
  -device nvme,drive=nvme0,serial=finte0
```

The Windows GUI launcher can attach the same test disk:

```powershell
.\run-gui.ps1 -Mode uefi -TestDisk
```

`tools/mkdisk_test.py` creates a 64 MiB MBR image with lfc1, ext4 and NTFS
partitions and deterministic file contents. A GPT image can be made with
`sgdisk`; GPT parsing is exercised by the same host path.

## Limits and next steps

- Only **lfc1** is writable. NTFS and ext4 are read-only.
- lfc1 has no journal, permissions, timestamps, hard links or symlinks. Sync is
  atomic at the superblock, but an interrupted sync leaves the previous tree.
- Every sync appends a full tree; compact with `tools/lfc1ck.py --compact`.
- NVMe and USB storage are reached through UEFI firmware Block I/O, not native
  drivers. BIOS has ATA PIO only.
- The mount table is prefix-based; there is no VFS inode cache, page cache or
  concurrent writer protocol.
- NTFS attribute lists and ext4 journal recovery are not implemented.

Adding a new read-only filesystem means implementing `fdfs_ops_t` from
`src/core/fs_internal.h`, adding it to the probe list in `src/core/disk.c`, and adding a
format builder/test fixture.
