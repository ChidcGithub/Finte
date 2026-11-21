#!/bin/bash
# Boot Finte with a second test disk and exercise the disk manager over serial.
# usage: qemu_disk_test.sh bios|uefi
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build

MODE=${1:-bios}
LOG="build/disk-$MODE.log"
rm -f "$LOG"

CMDS='disks
parts
mount hd1p1 /mnt
ls /mnt
cat /mnt/hello.txt
fsinfo /mnt
umount /mnt
mount hd1p2 /mnt
cat /mnt/hello.txt
cat /mnt/docs/readme.txt
fsinfo /mnt
umount /mnt
mount hd1p3 /mnt
cat /mnt/hello.txt
fsinfo /mnt
umount /mnt
shutdown
'

if [ "$MODE" = "uefi" ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_DISK.fd
    ( sleep 15; printf '%s' "$CMDS" ) | timeout 90 qemu-system-x86_64 -m 256M -smp 4 -machine q35 \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,file=build/OVMF_VARS_DISK.fd \
        -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
        -drive format=raw,file=build/disk-test.img,if=ide,index=1,media=disk \
        -boot c -display none -serial stdio -monitor none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        > "$LOG" 2>&1 || true
else
    ( sleep 7; printf '%s' "$CMDS" ) | timeout 60 qemu-system-x86_64 -m 256M -smp 4 \
        -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
        -drive format=raw,file=build/disk-test.img,if=ide,index=1,media=disk \
        -boot c -display none -serial stdio -monitor none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        > "$LOG" 2>&1 || true
fi

ok=1
for pat in 'hd1p1' 'hd1p2' 'hd1p3' 'hello from lfc1' 'hello from ext4' \
           'nested ext4 file' 'hello from ntfs' 'fstype=lfc1' 'fstype=ext4' \
           'fstype=ntfs' 'used=' 'power off'; do
    if ! grep -q "$pat" "$LOG"; then
        echo "missing: $pat"
        ok=0
    fi
done

if [ "$ok" -eq 1 ]; then
    echo "disk $MODE test: OK"
    grep -E "hd1p|hello from|fstype=|mounted|power off" "$LOG"
    exit 0
fi

echo "disk $MODE test: FAILED"
tail -100 "$LOG"
exit 1
