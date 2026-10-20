# Compiler and Tools
CC = gcc
AS = as
LD = ld
GRUB_MKRESCUE ?= grub-mkrescue
GRUB_MKIMAGE ?= grub-mkimage
GRUB_DIR ?= /usr/lib/grub/i386-pc
PYTHON ?= python3
# Native QEMU works in Linux/WSLg; retain the existing Windows installation as a fallback.
QEMU ?= $(shell command -v qemu-system-x86_64 2>/dev/null || command -v qemu-system-x86_64.exe 2>/dev/null || { test ! -f /mnt/d/mysys2/ucrt64/bin/qemu-system-x86_64.exe || echo /mnt/d/mysys2/ucrt64/bin/qemu-system-x86_64.exe; })
QEMU_FLAGS ?=
QEMU_MEMORY ?= 256
INSTALL_IMAGE = build/install.img
INSTALL_DISK ?= install-disk.img

# Flags for 32-bit standalone kernel development
CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pie -fno-pic -fno-tree-loop-distribute-patterns
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

C_SOURCES = $(wildcard *.c)
ASM_SOURCES = boot.s isr.s

C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.s=.o)
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

KERNEL = kernel.bin
ISO = ironos.iso

all: $(ISO)

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

# Rebuild C files when the shared header changes
%.o: %.c kernel.h fat.h blk.h
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJECTS) linker.ld
	$(LD) $(LDFLAGS) $(OBJECTS) -o $(KERNEL)

$(INSTALL_IMAGE): $(KERNEL) tools/mkinstall.py mkdisk.py $(shell find disk -type f -print | sed 's/ /\\ /g')
	$(PYTHON) tools/mkinstall.py --kernel $(KERNEL) --grub-dir "$(GRUB_DIR)" --grub-mkimage "$(GRUB_MKIMAGE)" --output $@

$(ISO): $(KERNEL) $(INSTALL_IMAGE) grub.cfg
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	cp $(INSTALL_IMAGE) iso/boot/install.img
	$(GRUB_MKRESCUE) -o $(ISO) iso

# The data disk (FAT16 image built from the files in disk/). Created once; 'make disk' rebuilds it from disk/.
DISK = disk.img

$(DISK):
	$(PYTHON) mkdisk.py $(DISK) disk

disk:
	rm -f $(DISK)
	$(MAKE) $(DISK)

# A separate, disposable target lets the live installer be tested without erasing disk.img.
$(INSTALL_DISK):
	$(PYTHON) -c 'import pathlib; p=pathlib.Path("$(INSTALL_DISK)"); f=p.open("xb"); f.truncate(128*1024*1024); f.close()'

# Override QEMU=/path/to/qemu-system-x86_64.exe for a different Windows installation.
# QEMU_FLAGS can select a display/monitor; source and image paths stay relative for WSL.
run: $(ISO) $(DISK) $(INSTALL_DISK)
	@test -n "$(QEMU)" || { echo 'QEMU not found. Install qemu-system-x86 or set QEMU=/mnt/c/.../qemu-system-x86_64.exe'; exit 1; }
	"$(QEMU)" -m $(QEMU_MEMORY) -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -drive file=$(INSTALL_DISK),format=raw,if=ide,index=1 -boot d -vga std $(QEMU_FLAGS)

# After live installation, verify the target boots without the live ISO or data disk.
run-installed: $(INSTALL_DISK)
	@test -n "$(QEMU)" || { echo 'QEMU not found; set QEMU=/path/to/qemu'; exit 1; }
	"$(QEMU)" -m $(QEMU_MEMORY) -drive file=$(INSTALL_DISK),format=raw,if=ide,index=0 -boot c -vga std $(QEMU_FLAGS)

test-install: $(ISO)
	$(PYTHON) tools/test_install.py --qemu "$(QEMU)"

test-desktop: $(ISO)
	$(PYTHON) tools/test_desktop.py --qemu "$(QEMU)"

test-installer-host:
	@set -e; test_bin=$$(mktemp /tmp/ironos-installer-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -fno-builtin -DHOST_TEST tools/test_installer.c -o "$$test_bin"; "$$test_bin"

.PHONY: all run run-installed clean disk test-install test-installer-host test-desktop

clean:
	rm -rf *.o $(KERNEL) $(ISO) iso build/install.img
