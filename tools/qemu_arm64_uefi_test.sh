#!/bin/bash
# Boot the arm64 UEFI ISO under AAVMF and verify MMU/PMM/GIC/timer/SMP.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build

CODE=${AAVMF_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}
VARS_SRC=${AAVMF_VARS:-/usr/share/AAVMF/AAVMF_VARS.fd}
if [ ! -f "$CODE" ] || [ ! -f "$VARS_SRC" ]; then
    echo "arm64 UEFI test: SKIPPED (AAVMF firmware not found)"
    exit 0
fi

cp "$VARS_SRC" build/AAVMF_VARS_TEST.fd
LOG=build/arm64-uefi.log
rm -f "$LOG"
timeout 60 qemu-system-aarch64 -M virt -cpu cortex-a57 -m 512M -smp 4 \
    -nographic -monitor none -no-reboot \
    -drive if=pflash,format=raw,unit=0,file="$CODE",readonly=on \
    -drive if=pflash,format=raw,unit=1,file=build/AAVMF_VARS_TEST.fd \
    -cdrom build/finte-arm64.iso > "$LOG" 2>&1 || true

ok=1
for pat in \
    'arm64 self-check: PASS' \
    'boot: protocol v1' \
    'pmm: usable=' \
    'mmu: identity translations ok' \
    'arm64 timer: gic=v2 ticks=' \
    'arm64 smp: 3 application processor(s) online, 4 cpu(s) total' \
    'arm64 smp: hits=2/2/2/2'; do
    if ! grep -q "$pat" "$LOG"; then
        echo "missing: $pat"
        ok=0
    fi
done
if ! grep -Eq 'mmu: (39|48)-bit identity map enabled' "$LOG"; then
    echo "missing: MMU identity map"
    ok=0
fi
if ! grep -Eq 'arm64 timer: gic=v2 ticks=[1-9]' "$LOG"; then
    echo "missing: nonzero timer ticks"
    ok=0
fi

if [ "$ok" -eq 1 ]; then
    echo "arm64 UEFI test: OK"
    grep -E 'mmu:|pmm:|protocol|timer:|smp:|self-check' "$LOG" | tail -10
    exit 0
fi
echo "arm64 UEFI test: FAILED"
tail -100 "$LOG"
exit 1
