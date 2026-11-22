#!/bin/bash
# Run the x86_64 process ABI suite on a single CPU.
#
# QEMU's serial input comes from a FIFO whose write end stays open, and the
# commands are sent only after the shell prompt appears. This avoids fixed
# sleep-based input timing and works without a TTY on CI runners.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
LOG=build/abi-x86.log
QEMULOG=build/abi-x86-qemu.log
FIFO=${TMPDIR:-/tmp}/finte-abi-$$.in

run_once() {
    rm -f "$LOG" "$QEMULOG" "$FIFO"
    if ! mkfifo "$FIFO"; then
        echo "x86 process ABI test: cannot create $FIFO" >&2
        return 1
    fi
    qemu-system-x86_64 -m 256M -smp 1 -machine pc -accel tcg \
        -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
        -boot c -display none -monitor none -no-reboot -snapshot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -serial stdio -D "$QEMULOG" < "$FIFO" > "$LOG" 2>&1 &
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
        printf 'abi\nshutdown\n' >&3 || true
    else
        echo "[abi] QEMU exited before the shell prompt" >&2
    fi
    exec 3>&-

    # Wait for the result, then stop QEMU. The shutdown path may halt the
    # machine without terminating the emulator, so PASS is the stop signal.
    local pass=0
    for i in $(seq 1 600); do
        if grep -q "abi: PASS" "$LOG" 2>/dev/null; then
            pass=1
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
    echo "qemu pass=$pass status=$qstatus" >> "$LOG"
    rm -f "$FIFO"
    if [ "$pass" -eq 0 ] && [ "$qstatus" -ne 0 ]; then
        {
            echo "--- QEMU diagnostics ---"
            uname -m
            command -v qemu-system-x86_64 || echo "qemu binary missing"
            qemu-system-x86_64 --version 2>&1 | head -2 || true
            echo "qemu exit status: $qstatus"
            timeout 3 qemu-system-x86_64 -machine pc -accel tcg -display none \
                -serial none -monitor none -no-reboot -S >/dev/null 2>&1
            echo "minimal qemu probe status: $?"
        } >> "$LOG"
    fi
    grep -q "abi: PASS" "$LOG" &&
        grep -Fq "abi] space=OK user=OK fork=OK exec=OK signal=OK cap=OK service=OK bad=OK dyn=OK churn=OK blocked=OK" "$LOG"
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
