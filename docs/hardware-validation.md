# Physical-hardware validation

Finte is verified under QEMU (SeaBIOS, OVMF, AAVMF, AHCI, virtio). A physical
run needs the same on-device checks, captured from serial or the VGA console.

## Build and boot media

```sh
make images
```

Write `build/finte.img` to a USB device (the disk image boots on BIOS and UEFI
x86_64). For arm64, write `build/finte-arm64.img`; current arm64 physical boot
still needs platform DTB support, so validated targets are x86_64 laptops and
desktops with AHCI/ATA storage and legacy or UEFI serial/PS2 input.

For VMware, build a ready-to-open bundle:

```sh
make vmware
```

Open `build/vmware/Finte-UEFI.vmx` (UEFI ISO) or
`build/vmware/Finte-BIOS.vmx` (legacy BIOS + VMDK). Both reach the shell, and
`serial.log` beside the VMX captures the run.

## On-device run

At the `finte>` prompt:

```
hwtest
```

Expected report:

```
[hw] validation start
[hw] selfcheck=OK sections=6 checks=147 failures=0
[hw] procs=OK
[hw] user=OK
[hw] cpus=N disks=N mounts=N
[hw] PASS
```

`hwtest` runs the 147 core checks, the four-process IPC/arena/preemption/fault
demo, and `init.elf` at ring 3. `disks`/`mounts` report the detected storage.
The shell `abi` command additionally runs the full process ABI (fd table,
demand paging, swap, fork/exec, signals, timers) but requires a second raw block
device (`hd1`) for the swap test.

## Checklist

- [ ] BIOS boot reaches the shell.
- [ ] UEFI boot reaches the shell.
- [ ] `hwtest` prints `[hw] PASS`.
- [ ] Keyboard and serial input both work.
- [ ] `disks`, `parts`, `mount`, `ls /mnt`, `cat /mnt/hello.txt` work with the
      machine's SATA/AHCI disk.
- [ ] `shutdown` powers the machine off.
- [ ] Capture the serial log and machine model/firmware version.

## Recorded results

| Platform | Firmware | Result | Notes |
| --- | --- | --- | --- |
| QEMU pc (SeaBIOS, AHCI) | SeaBIOS | PASS | `make qemu-test`, `make qemu-disk-test` |
| QEMU q35 (OVMF, AHCI) | OVMF | PASS | `make qemu-uefi-test`, `make qemu-uefi-disk-test` |
| QEMU virt (AAVMF) | AAVMF | PASS | `make arm64-test`, `make arm64-uefi-test` |
| Physical x86_64 | | pending | Run `hwtest` and record the serial log |
| Physical arm64 | | blocked | Needs DTB/ACPI platform discovery |
