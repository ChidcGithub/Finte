#!/bin/bash
# Build the VMware bundle: UEFI ISO + BIOS VMDK + ready-to-open VMX files.
set -eu
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/vmware

cp build/finte-x86_64.iso build/vmware/finte-x86_64.iso
qemu-img convert -f raw -O vmdk -o subformat=monolithicSparse \
    build/finte-x86_64.img build/vmware/finte-x86_64.vmdk

cat > build/vmware/Finte-UEFI.vmx <<'EOF'
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
guestOS = "other-64"
displayName = "Finte UEFI"
firmware = "efi"
memsize = "512"
numvcpus = "2"
cpuid.coresPerSocket = "2"
ide1:0.present = "TRUE"
ide1:0.deviceType = "cdrom-image"
ide1:0.fileName = "finte-x86_64.iso"
ide1:0.startConnected = "TRUE"
floppy0.present = "FALSE"
ethernet0.present = "FALSE"
usb.present = "FALSE"
sound.present = "FALSE"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "serial.log"
serial0.tryNoRxLoss = "FALSE"
EOF

cat > build/vmware/Finte-BIOS.vmx <<'EOF'
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
guestOS = "other-64"
displayName = "Finte BIOS"
firmware = "bios"
memsize = "512"
numvcpus = "2"
cpuid.coresPerSocket = "2"
ide0:0.present = "TRUE"
ide0:0.deviceType = "disk"
ide0:0.fileName = "finte-x86_64.vmdk"
ide0:0.mode = "persistent"
floppy0.present = "FALSE"
ethernet0.present = "FALSE"
usb.present = "FALSE"
sound.present = "FALSE"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "serial.log"
serial0.tryNoRxLoss = "FALSE"
EOF

cat > build/vmware/README.md <<'EOF'
# Finte in VMware

## UEFI (recommended)

Open `Finte-UEFI.vmx` and power it on. At the `finte>` prompt run:

```
hwtest
```

Expected tail:

```
[hw] selfcheck=OK sections=6 checks=147 failures=0
[hw] procs=OK
[hw] user=OK
[hw] PASS
```

The same output is written to `serial.log` beside the VMX file.

## BIOS

If UEFI firmware is unavailable, open `Finte-BIOS.vmx`. It boots the
`finte-x86_64.vmdk` disk through the legacy BIOS/ATA path.

## Notes

- 512 MB RAM and 2 CPUs are enough; keep Secure Boot off.
- The console keyboard is the supported input path.
- The ISO's BIOS El Torito path is not reliable in all firmware; use the UEFI
  ISO or the BIOS VMDK.
- `abi` additionally needs a second raw block device for swap and is normally
  run by the QEMU harness.
EOF

ls -lh build/vmware
