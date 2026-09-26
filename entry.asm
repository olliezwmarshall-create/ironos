bits 32
global start
extern _kernel_main

start:
    call _kernel_main
    jmp $