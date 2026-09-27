# Compiler and Tools
CC = gcc
AS = as
LD = ld
GRUB_MKRESCUE = grub-mkrescue

# Flags for 32-bit standalone kernel development
CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# Automatically find all C source files in the directory
C_SOURCES = $(wildcard *.c)
ASM_SOURCES = boot.s

# Generate object file lists from source files
C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.s=.o)
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# Output Names
KERNEL = kernel.bin
ISO = ironos.iso

all: $(ISO)

# Compile assembly boot stub
%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

# Compile C files automatically
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
run: $(ISO)
	/mnt/d/mysys2/ucrt64/bin/qemu-system-x86_64.exe -cdrom $(ISO) -vga std -display sdl

# Clean up build artifacts
clean:
	rm -rf *.o $(KERNEL) $(ISO) iso