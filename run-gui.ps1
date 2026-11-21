# Launch the Finte USB image in a windowed QEMU session.
# BIOS: VGA text console. UEFI: GOP framebuffer console.
param(
    [ValidateSet("uefi", "bios")]
    [string]$Mode = "uefi",
    [string]$Distro = "Ubuntu-26.04",
    [int]$CPUs = 4,
    [ValidateSet("auto", "whpx", "tcg")]
    [string]$Accel = "auto",
    [switch]$UsbKeyboard,
    [switch]$NoBuild,
    [switch]$TestDisk,
    [switch]$FullScreen
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root "build"
$img = Join-Path $build "finte.img"

$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
if (-not (Test-Path $qemu)) {
    $cmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "qemu-system-x86_64 not found. Install QEMU for Windows."
    }
    $qemu = $cmd.Source
}

$accelName = "tcg"
if ($Accel -eq "whpx") {
    $accelName = "whpx"
} elseif ($Accel -eq "auto") {
    $cacheDir = Join-Path $env:LOCALAPPDATA "FinteGUI"
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    $accelCache = Join-Path $cacheDir "accel.txt"
    $cached = $null
    if (Test-Path $accelCache) {
        $cached = (Get-Content $accelCache -ErrorAction SilentlyContinue |
                   Select-Object -First 1)
    }
    if ($cached -eq "whpx" -or $cached -eq "tcg") {
        $accelName = $cached
    } else {
        $probeErr = Join-Path $env:TEMP "finte-whpx-probe.txt"
        $probe = Start-Process $qemu -ArgumentList @(
            "-accel", "whpx", "-M", "q35", "-m", "16M",
            "-display", "none", "-serial", "none", "-monitor", "none",
            "-nodefaults", "-no-reboot"
        ) -RedirectStandardError $probeErr -PassThru
        Start-Sleep -Milliseconds 500
        if (-not $probe.HasExited) {
            $accelName = "whpx"
            Stop-Process -Id $probe.Id -Force -ErrorAction SilentlyContinue
        }
        Set-Content -Path $accelCache -Value $accelName
    }
    if ($accelName -eq "whpx") {
        Write-Host "WHPX hardware acceleration enabled." -ForegroundColor Green
    } else {
        Write-Host "WHPX is unavailable; using TCG." -ForegroundColor Yellow
    }
}
if ($accelName -eq "tcg") {
    Write-Host "Using TCG software emulation (slower)." -ForegroundColor Yellow
}

if (-not $NoBuild) {
    Write-Host "Updating build/finte.img with WSL..." -ForegroundColor Cyan
    wsl -d $Distro --cd $root -- bash -lc "make image"
    if ($LASTEXITCODE -ne 0) {
        if (-not (Test-Path $img)) {
            throw "image build failed"
        }
        Write-Warning "make image failed; using the existing build/finte.img"
    }
} elseif (-not (Test-Path $img)) {
    throw "build/finte.img is missing; run without -NoBuild"
}

$testDiskPath = Join-Path $build "disk-test.img"
if ($TestDisk -and -not (Test-Path $testDiskPath)) {
    Write-Host "Building build/disk-test.img with WSL..." -ForegroundColor Cyan
    wsl -d $Distro --cd $root -- bash -lc "make disk-image"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $testDiskPath)) {
        throw "disk test image build failed"
    }
}

$needUefi = $Mode -eq "uefi"
$code = $null
$vars = $null
if ($needUefi) {
    $code = Join-Path $build "OVMF_CODE_4M.fd"
    $varsTemplate = Join-Path $build "OVMF_VARS_4M.fd"
    if (-not (Test-Path $code) -or -not (Test-Path $varsTemplate)) {
        Write-Host "Copying OVMF firmware from WSL..." -ForegroundColor Cyan
        wsl -d $Distro --cd $root -- bash -lc "cp /usr/share/OVMF/OVMF_CODE_4M.fd build/OVMF_CODE_4M.fd; cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_4M.fd"
        if ($LASTEXITCODE -ne 0) {
            throw "could not copy OVMF firmware"
        }
    }
}

# QEMU for Windows cannot reliably open paths with non-ASCII/space characters,
# so stage the image and firmware under %LOCALAPPDATA%\FinteGUI.
$stage = Join-Path $env:LOCALAPPDATA "FinteGUI"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$stageImg = Join-Path $stage "finte.img"
Copy-Item $img $stageImg -Force
$img = $stageImg

if ($TestDisk) {
    $stageDisk = Join-Path $stage "disk-test.img"
    Copy-Item $testDiskPath $stageDisk -Force
    $testDiskPath = $stageDisk
}

if ($needUefi) {
    $stageCode = Join-Path $stage "OVMF_CODE_4M.fd"
    $stageVars = Join-Path $stage "OVMF_VARS.fd"
    Copy-Item $code $stageCode -Force
    Copy-Item $varsTemplate $stageVars -Force
    $code = $stageCode
    $vars = $stageVars
}

$common = @(
    "-accel", $accelName,
    "-m", "256M",
    "-smp", "$CPUs",
    "-name", "Finte $Mode",
    "-display", $(if ($accelName -eq "whpx") { "sdl" } else { "gtk" }),
    "-serial", "none",
    "-monitor", "none",
    "-no-reboot",
    "-nic", "none",
    "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"
)
if ($accelName -eq "whpx") {
    $common += @("-cpu", "qemu64,-svm")
}
$common += @("-k", "en-us")
if ($UsbKeyboard -and $Mode -eq "uefi") {
    $common += @("-device", "qemu-xhci", "-device", "usb-kbd")
}
if ($FullScreen) {
    $common += "-full-screen"
}

if ($needUefi) {
    $qemuArgs = @(
        "-machine", "q35",
        "-drive", "if=pflash,format=raw,readonly=on,file=$code",
        "-drive", "if=pflash,format=raw,file=$vars",
        "-drive", "format=raw,file=$img,if=ide,index=0,media=disk",
        "-boot", "c"
    ) + $common
} else {
    $qemuArgs = @(
        "-drive", "format=raw,file=$img,if=ide,index=0,media=disk",
        "-boot", "c"
    ) + $common
}
if ($TestDisk) {
    $qemuArgs += @("-drive", "format=raw,file=$testDiskPath,if=ide,index=1,media=disk")
}

Write-Host "Starting Finte ($Mode, $CPUs CPUs, $accelName). Close the window or run 'shutdown' in the shell." -ForegroundColor Cyan
if ($TestDisk) {
    Write-Host "Second disk attached. Try: disks, parts, mount hd1p1 /mnt, cd /mnt, write notes.txt hello, cat notes.txt" -ForegroundColor Cyan
}
$qemuLog = Join-Path $stage "qemu.log"
if ($accelName -eq "whpx") {
    try {
        Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class FinteIme {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr hWnd, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr LoadKeyboardLayout(string id, uint flags);
    [DllImport("user32.dll")]
    static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);
    public static int DisableForPid(int pid) {
        IntPtr hkl = LoadKeyboardLayout("00000409", 1);
        int count = 0;
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == (uint)pid) {
                var c = new StringBuilder(64);
                GetClassName(h, c, 64);
                if (c.ToString() == "SDL_app") {
                    PostMessage(h, 0x0050, IntPtr.Zero, hkl);
                    count++;
                }
            }
            return true;
        }, IntPtr.Zero);
        return count;
    }
}
'@
    } catch {
        # Best-effort: keep going if the helper is already loaded.
    }
}

$argList = @()
foreach ($a in $qemuArgs) {
    if ($a -match '\s') {
        $argList += '"' + $a + '"'
    } else {
        $argList += $a
    }
}
$proc = Start-Process -FilePath $qemu -ArgumentList $argList `
    -RedirectStandardError $qemuLog -PassThru
if ($accelName -eq "whpx") {
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 100
        $proc.Refresh()
        if ($proc.HasExited) {
            break
        }
        $n = [FinteIme]::DisableForPid($proc.Id)
        if ($n -gt 0) {
            Start-Sleep -Milliseconds 300
            [FinteIme]::DisableForPid($proc.Id) | Out-Null
            break
        }
    }
}
$proc.WaitForExit()
$exitCode = $proc.ExitCode
if ($exitCode -ne 0 -and $exitCode -ne 33) {
    if (Test-Path $qemuLog) {
        Write-Host "QEMU diagnostics:" -ForegroundColor Yellow
        Get-Content $qemuLog -Tail 20
    }
}
exit $exitCode
