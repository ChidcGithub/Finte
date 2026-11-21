#!/bin/bash
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS.fd
rm -f build/uefi-cli.log

(
    sleep 15
    printf 'uname\nls\ncat hello.txt\ndemo\ncompat\nshutdown\n'
) | timeout 90 qemu-system-x86_64 -m 256M -smp 4 -machine q35 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
    -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
    -boot c -display none -serial stdio -monitor none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > build/uefi-cli.log 2>&1 || true

if grep -q "pipeline=OK" build/uefi-cli.log &&
   grep -q "persona=OK" build/uefi-cli.log &&
   grep -q "hello.txt" build/uefi-cli.log &&
   grep -q "Finte 0.0.1" build/uefi-cli.log &&
   grep -q "Finte (base) 0.0.1" build/uefi-cli.log &&
   grep -q "UEFI loader" build/uefi-cli.log; then
    echo "UEFI CLI boot test: OK"
    grep -E "UEFI loader|Finte \(base\) 0.0.1|Finte 0.0.1|hello.txt|pipeline=OK|persona=OK|bye" \
        build/uefi-cli.log
    exit 0
fi

echo "UEFI CLI boot test: FAILED"
tail -80 build/uefi-cli.log
exit 1
