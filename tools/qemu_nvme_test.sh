#!/bin/bash
# Boot the x86_64 UEFI ISO with one QEMU NVMe namespace and verify the polled
# driver enumerates it as hd0, parses partitions and mounts lfc1.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build

CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
VARS_SRC=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
if [ ! -f "$CODE" ] || [ ! -f "$VARS_SRC" ]; then
    echo "NVMe test: SKIPPED (OVMF firmware not found)"
    exit 0
fi

LOG=build/nvme-test.log
VARS=build/OVMF_VARS_NVME.fd
FIFO=${TMPDIR:-/tmp}/finte-nvme-$$.in
rm -f "$LOG" "$VARS" "$FIFO"
cp "$VARS_SRC" "$VARS"
if ! mkfifo "$FIFO"; then
    echo "NVMe test: cannot create FIFO" >&2
    exit 1
fi

qemu-system-x86_64 -m 512M -smp 2 -machine q35 -accel tcg \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -cdrom build/finte-x86_64.iso \
    -drive if=none,id=nvme0,format=raw,file=build/disk-test.img \
    -device nvme,drive=nvme0,serial=FINTE_NVME \
    -boot order=d \
    -display none -monitor none -no-reboot -snapshot \
    -serial stdio -D build/nvme-test-qemu.log \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3<>"$FIFO"
for _ in $(seq 1 300); do
    if ! kill -0 "$qpid" 2>/dev/null; then
        break
    fi
    if grep -q "finte>" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if kill -0 "$qpid" 2>/dev/null; then
    printf 'disks\nparts\nmount hd0p1 /mnt\ncat /mnt/hello.txt\nwrite /mnt/nvme.txt nvme write OK\nsync\ncat /mnt/nvme.txt\nfsinfo /mnt\nshutdown\n' >&3 || true
else
    echo "NVMe test: QEMU exited before the shell prompt" >&2
fi
exec 3>&-
for _ in $(seq 1 600); do
    if grep -q "power off" "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$qpid" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
kill "$qpid" 2>/dev/null || true
wait "$qpid" 2>/dev/null || true
rm -f "$FIFO"

ok=1
for pat in \
    'hd0  64.0 MiB' \
    'hd0p1' \
    'mounted hd0p1 on /mnt type=lfc1' \
    'hello from lfc1' \
    'nvme write OK' \
    'fstype=lfc1' \
    'power off'; do
    if ! grep -Fq "$pat" "$LOG"; then
        echo "missing: $pat"
        ok=0
    fi
done
if [ "$ok" -eq 1 ]; then
    echo "NVMe polled test: OK"
    grep -E "hd0|mounted|hello from|fstype=|power off" "$LOG"
    exit 0
fi
echo "NVMe polled test: FAILED"
tail -100 "$LOG"
exit 1
