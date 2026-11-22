# Finte build.
#   make all        host simulator + bootable x86_64 kernel
#   make test       core self-tests
#   make bench      microbenchmarks
#   make qemu       boot the kernel in QEMU (serial on stdio)
#   make qemu-test  boot the kernel and verify the log

CC       ?= gcc
LD       ?= ld
OBJCOPY  ?= objcopy
QEMU     ?= qemu-system-x86_64
SMP      ?= 4
ACCEL    ?= $(shell if [ -w /dev/kvm ]; then echo "-accel kvm"; else echo "-accel tcg"; fi)
CLANG    ?= clang
LLD_LINK ?= lld-link
ARM64_CC ?= clang
ARM64_LD ?= aarch64-linux-gnu-ld
ARM64_TARGET ?= aarch64-none-elf
QEMU_ARM64 ?= qemu-system-aarch64
PYTHON   ?= python3
BUILD    ?= build
$(shell mkdir -p $(BUILD))
VERSION_HEADER := $(BUILD)/finte_version.h

HOST_CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Isrc/include -I$(BUILD)
KCFLAGS := -m64 -march=x86-64 -mtune=generic -std=c11 -O2 -g -ffreestanding \
           -fno-builtin -fno-pic -fno-pie \
           -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 \
           -mgeneral-regs-only -Wall -Wextra -Isrc/include -I$(BUILD)
UEFI_CFLAGS := -target x86_64-pc-windows-msvc -c -ffreestanding \
               -fno-stack-protector -Isrc/include
UEFI_ARM64_CFLAGS := -target aarch64-pc-windows-msvc -c -ffreestanding \
                     -fno-stack-protector -Isrc/include
USER_CFLAGS := -m64 -march=x86-64 -mtune=generic -std=c11 -O2 -ffreestanding \
               -fno-builtin -fno-pic \
               -fno-pie -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 \
               -mcmodel=large -Wall -Wextra
ARM64_CFLAGS := --target=$(ARM64_TARGET) -std=c11 -O2 -g -ffreestanding \
                -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
                -fno-unwind-tables -fno-asynchronous-unwind-tables \
                -mno-outline-atomics -mgeneral-regs-only -mstrict-align \
                -Wall -Wextra -Isrc/include -I$(BUILD)
ARM64_USER_CFLAGS := --target=$(ARM64_TARGET) -std=c11 -O2 -ffreestanding \
                     -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
                     -fno-unwind-tables -fno-asynchronous-unwind-tables \
                     -mno-outline-atomics -mgeneral-regs-only -mstrict-align \
                     -Wall -Wextra
USER_PROGS := init ipc abi fork exec signal kill cap bad svc svc_client dyn wait exit block
USER_ELF := $(addprefix $(BUILD)/,$(addsuffix .elf,$(USER_PROGS)))
ARM64_USER_ELF := $(addprefix $(BUILD)/arm64/,$(addsuffix .elf,$(USER_PROGS)))

HEADERS := $(wildcard src/include/finte/*.h)
CORE_SRC := src/core/util.c src/core/object.c src/core/cap.c src/core/arena.c src/core/channel.c \
            src/core/flow.c src/core/sched.c src/core/verb.c src/core/compat_linux.c \
            src/core/demo.c src/core/guest_linux.c src/core/selftest.c src/core/initrd.c \
            src/core/disk.c src/core/lfc1.c src/core/ext4.c src/core/ntfs.c src/core/file.c \
            src/core/lock.c src/core/cas.c src/core/ansi.c src/core/watchdog.c \
            src/core/bootinfo.c src/core/pmm.c src/core/process.c src/core/service.c
HOST_SRC := $(CORE_SRC) src/host/hal_host.c src/host/blk_host.c src/host/process_host.c \
            src/host/main.c src/host/bench.c src/tests/test_main.c

KERNEL_SRC := $(CORE_SRC) src/arch/x86_64/kmain.c src/arch/x86_64/hal_x86.c \
              src/arch/x86_64/faults.c src/arch/x86_64/post.c src/arch/x86_64/libc.c \
              src/arch/x86_64/vga.c src/arch/x86_64/fb.c \
              src/arch/x86_64/input.c src/arch/x86_64/scroll.c \
              src/arch/x86_64/blk.c src/arch/x86_64/timer.c src/arch/x86_64/usermode.c \
              src/arch/x86_64/process.c src/arch/x86_64/smp.c src/arch/x86_64/shell.c \
              $(BUILD)/ap_trampoline_bin.c
KERNEL_ASM := src/arch/x86_64/context.S src/arch/x86_64/faults.S src/arch/x86_64/entry64.S \
              src/arch/x86_64/usermode.S src/arch/x86_64/timer.S
KERNEL_OBJ := $(patsubst %.c,$(BUILD)/kobj/%.o,$(KERNEL_SRC)) \
              $(patsubst %.S,$(BUILD)/kobj/asm/%.o,$(KERNEL_ASM))

ARM64_SRC := $(CORE_SRC) src/arch/arm64/kmain_arm64.c src/arch/arm64/hal_arm64.c \
             src/arch/arm64/mmu.c src/arch/arm64/gic.c src/arch/arm64/process.c \
             src/arch/arm64/virtio_blk.c src/arch/arm64/string.c
ARM64_ASM := src/arch/arm64/entry.S src/arch/arm64/context.S \
             src/arch/arm64/ap_entry.S src/arch/arm64/usermode.S
ARM64_OBJ := $(patsubst %.c,$(BUILD)/arm64/obj/%.o,$(ARM64_SRC)) \
             $(patsubst %.S,$(BUILD)/arm64/obj/%.o,$(ARM64_ASM)) \
             $(BUILD)/arm64/obj/src/arch/arm64/initrd_blob.o

.PHONY: all host kernel kernel-elf image uefi bootloader initrd test selfcheck \
        bench demo compat qemu qemu-uefi qemu-uefi-test qemu-kernel qemu-test \
        disk-image disk-test qemu-disk-test qemu-uefi-disk-test user clean FORCE \
        arm64 arm64-test arm64-uefi-test arm64-disk-test arm64-uefi-disk-test \
        abi-test qemu-arm64 images iso

$(VERSION_HEADER): FORCE
	@mkdir -p $(BUILD)
	$(PYTHON) tools/gen_version.py $@

FORCE:

arm64: $(BUILD)/finte-arm64.elf

$(BUILD)/arm64/obj/%.o: %.c $(HEADERS) $(VERSION_HEADER)
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CFLAGS) -c $< -o $@

$(BUILD)/arm64/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(ARM64_CC) --target=$(ARM64_TARGET) -c $< -o $@

$(BUILD)/arm64/%.elf: src/user/%.c src/user/syscall.h src/user/user_arm64.ld
	@mkdir -p $(BUILD)/arm64
	$(ARM64_CC) $(ARM64_USER_CFLAGS) -c $< -o $(BUILD)/arm64/$*.o
	$(ARM64_LD) -T src/user/user_arm64.ld -o $@ $(BUILD)/arm64/$*.o

$(BUILD)/arm64/libsvc.so: src/user/libsvc.c src/user/libsvc_arm64.ld
	@mkdir -p $(BUILD)/arm64
	$(ARM64_CC) $(ARM64_USER_CFLAGS) -fPIC -c src/user/libsvc.c \
	  -o $(BUILD)/arm64/libsvc.o
	$(ARM64_LD) -shared -nostdlib -T src/user/libsvc_arm64.ld \
	  -o $@ $(BUILD)/arm64/libsvc.o

$(BUILD)/arm64/initrd.bin: $(ARM64_USER_ELF) $(BUILD)/arm64/libsvc.so \
                           tools/mkinitrd.py
	$(PYTHON) tools/mkinitrd.py --dir src/ramdisk --out $@ \
	  $(foreach p,$(USER_PROGS),--extra $(p).elf=$(BUILD)/arm64/$(p).elf) \
	  --extra libsvc.so=$(BUILD)/arm64/libsvc.so

$(BUILD)/arm64/initrd_blob.c: $(BUILD)/arm64/initrd.bin tools/mkbinarray.py
	$(PYTHON) tools/mkbinarray.py $< $@ initrd_bin

$(BUILD)/arm64/obj/src/arch/arm64/initrd_blob.o: $(BUILD)/arm64/initrd_blob.c
	@mkdir -p $(dir $@)
	$(ARM64_CC) $(ARM64_CFLAGS) -c $< -o $@

$(BUILD)/finte-arm64.elf: $(ARM64_OBJ) src/arch/arm64/kernel.ld
	$(ARM64_LD) -T src/arch/arm64/kernel.ld -o $@ $(ARM64_OBJ)

arm64-test: arm64
	@rm -f $(BUILD)/arm64.log
	timeout 40 $(QEMU_ARM64) -M virt -cpu cortex-a57 -m 256M -smp 4 \
	  -nographic -monitor none -no-reboot \
	  -kernel $(BUILD)/finte-arm64.elf > $(BUILD)/arm64.log 2>&1 || true
	@if grep -q "arm64 self-check: PASS" $(BUILD)/arm64.log && \
	    grep -Eq "mmu: (39|48)-bit identity map enabled" $(BUILD)/arm64.log && \
	    grep -q "mmu: identity translations ok" $(BUILD)/arm64.log && \
	    grep -q "arm64 timer: gic=v2 ticks=[1-9]" $(BUILD)/arm64.log && \
	    grep -q "arm64 smp: 3 application processor(s) online, 4 cpu(s) total" $(BUILD)/arm64.log && \
	    grep -q "arm64 smp: hits=2/2/2/2" $(BUILD)/arm64.log && \
	    grep -q "arm64 user: init.elf PASS" $(BUILD)/arm64.log && \
	    grep -q "arm64 procs: PASS" $(BUILD)/arm64.log && \
	    grep -q "arm64 abi: PASS" $(BUILD)/arm64.log && \
	    grep -Fq "abi] space=OK user=OK fork=OK wait=OK exec=OK signal=OK cap=OK service=OK bad=OK dyn=OK churn=OK blocked=OK" $(BUILD)/arm64.log && \
	    grep -Fq "procs] ipc=OK isolation=OK shm=OK arena=OK time=OK preempt=OK fault=OK" $(BUILD)/arm64.log; then \
	  echo "arm64 boot test: OK"; \
	else \
	  echo "arm64 boot test: FAILED"; tail -80 $(BUILD)/arm64.log; exit 1; \
	fi

arm64-uefi-test: $(BUILD)/finte-arm64.iso
	AAVMF_CODE=$(AAVMF_CODE) AAVMF_VARS=$(AAVMF_VARS) \
	  bash tools/qemu_arm64_uefi_test.sh

abi-test: $(BUILD)/finte-bios.img
	bash tools/qemu_abi_test.sh

arm64-disk-test: arm64 $(BUILD)/disk-test.img
	QEMU_ARM64=$(QEMU_ARM64) \
	  bash tools/qemu_arm64_disk_test.sh direct

arm64-uefi-disk-test: $(BUILD)/finte-arm64.iso $(BUILD)/disk-test.img
	QEMU_ARM64=$(QEMU_ARM64) AAVMF_CODE=$(AAVMF_CODE) AAVMF_VARS=$(AAVMF_VARS) \
	  bash tools/qemu_arm64_disk_test.sh uefi

qemu-arm64: arm64
	$(QEMU_ARM64) -M virt -cpu cortex-a57 -m 256M -smp 4 \
	  -nographic -monitor none -no-reboot \
	  -kernel $(BUILD)/finte-arm64.elf

all: host image

host: $(BUILD)/finte

kernel: $(BUILD)/kernel64.bin

kernel-elf: $(BUILD)/finte.elf

image: $(BUILD)/finte.img

images: $(BUILD)/finte-x86_64.img $(BUILD)/finte-x86_64.iso \
        $(BUILD)/finte-arm64.img $(BUILD)/finte-arm64.iso

iso: $(BUILD)/finte-x86_64.iso $(BUILD)/finte-arm64.iso

uefi: $(BUILD)/uefi/BOOTX64.EFI $(BUILD)/uefi/BOOTAA64.EFI

bootloader: $(BUILD)/boot.bin $(BUILD)/stage2.bin

initrd: $(BUILD)/initrd.bin

user: $(USER_ELF) $(BUILD)/libsvc.so

$(BUILD)/finte: $(HOST_SRC) $(HEADERS) $(VERSION_HEADER)
	@mkdir -p $(BUILD)
	$(CC) $(HOST_CFLAGS) $(HOST_SRC) -o $@

$(BUILD)/kobj/%.o: %.c $(HEADERS) $(VERSION_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/kobj/asm/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -m64 -c $< -o $@

$(BUILD)/kernel64.elf: $(KERNEL_OBJ) src/arch/x86_64/kernel64.ld
	$(LD) -m elf_x86_64 -T src/arch/x86_64/kernel64.ld -o $@ $(KERNEL_OBJ)

$(BUILD)/kernel64.bin: $(BUILD)/kernel64.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/ap_trampoline.bin: src/arch/x86_64/ap_trampoline.S src/arch/x86_64/ap_trampoline.ld
	@mkdir -p $(BUILD)
	$(CC) -m64 -c src/arch/x86_64/ap_trampoline.S -o $(BUILD)/ap_trampoline.o
	$(LD) -m elf_x86_64 -T src/arch/x86_64/ap_trampoline.ld \
	  -o $(BUILD)/ap_trampoline.elf $(BUILD)/ap_trampoline.o
	$(OBJCOPY) -O binary $(BUILD)/ap_trampoline.elf $@

$(BUILD)/ap_trampoline_bin.c: $(BUILD)/ap_trampoline.bin tools/mkbinarray.py
	$(PYTHON) tools/mkbinarray.py $< $@ ap_trampoline_bin

$(BUILD)/boot32.o: src/arch/x86_64/boot32.S $(BUILD)/kernel64.bin
	$(CC) -m32 -c $< -o $@

$(BUILD)/finte.elf: $(BUILD)/boot32.o src/arch/x86_64/boot32.ld $(BUILD)/kernel64.bin
	$(LD) -m elf_i386 -T src/arch/x86_64/boot32.ld -o $@ $(BUILD)/boot32.o

$(BUILD)/boot.bin: src/boot/boot.S src/boot/boot.ld
	$(CC) -m32 -c $< -o $(BUILD)/boot.o
	$(LD) -m elf_i386 -T src/boot/boot.ld -o $@ $(BUILD)/boot.o

$(BUILD)/stage2.bin: src/boot/stage2.S src/boot/stage2.ld
	$(CC) -m32 -c $< -o $(BUILD)/stage2.o
	$(LD) -m elf_i386 -T src/boot/stage2.ld -o $@ $(BUILD)/stage2.o

$(BUILD)/uefi/loader.obj: src/uefi/loader.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CLANG) $(UEFI_CFLAGS) $< -o $@

$(BUILD)/uefi/jump.obj: src/uefi/jump.S
	@mkdir -p $(dir $@)
	$(CLANG) -target x86_64-pc-windows-msvc -c $< -o $@

$(BUILD)/uefi/BOOTX64.EFI: $(BUILD)/uefi/loader.obj $(BUILD)/uefi/jump.obj
	$(LLD_LINK) /nologo /subsystem:efi_application /entry:efi_main \
	  /nodefaultlib /out:$@ $^

$(BUILD)/initrd.bin: $(wildcard src/ramdisk/*) $(USER_ELF) \
                     $(BUILD)/libsvc.so tools/mkinitrd.py
	@mkdir -p $(BUILD)
	$(PYTHON) tools/mkinitrd.py --dir src/ramdisk --out $@ \
	  $(foreach p,$(USER_PROGS),--extra $(p).elf=$(BUILD)/$(p).elf) \
	  --extra libsvc.so=$(BUILD)/libsvc.so

$(BUILD)/libsvc.so: src/user/libsvc.c src/user/libsvc_x86.ld
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -fPIC -c src/user/libsvc.c -o $(BUILD)/libsvc.o
	$(LD) -m elf_x86_64 -shared -nostdlib -T src/user/libsvc_x86.ld \
	  -o $@ $(BUILD)/libsvc.o

$(BUILD)/%.elf: src/user/%.c src/user/syscall.h src/user/user.ld
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $(BUILD)/$*.o
	$(LD) -m elf_x86_64 -T src/user/user.ld -o $@ $(BUILD)/$*.o

$(BUILD)/finte.img: $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel64.bin \
                    $(BUILD)/initrd.bin $(BUILD)/uefi/BOOTX64.EFI \
                    tools/mkimage.py tools/mkfat32.py
	$(PYTHON) tools/mkimage.py --boot $(BUILD)/boot.bin \
	  --stage2 $(BUILD)/stage2.bin --kernel $(BUILD)/kernel64.bin \
	  --ramdisk $(BUILD)/initrd.bin --efi $(BUILD)/uefi/BOOTX64.EFI \
	  --out $@

# A BIOS-only El Torito hard-disk image. It contains the same stage-1,
# stage-2, kernel and ramdisk as finte.img, with a small MBR partition whose
# CHS geometry fits below the EBDA.
$(BUILD)/finte-bios.img: $(BUILD)/boot.bin $(BUILD)/stage2.bin \
                         $(BUILD)/kernel64.bin $(BUILD)/initrd.bin \
                         tools/mkimage.py
	$(PYTHON) tools/mkimage.py --boot $(BUILD)/boot.bin \
	  --stage2 $(BUILD)/stage2.bin --kernel $(BUILD)/kernel64.bin \
	  --ramdisk $(BUILD)/initrd.bin --esp-size-mb 0 --pad-to 294912 \
	  --mbr-chs 0,1,0,1,18,15 --out $@

# FAT16 ESPs used inside ISO images. FAT16 keeps the El Torito image below
# the 16-bit sector-count limit.
$(BUILD)/esp-x86.iso.img: $(BUILD)/uefi/BOOTX64.EFI $(BUILD)/kernel64.bin \
                          $(BUILD)/initrd.bin tools/mkesp.py
	$(PYTHON) tools/mkesp.py --out $@ --size-mb 16 \
	  --add EFI/BOOT/BOOTX64.EFI=$(BUILD)/uefi/BOOTX64.EFI \
	  --add EFI/FINTE/KERNEL.BIN=$(BUILD)/kernel64.bin \
	  --add EFI/FINTE/RAMDISK.BIN=$(BUILD)/initrd.bin

$(BUILD)/uefi/loader_arm64.obj: src/uefi/loader_arm64.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CLANG) $(UEFI_ARM64_CFLAGS) $< -o $@

$(BUILD)/uefi/BOOTAA64.EFI: $(BUILD)/uefi/loader_arm64.obj
	$(LLD_LINK) /nologo /subsystem:efi_application /entry:efi_main \
	  /nodefaultlib /machine:arm64 /out:$@ $^

$(BUILD)/esp-arm64.img: $(BUILD)/uefi/BOOTAA64.EFI $(BUILD)/finte-arm64.elf \
                        tools/mkesp.py
	$(PYTHON) tools/mkesp.py --out $@ --size-mb 16 \
	  --add EFI/BOOT/BOOTAA64.EFI=$(BUILD)/uefi/BOOTAA64.EFI \
	  --add EFI/FINTE/KERNEL.ELF=$(BUILD)/finte-arm64.elf

# Architecture-labelled disk images. x86_64 keeps the hybrid BIOS/UEFI image;
# arm64 wraps the FAT16 ESP used by the ISO in an MBR.
$(BUILD)/finte-x86_64.img: $(BUILD)/finte.img
	cp $< $@

$(BUILD)/finte-arm64.img: $(BUILD)/esp-arm64.img tools/mkdiskimg.py
	$(PYTHON) tools/mkdiskimg.py --esp $(BUILD)/esp-arm64.img --out $@

$(BUILD)/finte-x86_64.iso: $(BUILD)/finte-bios.img $(BUILD)/esp-x86.iso.img \
                           tools/mkiso.py
	$(PYTHON) tools/mkiso.py --arch x86_64 --bios $(BUILD)/finte-bios.img \
	  --efi $(BUILD)/esp-x86.iso.img --out $@

$(BUILD)/finte-arm64.iso: $(BUILD)/esp-arm64.img tools/mkiso.py
	$(PYTHON) tools/mkiso.py --arch arm64 --efi $(BUILD)/esp-arm64.img \
	  --out $@

test: host
	./$(BUILD)/finte test

selfcheck: host
	./$(BUILD)/finte selfcheck

bench: host
	./$(BUILD)/finte bench

demo: host
	./$(BUILD)/finte demo

compat: host
	./$(BUILD)/finte compat

qemu: image
	-$(QEMU) -m 256M -smp $(SMP) $(ACCEL) -drive format=raw,file=$(BUILD)/finte.img,if=ide,index=0,media=disk \
	  -boot c -display none -serial stdio -monitor none -no-reboot \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04

OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
AAVMF_CODE ?= /usr/share/AAVMF/AAVMF_CODE.fd
AAVMF_VARS ?= /usr/share/AAVMF/AAVMF_VARS.fd

$(BUILD)/OVMF_VARS.fd: $(OVMF_VARS)
	cp $< $@

qemu-uefi: image $(BUILD)/OVMF_VARS.fd
	-$(QEMU) -m 256M -smp $(SMP) $(ACCEL) -machine q35 \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=$(BUILD)/finte.img,if=ide,index=0,media=disk \
	  -boot c -display none -serial stdio -monitor none -no-reboot \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04

qemu-uefi-test: image $(BUILD)/OVMF_VARS.fd
	bash tools/qemu_uefi_test.sh

qemu-kernel: kernel-elf
	-$(QEMU) -m 256M -kernel $(BUILD)/finte.elf -display none \
	  -serial stdio -monitor none -no-reboot \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04

qemu-test: image
	bash tools/qemu_test.sh

$(BUILD)/disk-test.img: tools/mkdisk_test.py tools/mklfc1.py
	$(PYTHON) tools/mkdisk_test.py --out $@

disk-image: $(BUILD)/disk-test.img

disk-test: host $(BUILD)/disk-test.img
	bash tools/disk_test.sh

qemu-disk-test: image $(BUILD)/disk-test.img
	bash tools/qemu_disk_test.sh bios

qemu-uefi-disk-test: image $(BUILD)/disk-test.img $(BUILD)/OVMF_VARS.fd
	bash tools/qemu_disk_test.sh uefi

clean:
	rm -rf $(BUILD)
