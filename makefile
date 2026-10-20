# Compiler and Tools
CC = gcc
AS = as
LD = ld
GRUB_MKRESCUE = grub-mkrescue

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

$(KERNEL): $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $(KERNEL)

$(ISO): $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) iso

# The data disk (FAT16 image built from the files in disk/). Created once; 'make disk' rebuilds it from disk/.
DISK = disk.img

$(DISK):
	python3 mkdisk.py $(DISK) disk

disk:
	rm -f $(DISK)
	$(MAKE) $(DISK)

# -boot d: boot the CD first (the data disk has no boot code).
run: $(ISO) $(DISK)
	/mnt/d/mysys2/ucrt64/bin/qemu-system-x86_64.exe -cdrom $(ISO) -drive file=$(DISK),format=raw,if=ide,index=0 -boot d -vga std -display sdl

.PHONY: all run clean disk

clean:
	rm -rf *.o $(KERNEL) $(ISO) iso
