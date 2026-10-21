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
# User-mode NAT needs no admin privileges; disable with QEMU_NETWORK=
QEMU_NIC ?= rtl8139
QEMU_NETWORK ?= -netdev user,id=net0 -device $(QEMU_NIC),netdev=net0,romfile=
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
	"$(QEMU)" -m $(QEMU_MEMORY) -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -drive file=$(INSTALL_DISK),format=raw,if=ide,index=1 -boot d -vga std $(QEMU_NETWORK) $(QEMU_FLAGS)

# After live installation, verify the target boots without the live ISO or data disk.
run-installed: $(INSTALL_DISK)
	@test -n "$(QEMU)" || { echo 'QEMU not found; set QEMU=/path/to/qemu'; exit 1; }
	"$(QEMU)" -m $(QEMU_MEMORY) -drive file=$(INSTALL_DISK),format=raw,if=ide,index=0 -boot c -vga std $(QEMU_NETWORK) $(QEMU_FLAGS)

test-install: $(ISO)
	$(PYTHON) tools/test_install.py --qemu "$(QEMU)"

test-screen:
	@set -e; test_bin=$$(mktemp /tmp/ironos-screen-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -fno-builtin -fsanitize=address -DHOST_TEST tools/test_screen.c -o "$$test_bin"; "$$test_bin"

test-mouse-host:
	@set -e; test_bin=$$(mktemp /tmp/ironos-mouse-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -fno-builtin -fsanitize=address,undefined -DHOST_TEST tools/test_mouse.c -o "$$test_bin"; "$$test_bin"

test-gui: $(ISO)
	$(PYTHON) tools/test_gui.py --qemu "$(QEMU)"
	$(PYTHON) tools/test_network_fixture.py --qemu "$(QEMU)" --gui

test-network-driver:
	@set -e; test_bin=$$(mktemp /tmp/ironos-nic-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -Wno-pointer-to-int-cast -fno-builtin -fsanitize=address,undefined -DHOST_TEST tools/test_rtl8139.c -o "$$test_bin"; "$$test_bin"

test-network-host:
	@set -e; test_bin=$$(mktemp /tmp/ironos-net-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -fno-builtin -fsanitize=address,undefined -DHOST_TEST tools/test_net.c -o "$$test_bin"; "$$test_bin"

test-network: $(ISO)
	$(PYTHON) tools/test_network.py --qemu "$(QEMU)"
	$(PYTHON) tools/test_network_fixture.py --qemu "$(QEMU)"

test-graphics: $(ISO)
	$(PYTHON) tools/test_graphics.py --qemu "$(QEMU)"

test-history: $(ISO)
	$(PYTHON) tools/test_history.py --qemu "$(QEMU)"

test-desktop: $(ISO)
	$(PYTHON) tools/test_desktop.py --qemu "$(QEMU)"

test-installer-host:
	@set -e; test_bin=$$(mktemp /tmp/ironos-installer-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -fno-builtin -DHOST_TEST tools/test_installer.c -o "$$test_bin"; "$$test_bin"

.PHONY: all run run-installed clean disk test-install test-installer-host test-desktop test-history test-graphics test-screen test-network test-network-host test-network-driver test-gui test-mouse-host

clean:
	rm -rf *.o $(KERNEL) $(ISO) iso build/install.img

# Choose the Intel Ethernet controller with: make run QEMU_NIC=e1000
test-calc:
	@set -e; test_bin=$$(mktemp /tmp/ironos-calc-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -fno-builtin -fsanitize=address,undefined -DHOST_TEST tools/test_calc.c -o "$$test_bin"; "$$test_bin"
.PHONY: test-calc

test-e1000-host:
	@set -e; test_bin=$$(mktemp /tmp/ironos-e1000-test.XXXXXX); trap 'rm -f "$$test_bin"' EXIT; $(CC) -std=gnu99 -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -fno-builtin -fsanitize=address,undefined -DHOST_TEST tools/test_e1000.c -o "$$test_bin"; "$$test_bin"
.PHONY: test-e1000-host
