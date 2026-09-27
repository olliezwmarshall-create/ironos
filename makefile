# Compiler and Tools
CC = gcc
AS = as
LD = ld
GRUB_MKRESCUE = grub-mkrescue

# Flags (Added -m32 for 32-bit architecture matching)
CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# Project Files
C_SOURCES = kernel.c
ASM_SOURCES = boot.s
OBJECTS = boot.o kernel.o

# Output Names
KERNEL = kernel.bin
ISO = ironos.iso

all: $(ISO)

# Compile assembly boot stub
%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

# Compile C kernel
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Link the kernel binary
$(KERNEL): $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) -o $(KERNEL)

# Build the bootable GRUB ISO
$(ISO): $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/kernel.bin
	cp grub.cfg iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(ISO) iso

# Run the ISO in QEMU
# Run the ISO in QEMU
run: $(ISO)
	/mnt/d/mysys2/ucrt64/bin/qemu-system-x86_64.exe -cdrom $(ISO) -vga std -display sdl

# Clean up build artifacts
clean:
	rm -rf *.o $(KERNEL) $(ISO) iso