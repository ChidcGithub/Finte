#!/bin/bash
# Run the x86_64 process ABI suite on a single CPU.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
LOG=build/abi-x86.log

run_once() {
    rm -f "$LOG"
    (sleep 3; printf 'abi\nshutdown\n') | timeout 50 qemu-system-x86_64 -m 256M -smp 1 \
        -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
        -boot c -display none -serial stdio -monitor none -no-reboot \
        > "$LOG" 2>&1 || true
    grep -q "abi: PASS" "$LOG" &&
        grep -Fq "abi] space=OK user=OK fork=OK exec=OK signal=OK cap=OK service=OK bad=OK dyn=OK churn=OK blocked=OK" "$LOG"
}

if run_once || { echo "x86 process ABI test: retrying after a boot-time scheduler timeout"; run_once; }; then
    echo "x86 process ABI test: OK"
    grep -E "abi\] open=|signal\] OK|cap\]|abi\] space=|abi: PASS" "$LOG"
    exit 0
fi
echo "x86 process ABI test: FAILED"
tail -80 "$LOG"
exit 1
