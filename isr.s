# Low-level interrupt plumbing: GDT/IDT loading + ISR entry stubs.

.section .text

.global gdt_flush
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ljmp $0x08, $.gdt_reloaded
.gdt_reloaded:
    ret

.global idt_flush
idt_flush:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

# Stub for vectors where the CPU does NOT push an error code (we push a dummy 0)
.macro ISR_NOERR n
.global isr\n
isr\n:
    push $0
    push $\n
    jmp isr_common
.endm

# Stub for vectors where the CPU pushes an error code itself
.macro ISR_ERR n
.global isr\n
isr\n:
    push $\n
    jmp isr_common
.endm

.irp n, 0,1,2,3,4,5,6,7,9,15,16,18,19,20,22,23,24,25,26,27,28,31
    ISR_NOERR \n
.endr
.irp n, 8,10,11,12,13,14,17,21,29,30
    ISR_ERR \n
.endr
# IRQ0..15 remapped to vectors 32..47 (no error code)
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    ISR_NOERR \n
.endr

isr_common:
    pusha
    cld
    push %esp            # argument: struct registers *
    call isr_handler
    add $4, %esp
    popa
    add $8, %esp         # drop int_no + err_code
    iret

.section .data
.global isr_stub_table
isr_stub_table:
.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    .long isr\n
.endr

.section .note.GNU-stack,"",@progbits
