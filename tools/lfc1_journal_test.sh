#!/bin/bash
# lfc1 journal test: commit a write, roll the superblock back to the pre-write
# state, and verify mount replays the journal. A corrupted record must be
# ignored.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build

IMG=build/lfc1-journal.img
BEFORE=build/lfc1-journal-before.img
AFTER=build/lfc1-journal-after.img
CRASH=build/lfc1-journal-crash.img
TORN=build/lfc1-journal-torn.img
FINTE=./build/finte

python3 tools/mkdisk_test.py --out "$IMG" >/dev/null || exit 1
cp "$IMG" "$BEFORE"
"$FINTE" disk "$IMG" write hd0p1 journal-test.txt journal-data || exit 1
cp "$IMG" "$AFTER"

python3 - "$BEFORE" "$AFTER" "$CRASH" 0 >/dev/null <<'PY'
import struct, sys
old = open(sys.argv[1], "rb").read()
new = bytearray(open(sys.argv[2], "rb").read())
start = struct.unpack_from("<I", new, 446 + 8)[0]
p = start * 512
sb = new[p:p + 512]
jstart = struct.unpack_from("<I", sb, 96)[0]
gen = struct.unpack_from("<I", sb, 88)[0]
slot = gen & 1
new[p:p + 512] = old[p:p + 512]
open(sys.argv[3], "wb").write(new)
print(start, jstart, gen, slot)
PY

read -r P1 JSTART GEN SLOT < <(python3 - "$BEFORE" "$AFTER" /dev/null 0 <<'PY'
import struct, sys
new = open(sys.argv[2], "rb").read()
start = struct.unpack_from("<I", new, 446 + 8)[0]
p = start * 512
sb = new[p:p + 512]
print(start, struct.unpack_from("<I", sb, 96)[0],
      struct.unpack_from("<I", sb, 88)[0], struct.unpack_from("<I", sb, 88)[0] & 1)
PY
)

out=$("$FINTE" disk "$CRASH" cat hd0p1 journal-test.txt 2>&1)
if [ "$out" != "journal-data" ]; then
    echo "journal replay failed: $out"
    exit 1
fi

python3 - "$AFTER" "$TORN" "$P1" "$JSTART" "$SLOT" <<'PY'
import struct, sys
new = bytearray(open(sys.argv[1], "rb").read())
p = int(sys.argv[3]) * 512
j = p + int(sys.argv[4]) * 512
slot = int(sys.argv[5])
old_sb = bytearray(open("build/lfc1-journal-before.img", "rb").read())[p:p + 512]
new[p:p + 512] = old_sb
# Corrupt the record CRC of the committed slot.
new[j + slot * 2 * 512 + 532] ^= 0xFF
open(sys.argv[2], "wb").write(new)
PY

out=$("$FINTE" disk "$TORN" cat hd0p1 journal-test.txt 2>&1)
if echo "$out" | grep -q journal-data; then
    echo "torn journal was replayed: $out"
    exit 1
fi

python3 - "$CRASH" "$P1" <<'PY'
import struct, sys
img = open(sys.argv[1], "rb").read()
p = int(sys.argv[2]) * 512
gen = struct.unpack_from("<I", img, p + 88)[0]
if gen < 2:
    print(f"journal replay did not update generation: {gen}")
    raise SystemExit(1)
PY

python3 - "$IMG" "$P1" <<'PY'
import struct, sys
img = open(sys.argv[1], "rb").read()
start = int(sys.argv[2])
size = struct.unpack_from("<I", img, 446 + 12)[0]
with open("build/lfc1-journal-part.img", "wb") as f:
    f.write(img[start * 512:(start + size) * 512])
PY
if ! python3 tools/lfc1ck.py build/lfc1-journal-part.img | grep -q "lfc1ck: OK"; then
    echo "lfc1ck rejected the journaled image"
    exit 1
fi

echo "lfc1 journal test: OK"
