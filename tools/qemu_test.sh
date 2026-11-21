#!/bin/bash
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
rm -f build/cli.log

(
    sleep 6
    printf 'uname\nls\ncat hello.txt\ndemo\ncompat\nshutdown\n'
) | timeout 45 qemu-system-x86_64 -m 256M -smp 4 \
    -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
    -boot c -display none -serial stdio -monitor none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    > build/cli.log 2>&1 || true

if grep -q "pipeline=OK" build/cli.log &&
   grep -q "persona=OK" build/cli.log &&
   grep -q "hello.txt" build/cli.log &&
   grep -q "Finte 0.0.0" build/cli.log &&
   grep -q "Finte (base) 0.0.0" build/cli.log; then
    echo "CLI boot test: OK"
    grep -E "Finte \(base\) 0.0.0|Finte 0.0.0|hello.txt|pipeline=OK|persona=OK|bye" build/cli.log
    exit 0
fi

echo "CLI boot test: FAILED"
cat build/cli.log
exit 1
