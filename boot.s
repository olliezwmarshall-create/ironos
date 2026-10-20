# Declare constants for the Multiboot header
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

# This section tells GRUB this is a valid Multiboot kernel
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

# Reserve a small stack for your kernel (16 KB)
.section .bss
.align 16
stack_bottom:
.skip 16384 
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    # 1. Set up the stack pointer
    mov $stack_top, %esp

    # 2. Ensure stack is 16-byte aligned (Crucial for GCC-compiled C code)
    and $-16, %esp

    # 3. Pass the multiboot magic (eax) and info pointer (ebx) to kernel_main(magic, mbi).
    #    sub 8 + two pushes = 16 bytes, so the stack stays 16-byte aligned at the call.
    sub $8, %esp
    push %ebx
    push %eax
    call kernel_main

    # 4. If kernel_main ever returns, loop forever
.hang:
    cli
    hlt
    jmp .hang

.section .note.GNU-stack,"",@progbits
