# Building Finte

## Requirements

### Host simulator

- Linux or WSL: GCC (or Clang), GNU make, libc, `ucontext`.
- Windows: LLVM `clang-cl` and Visual Studio Build Tools with the C++ workload.
- No third-party libraries. The core uses compiler-provided headers and the
  host HAL uses the C runtime.

### Bootable image / kernel

- Linux/WSL x86_64 with GCC, GNU binutils (`ld`, `objcopy`), GNU make.
- `gcc -m32` support for the 32-bit bootloader assembly. No 32-bit libc is
  linked.
- Python 3 for the image/FAT32/initrd builders.
- QEMU `qemu-system-x86_64` for BIOS (`SeaBIOS`) and UEFI (`OVMF`) tests.
- LLVM `clang` + `lld-link` for the PE32+ UEFI loader.
- Optional arm64: `clang`, `aarch64-linux-gnu-ld` and
  `qemu-system-aarch64`.
- OVMF firmware files for `make qemu-uefi`:
  `/usr/share/OVMF/OVMF_CODE_4M.fd`, `OVMF_VARS_4M.fd`.

Ubuntu/WSL:

```bash
sudo apt install build-essential qemu-system ovmf clang lld \
  binutils-aarch64-linux-gnu qemu-system-arm
```

Build the arm64 QEMU `virt` image and run the core suite:

```bash
make arm64-test
```

The modern UEFI test also needs `lld-link` (from LLVM), `python3`, and `cp`.

## Targets

```bash
make all            # host simulator + build/finte.img
make host           # build/finte
make kernel         # build/kernel64.bin (flat kernel)
make kernel-elf     # optional Multiboot fallback (build/finte.elf)
make bootloader     # build/boot.bin + build/stage2.bin
make uefi           # build/uefi/BOOTX64.EFI
make user           # build/init.elf + ipc.elf (ring-3 processes, packed)
make initrd         # build/initrd.bin
make image          # build/finte.img (BIOS + UEFI USB image)
make test           # 19 extended self-tests (host)
make selfcheck      # startup POST/self-check suite (host)
make demo           # host-side native pipeline
make compat         # host-side Linux persona
make bench          # host microbenchmarks
make qemu           # BIOS boot, serial console (SMP=4 by default)
make qemu-uefi      # OVMF UEFI boot, serial console
make qemu-test      # BIOS CLI boot test
make qemu-uefi-test # UEFI CLI boot test
make clean
```

`SMP` defaults to 4 for `make qemu` / `make qemu-uefi`; override it with
`make qemu SMP=2`. The test scripts and the Windows GUI launcher also default
to 4 CPUs. `ACCEL` automatically uses KVM when `/dev/kvm` is writable and falls
back to TCG; override it with `make qemu ACCEL="-accel tcg"`.

The image targets need Linux/WSL. The host simulator also builds natively on
Windows.

## Windows: WSL driver

`build.ps1` runs make inside WSL and preserves the exit code:

```powershell
.\build.ps1 -Target all
.\build.ps1 -Target image
.\build.ps1 -Target test
.\build.ps1 -Target qemu-test
.\build.ps1 -Target qemu-uefi-test
```

`-Distro` defaults to `Ubuntu-26.04`; check `wsl -l -v` if your distribution has
another name.

## Windows: native host simulator

`build-win.bat` locates Visual Studio Build Tools, calls `vcvars64.bat`, and
builds with `clang-cl`:

```powershell
.\build-win.bat
.\build\finte.exe test
.\build\finte.exe selfcheck
.\build\finte.exe demo
.\build\finte.exe compat
.\build\finte.exe bench
```

The Windows host uses **Win32 fibers**; Linux uses **`ucontext`**. The core,
tests and ABI are identical.

## Image layout

`make image` creates a hybrid raw image:

```
LBA 0        own MBR boot sector
LBA 1..32    BIOS stage2 (CHS loader)
LBA 33       BIOS header
LBA 34..     flat kernel (BIOS copy)
...          ramdisk (BIOS copy)
LBA 2048..   EFI System Partition (FAT32)
               EFI/BOOT/BOOTX64.EFI
               EFI/FINTE/KERNEL.BIN
               EFI/FINTE/RAMDISK.BIN
```

- The kernel is linked at **64 MiB**.
- BIOS stage2 reads with CHS INT 13h one sector at a time, copies the kernel to
  64 MiB, builds a 1 GiB identity map with 2 MiB pages, and enters long mode.
- UEFI loader is a PE32+ application built with `clang` and `lld-link`. It reads
  the FAT32 ESP with `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, allocates a 36 MiB
  kernel region at 64 MiB, fills `fboot_info`, reads the ramdisk, and jumps to
  the flat kernel. It also obtains GOP framebuffer information, the firmware IDT
  descriptor, and the firmware Block I/O device list.
- The ramdisk contains `init.elf` and `ipc.elf` (the isolated ring-3 processes)
in addition to the text
  files packed from `src/ramdisk/`.
- The kernel uses its own VGA text console on BIOS and the GOP framebuffer on
  UEFI; serial is mirrored on COM1. Storage and input may call firmware services
  through the saved IDT; console output never does.

## GUI

Use the Windows QEMU launcher:

```powershell
.\run-gui.ps1 -Mode uefi
.\run-gui.ps1 -Mode uefi -CPUs 4
.\run-gui.ps1 -Mode uefi -Accel whpx
.\run-gui.ps1 -Mode bios
.\run-gui.ps1 -Mode uefi -TestDisk
.\run-gui.ps1 -Mode uefi -FullScreen
```

`-Accel auto` (the default) probes WHPX and falls back to TCG. `-Accel tcg`
forces software emulation; `-Accel whpx` requires WHPX to be available.

`-TestDisk` builds `build/disk-test.img` if needed and attaches it as the
second disk (`hd1`).

The launcher rebuilds `build/finte.img`, stages it and OVMF under
`%LOCALAPPDATA%\FinteGUI` because QEMU for Windows cannot reliably open the
project path (it contains spaces and Chinese characters), and opens a QEMU
window. Click once inside the window to capture keyboard focus; the shell
accepts PS/2, UEFI ConIn (USB HID), and serial input. `PageUp`/`PageDown`,
`Shift+Up/Down`, `Home`/`End` and the mouse wheel scroll the last 512 console
lines; typing returns to the prompt. `shutdown` exits QEMU.
WHPX is used automatically when available; install/enable **Windows Hypervisor
Platform** for hardware acceleration, otherwise the launcher falls back to TCG.
WHPX launches use the **SDL** frontend (GTK can stall before guest start on some
WHPX setups); TCG uses GTK. The launcher switches the SDL window to the US
keyboard layout so the Windows IME cannot consume letter keys. Direct PS/2 is
the default for low typing latency; `-UsbKeyboard` opts into a virtual USB
keyboard through UEFI ConIn. QEMU diagnostics go to
`%LOCALAPPDATA%\FinteGUI\qemu.log`, so benign firmware and WHPX warnings do not
clutter the console.

## Write to USB

Use Rufus and choose **DD Image mode** for `build/finte.img`. See
[bootloader.md](bootloader.md) for Secure Boot/CSM details.

**Target the USB device only.** Finte does not install itself. Mounted lfc1
partitions are writable; NTFS and ext4 are read-only.

## QEMU

BIOS:

```bash
make qemu
# or
qemu-system-x86_64 -m 256M \
  -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
  -boot c -display none -serial stdio -monitor none -no-reboot
```

UEFI:

```bash
make qemu-uefi
```

The UEFI target copies `OVMF_VARS_4M.fd` to `build/OVMF_VARS.fd` and uses the
read-only OVMF code image. It boots the same `finte.img` through the FAT32 ESP.

## Disk tests

`make disk-test` builds a 64 MiB MBR test disk with lfc1, ext4 and NTFS
partitions, then reads all three through the host simulator. It needs
`mkfs.ext4` and the `ntfs-3g` tools (`mkntfs`, `ntfscp`).

```bash
make disk-test
make qemu-disk-test       # BIOS / ATA PIO
make qemu-uefi-disk-test  # UEFI / AHCI
```

The QEMU targets attach `build/disk-test.img` as the second IDE/SATA disk and
mount `hd1p1` (lfc1), `hd1p2` (ext4) and `hd1p3` (NTFS) in turn. The lfc1
partition is writable; `write`, `mkdir`, `cp` and `sync` are exercised there.

Writable lfc1 and the UEFI firmware path can also be tested with an NVMe image:

```bash
python3 tools/mklfc1.py --source build/lfc-src --out build/nvme-test.img \
  --label NVME_LFC1 --size 50331648
qemu-system-x86_64 -m 256M -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
  -drive format=raw,file=build/finte.img,if=ide,index=0,media=disk \
  -drive format=raw,file=build/nvme-test.img,if=none,id=nvme0 \
  -device nvme,drive=nvme0,serial=finte0
```

The NVMe disk appears as `hd1`; firmware Block I/O is slow (about 2 seconds per
operation under QEMU), so allow time for `mount`, `sync` and `umount`.

## Startup checks

Before the shell starts, the kernel runs:

- boot protocol (`custom BIOS loader` / `custom UEFI loader` / Multiboot fallback)
- boot info magic and ramdisk bounds
- long mode: PE/PG, PAE, LME/LMA
- GDT/CS validity
- IDT with 32 exception gates, timer and reschedule gates, and an `int 0x80` gate
- 64 MiB kernel mapping
- monotonic TSC
- page-aligned, writable heap
- shared core self-check: 6 sections, 147 checks

Any failure prints the failing check/section and halts. The shell is only
started after `startup: PASS`.

## Troubleshooting

**UEFI boot fails at `kernel allocation at 64 MiB`**  
The firmware does not have a free 36 MiB contiguous region at 64 MiB. QEMU/OVMF
works. Some firmware reserves that area; the kernel is linked at 64 MiB, so the
loader must allocate exactly there.

**UEFI boot works in QEMU but the laptop screen stays blank**  
Check the serial log for `UEFI: gop mode` and `fb`. If GOP is absent or has an
unsupported format, the kernel falls back to serial. The loader prints its
status through ConOut before handoff.

**Keyboard does not respond**  
Click once inside the QEMU window to capture keyboard focus. If it still does
not respond, rebuild the image (`run-gui.ps1` does this automatically); an older
image could starve the PS/2 path when no serial backend exists. Under UEFI the
loader records whether the firmware ConIn handle is USB: USB keyboards use
ConIn, PS/2-only systems use direct i8042 polling, and serial is always polled
first. BIOS builds use PS/2 and serial.

**`qemu: Failed to get "write" lock`**  
Another QEMU instance still has `build/finte.img` open. Close it; do not run the
BIOS and UEFI tests concurrently.

**Disk test fails building the image**  
Install `ntfs-3g` (for `mkntfs`/`ntfscp`) and `e2fsprogs` (for `mkfs.ext4`).
The host disk test skips no steps: all three filesystems must build.

**Secure Boot rejects `BOOTX64.EFI`**  
The loader is unsigned. Disable Secure Boot or sign the PE image.

**`make clean` says `Permission denied` on `/mnt/d/.../build`**  
A Windows process is holding the directory. Close QEMU/editors or remove
`build` from Windows.

## Test matrix

- WSL Ubuntu 26.04 / GCC 15 / QEMU 10.2 SeaBIOS: 19 host tests, BIOS CLI boot
  test, kernel POST, USB mass-storage boot, VGA screenshot, PS/2 keyboard,
  timer preemption (`preempt=OK`), ring-3 `user`/`procs`, 4-CPU SMP boot.
- 4-CPU scheduler: `selfcheck` 147/147, `demo`, `preempt`, `compat`, `smp`,
  `procs`; AP LAPIC ticks and context switches advance.
- 3-strike watchdog: `watchdog test` ×3 prints a panic and powers off;
  `selfcheck` and the expected `procs` fault leave 0 strikes.
- Host disk manager: lfc1/ext4/NTFS on a three-partition MBR image, nested
  directories, 20 KiB/40 KiB read checksums, GPT listing and reads.
- Writable lfc1: create, write, mkdir, cp, sync, umount, remount and read back;
  `lfc1ck.py` validation and `--compact`.
- BIOS disk manager: ATA PIO, MBR, mount/read/fsinfo/umount for lfc1, ext4 and
  NTFS from `hd1`; writable lfc1.
- WSL Ubuntu 26.04 / OVMF 2025.11: UEFI CLI boot test, USB mass-storage boot,
  GOP 1280x800 framebuffer, USB HID and PS/2 input, timer preemption, ring-3
  `user`/`procs`, 4-CPU SMP, screenshot.
- UEFI disk manager: AHCI, MBR, mount/read/fsinfo/umount for lfc1, ext4 and
  NTFS from `hd1`; writable lfc1.
- UEFI firmware paths: NVMe and USB mass-storage Block I/O read/write/sync/
  remount; USB HID ConIn with no duplicate keys.
- Windows 11 QEMU GTK GUI: UEFI boot, 1280x800 window, shell screenshot.
- Windows 11 QEMU + WHPX: 4-CPU boot, UEFI disk write/read, real keyboard
  input, `preempt` ~135 ms (vs ~397 ms TCG).
- Windows 11 / clang-cl 23 / MSVC Build Tools: native host build, 19 tests,
  self-check, demo, compat, benchmark.

Physical laptop validation is still pending; the image is built for both BIOS
and UEFI USB boot.
