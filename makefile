# Variables
ASM = nasm
CC = gcc
LD = ld
CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -Os

all: os.img

# Combine bootloader and kernel into a single floppy disk image
os.img: bootloader.bin kernel.bin
	cat bootloader.bin kernel.bin > os.img

# Compile the 16-bit bootloader
bootloader.bin: bootloader.asm
	$(ASM) -f bin bootloader.asm -o bootloader.bin

# Link the kernel objects into a raw binary
# Link the kernel objects into a temporary file, then convert to raw binary using objcopy
kernel.bin: entry.o kernel.o linker.ld
	$(LD) -m i386pe -T linker.ld -o kernel.tmp entry.o kernel.o
	objcopy -O binary kernel.tmp kernel.bin
	rm -f kernel.tmp

# Compile the 32-bit assembly entry stub
entry.o: entry.asm
	$(ASM) -f elf32 entry.asm -o entry.o

# Compile the C kernel with size optimization (-Os)
kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

# Run the OS in QEMU using floppy emulation
run: os.img
	qemu-system-x86_64 -fda os.img

# Clean up build artifacts
clean:
	rm -f *.bin *.o os.img