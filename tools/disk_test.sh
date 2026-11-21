#!/bin/bash
# Host-side disk manager test: build a three-partition image and read it back.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build

python3 tools/mkdisk_test.py --out build/disk-test.img || exit 1
IMG=build/disk-test.img
FINTE=build/finte
fail=0

check() {
    local name="$1"
    shift
    if "$@"; then
        echo "  [ ok ] $name"
    else
        echo "  [fail] $name"
        fail=1
    fi
}

out=$(mktemp)
"$FINTE" disk "$IMG" list >"$out" 2>&1
check "list finds MBR disk" grep -q "scheme=MBR" "$out"
check "list finds lfc1 partition" grep -q "type=lfc1" "$out"
check "list finds ext4 partition" grep -q "type=linux" "$out"
check "list finds ntfs partition" grep -q "type=ntfs/exfat" "$out"

"$FINTE" disk "$IMG" ls hd0p1 >"$out" 2>&1
check "lfc1 ls" grep -q "hello.txt" "$out"
check "lfc1 nested directory" grep -q "docs/" "$out"

check "lfc1 hello" grep -q "hello from lfc1" \
    <("$FINTE" disk "$IMG" cat hd0p1 hello.txt)
check "lfc1 nested read" grep -q "nested lfc1 file" \
    <("$FINTE" disk "$IMG" cat hd0p1 docs/readme.txt)
check "ext4 hello" grep -q "hello from ext4" \
    <("$FINTE" disk "$IMG" cat hd0p2 hello.txt)
check "ext4 nested read" grep -q "nested ext4 file" \
    <("$FINTE" disk "$IMG" cat hd0p2 docs/readme.txt)
check "ntfs hello" grep -q "hello from ntfs" \
    <("$FINTE" disk "$IMG" cat hd0p3 hello.txt)

check "lfc1 info" grep -q "fstype=lfc1" \
    <("$FINTE" disk "$IMG" info hd0p1)
check "ext4 info" grep -q "fstype=ext4" \
    <("$FINTE" disk "$IMG" info hd0p2)
check "ntfs info" grep -q "fstype=ntfs" \
    <("$FINTE" disk "$IMG" info hd0p3)

for pair in "lfc1 hd0p1" "ext4 hd0p2" "ntfs hd0p3"; do
    set -- $pair
    name=$1
    part=$2
    src="build/disk-work/src/$name/big.bin"
    want=$(sha256sum "$src" | cut -d' ' -f1)
    got=$("$FINTE" disk "$IMG" cat "$part" big.bin | sha256sum | cut -d' ' -f1)
    if [ "$want" = "$got" ]; then
        echo "  [ ok ] $name big-file checksum"
    else
        echo "  [fail] $name big-file checksum"
        fail=1
    fi
done

rm -f "$out"
if [ "$fail" -ne 0 ]; then
    echo "disk test: FAILED"
    exit 1
fi
echo "disk test: OK"
