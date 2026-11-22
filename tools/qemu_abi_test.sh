#!/bin/bash
# Run the x86_64 process ABI suite on a single CPU.
#
# The suite boots the small BIOS image as a floppy. That avoids depending on
# the host BIOS's translation of a 68 MiB IDE disk, which is not consistent
# across QEMU/SeaBIOS versions. Serial input comes from a FIFO so the harness
# works without a TTY and sends commands only after the shell prompt.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
LOG=build/abi-x86.log
QEMULOG=build/abi-x86-qemu.log
PROBELOG=build/abi-x86-probe.log
FIFO=${TMPDIR:-/tmp}/finte-abi-$$.in
ABI_LINE="abi] space=OK user=OK fork=OK wait=OK cow=OK filemap=OK exec=OK signal=OK sigset=OK job=OK pgroup=OK cap=OK service=OK bad=OK dyn=OK churn=OK blocked=OK"

run_once() {
    rm -f "$LOG" "$QEMULOG" "$PROBELOG" "$PROBELOG.qemu" "$FIFO"
    if ! mkfifo "$FIFO"; then
        echo "x86 process ABI test: cannot create $FIFO" >&2
        return 1
    fi
    qemu-system-x86_64 -m 256M -smp 1 -machine pc -accel tcg \
        -drive if=floppy,format=raw,file=build/finte-bios.img -boot a \
        -display none -monitor none -no-reboot -snapshot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -serial stdio -D "$QEMULOG" -d guest_errors < "$FIFO" > "$LOG" 2>&1 &
    local qpid=$!
    exec 3<>"$FIFO"  # open both ends so startup cannot block the script

    local i
    for i in $(seq 1 300); do
        if ! kill -0 "$qpid" 2>/dev/null; then
            break
        fi
        if grep -q "finte>" "$LOG" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$qpid" 2>/dev/null; then
        printf 'abi\n' >&3 || true
    else
        echo "[abi] QEMU exited before the shell prompt" >&2
    fi
    exec 3>&-

    for i in $(seq 1 600); do
        if grep -q "abi: PASS" "$LOG" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$qpid" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$qpid" 2>/dev/null; then
        kill "$qpid" 2>/dev/null
    fi
    local qstatus=0
    wait "$qpid" 2>/dev/null
    qstatus=$?
    echo "qemu status=$qstatus" >> "$LOG"
    rm -f "$FIFO"

    if grep -q "abi: PASS" "$LOG" &&
        grep -Fq "$ABI_LINE" "$LOG"; then
        return 0
    fi

    {
        echo "--- QEMU diagnostics ---"
        uname -m
        command -v qemu-system-x86_64 || echo "qemu binary missing"
        qemu-system-x86_64 --version 2>&1 | head -2 || true
        cc --version 2>&1 | head -1 || true
        sha256sum build/kernel64.elf build/finte-bios.img 2>/dev/null || true
        echo "qemu exit status: $qstatus"
        timeout 3 qemu-system-x86_64 -machine pc -accel tcg -display none \
            -serial none -monitor none -no-reboot -S >/dev/null 2>&1
        echo "minimal qemu probe status: $?"
        timeout 10 qemu-system-x86_64 -m 256M -smp 1 -machine pc \
            -accel tcg -drive if=floppy,format=raw,file=build/finte-bios.img -boot a \
            -display none -monitor none -no-reboot -snapshot \
            -serial "file:$PROBELOG" -D "$PROBELOG.qemu" \
            -d int,guest_errors,cpu_reset >/dev/null 2>&1
        echo "foreground boot probe status: $?"
        if [ -s "$PROBELOG" ]; then
            echo "--- foreground boot output ---"
            head -20 "$PROBELOG" | tr -d '\000' |
                sed 's/\x1b\[[0-9;]*m//g'
        fi
        if [ -s "$PROBELOG.qemu" ]; then
            echo "--- foreground QEMU log ---"
            tail -80 "$PROBELOG.qemu" | tr -d '\000'
        fi
        if [ -s "$QEMULOG" ]; then
            echo "--- QEMU internal log ---"
            tail -60 "$QEMULOG" | tr -d '\000' | sed 's/\x1b\[[0-9;]*m//g'
        fi
    } >> "$LOG"
    return 1
}

if run_once || { echo "x86 process ABI test: retrying"; run_once; }; then
    echo "x86 process ABI test: OK"
    grep -aE "abi\] open=|signal\] OK|cap\]|abi\] space=|abi: PASS" "$LOG"
    exit 0
fi
echo "x86 process ABI test: FAILED"
tail -80 "$LOG" | tr -d '\000' | sed 's/\x1b\[[0-9;]*m//g'
if [ -s "$QEMULOG" ]; then
    echo "--- QEMU internal log ---"
    tail -40 "$QEMULOG" | tr -d '\000' | sed 's/\x1b\[[0-9;]*m//g'
fi
exit 1
