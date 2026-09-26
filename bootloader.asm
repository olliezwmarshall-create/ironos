[org 0x7C00]
[bits 16]
    

    mov [BOOT_DRIVE], dl

    mov [BOOT_DRIVE], dl

    ; Setup stack
    xor ax, ax
    mov es, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Load kernel from disk (sectors 2 onwards)
    mov ah, 0x02        ; BIOS read sector function
    mov al, 15          ; Read 10 sectors
    mov ch, 0           ; Cylinder 0
    mov dh, 0           ; Head 0
    mov cl, 2           ; Start at sector 2
    mov bx, 0x1000      ; Load to memory address 0x1000
    int 0x13
    jc disk_error
    

    ; Switch to 32-bit Protected Mode
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump into protected mode code segment
    jmp 0x08:init_pm

[bits 32]
init_pm:
    mov ax, 0x10        ; Data segment descriptor
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x90000
    mov esp, ebp
    

    ; Absolute jump to kernel entry
    mov eax, 0x1000
    call eax

hang:
    jmp $

disk_error:
    jmp $

; Global Descriptor Table (GDT)
gdt_start:
gdt_null:
    dd 0
    dd 0

gdt_code:
    dw 0xffff
    dw 0
    db 0
    db 10011010b
    db 11001111b
    db 0

gdt_data:
    dw 0xffff
    dw 0
    db 0
    db 10010010b
    db 11001111b
    db 0

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BOOT_DRIVE: db 0

times 510 - ($ - $$) db 0
dw 0xAA55