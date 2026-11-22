# Build Finte through WSL. The kernel target needs a Linux toolchain.
param(
    [ValidateSet("all", "host", "kernel", "kernel-elf", "image", "uefi",
                 "bootloader", "initrd", "user", "test", "selfcheck", "bench",
                 "demo", "compat", "disk-test", "disk-image", "qemu",
                 "gui", "gui-bios", "gui-uefi",
                 "qemu-uefi", "qemu-uefi-test", "qemu-test", "qemu-disk-test",
                 "qemu-uefi-disk-test", "arm64", "arm64-test",
                 "arm64-uefi-test", "qemu-arm64",
                 "clean")]
    [string]$Target = "all",
    [string]$Distro = "Ubuntu-26.04"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($Target -eq "gui" -or $Target -eq "gui-bios" -or $Target -eq "gui-uefi") {
    $mode = if ($Target -eq "gui-bios") { "bios" } else { "uefi" }
    & (Join-Path $root "run-gui.ps1") -Mode $mode
    exit $LASTEXITCODE
}

if ($Target -like "qemu*") {
    Write-Host "Finte: starting headless QEMU; use -Target gui for a window." -ForegroundColor Yellow
}

Write-Host "Finte: make $Target (WSL: $Distro)" -ForegroundColor Cyan
wsl -d $Distro --cd $root -- bash -lc "make $Target"
exit $LASTEXITCODE