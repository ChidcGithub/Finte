#!/bin/bash
# Boot the arm64 kernel with a virtio-mmio data disk and verify lfc1/ext4/NTFS.
# Usage: qemu_arm64_disk_test.sh direct|uefi
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build

QEMU=${QEMU_ARM64:-qemu-system-aarch64}
IMG=build/arm64-disk-test.img
cp build/disk-test.img "$IMG"

check_log() {
    log=$1
    label=$2
    ok=1
    for pat in \
        'arm64 self-check: PASS' \
        'arm64 disk: 2 block device(s)' \
        'arm64 disk: hd0 131072 sectors sector=512 scheme=1 parts=3' \
        'arm64 disk: hd0p1 lfc1 label=FINTE_LFC1' \
        'arm64 disk: hd0p1 hello="hello from lfc1"' \
        'arm64 disk: hd0p2 ext4 label=FINTE_EXT4' \
        'arm64 disk: hd0p2 hello="hello from ext4"' \
        'arm64 disk: hd0p3 ntfs label=FINTE_NTFS' \
        'arm64 disk: hd0p3 hello="hello from ntfs"'; do
        if ! grep -Fq "$pat" "$log"; then
            echo "missing ($label): $pat"
            ok=0
        fi
    done
    if [ "$ok" -eq 1 ]; then
        echo "arm64 disk test ($label): OK"
        return 0
    fi
    tail -80 "$log"
    return 1
}

run_direct() {
    for mode in legacy modern; do
        log="build/arm64-disk-$mode.log"
        rm -f "$log"
        extra=
        if [ "$mode" = modern ]; then
            extra="-global virtio-mmio.force-legacy=false"
        fi
        timeout 60 "$QEMU" -M virt -cpu cortex-a57 -m 256M -smp 4 \
            -nographic -monitor none -no-reboot -snapshot $extra \
            -kernel build/finte-arm64.elf \
            -drive if=none,id=hd1,format=raw,file=build/swap.img \
            -device virtio-blk-device,drive=hd1 \
            -drive if=none,id=hd0,format=raw,file="$IMG" \
            -device virtio-blk-device,drive=hd0 > "$log" 2>&1 || true
        check_log "$log" "$mode" || exit 1
    done
}

run_uefi() {
    code=${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}
    vars=${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}
    if [ ! -f "$code" ] || [ ! -f "$vars" ]; then
        echo "arm64 UEFI disk test: SKIPPED (AAVMF firmware not found)"
        exit 0
    fi
    cp "$vars" build/AAVMF_VARS_DISK.fd
    log=build/arm64-uefi-disk.log
    rm -f "$log"
    timeout 90 "$QEMU" -M virt -cpu cortex-a57 -m 512M -smp 4 \
        -nographic -monitor none -no-reboot -snapshot \
        -drive if=pflash,format=raw,unit=0,file="$code",readonly=on \
        -drive if=pflash,format=raw,unit=1,file=build/AAVMF_VARS_DISK.fd \
        -cdrom build/finte-arm64.iso \
        -drive if=none,id=hd1,format=raw,file=build/swap.img \
        -device virtio-blk-device,drive=hd1 \
        -drive if=none,id=hd0,format=raw,file="$IMG" \
        -device virtio-blk-device,drive=hd0 > "$log" 2>&1 || true
    check_log "$log" uefi || exit 1
}

case "${1:-direct}" in
    direct) run_direct ;;
    uefi) run_uefi ;;
    *) echo "usage: $0 direct|uefi"; exit 2 ;;
esac
