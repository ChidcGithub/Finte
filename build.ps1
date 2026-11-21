# Build Finte through WSL. The kernel target needs a Linux toolchain.
param(
    [ValidateSet("all", "host", "kernel", "kernel-elf", "image", "uefi",
                 "bootloader", "initrd", "user", "test", "selfcheck", "bench",
                 "demo", "compat", "disk-test", "disk-image", "qemu",
                 "qemu-uefi", "qemu-uefi-test", "qemu-test", "qemu-disk-test",
                 "qemu-uefi-disk-test", "arm64", "arm64-test", "qemu-arm64",
                 "clean")]
    [string]$Target = "all",
    [string]$Distro = "Ubuntu-26.04"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "Finte: make $Target (WSL: $Distro)" -ForegroundColor Cyan
wsl -d $Distro --cd $root -- bash -lc "make $Target"
exit $LASTEXITCODE
