#!/bin/bash
# xHCI HID test: boot UEFI ISO with a USB keyboard, type uname via the QEMU
# monitor, and verify the shell received it.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
LOG=build/xhci-hid.log
PORT=$(( (RANDOM % 1000) + 47000 ))
FIFO=${TMPDIR:-/tmp}/finte-xhci-$$.in
rm -f "$LOG" "$FIFO"
mkfifo "$FIFO"

VARS=build/OVMF_VARS_XHCI.fd
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$VARS"
qemu-system-x86_64 -m 512M -smp 4 -machine q35 -accel tcg \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file="$VARS" \
    -cdrom build/finte-x86_64.iso \
    -device qemu-xhci,id=xhci,p2=4,p3=0 \
    -device usb-kbd,bus=xhci.0 \
    -drive if=none,id=usb0,format=raw,file=build/disk-test.img \
    -device usb-storage,drive=usb0,bus=xhci.0 \
    -boot order=d -display none -no-reboot -snapshot \
    -serial stdio -monitor tcp:127.0.0.1:$PORT,server=on,wait=off \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 < "$FIFO" > "$LOG" 2>&1 &
qpid=$!
exec 3<>"$FIFO"
for _ in $(seq 1 300); do
    if grep -q "finte>" "$LOG" 2>/dev/null; then break; fi
    if ! kill -0 "$qpid" 2>/dev/null; then break; fi
    sleep 0.1
done

exec 3<>"$FIFO"
for _ in $(seq 1 100); do
    if exec 4<>/dev/tcp/127.0.0.1/$PORT; then
        break
    fi
    sleep 0.1
done
if ! { : >&4; } 2>/dev/null; then
    echo "xHCI HID test: monitor connection failed" >&2
    kill "$qpid" 2>/dev/null || true
    exit 1
fi
sleep 0.3
for k in u n a m e ret; do
    printf 'sendkey %s\n' "$k" >&4
    sleep 0.4
done
sleep 1
printf 'disks\nparts\nmount hd0p1 /mnt\ncat /mnt/hello.txt\nwrite /mnt/usb.txt usb write OK\nsync\ncat /mnt/usb.txt\nfsinfo /mnt\nshutdown\n' >&3 || true
exec 3>&-
exec 4>&-
for _ in $(seq 1 200); do
    if grep -q "power off" "$LOG" 2>/dev/null; then break; fi
    sleep 0.1
done
kill "$qpid" 2>/dev/null || true
wait "$qpid" 2>/dev/null || true
rm -f "$FIFO" "$VARS"

if grep -q "Finte 0.0.2.*x86_64" "$LOG" &&
   grep -q "hd0  64.0 MiB" "$LOG" &&
   grep -q "mounted hd0p1 on /mnt type=lfc1" "$LOG" &&
   grep -q "hello from lfc1" "$LOG" &&
   grep -q "usb write OK" "$LOG"; then
    echo "xHCI HID/storage test: OK"
    grep -E "Finte 0.0.2|hd0|mounted|hello from|usb write|fstype=" "$LOG" | tail -10
    exit 0
fi
echo "xHCI HID/storage test: FAILED"
tail -80 "$LOG"
exit 1
