ENTRY(start)

SECTIONS
{
    . = 0x1000;

    .text : {
        entry.o(.text)
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
    }
}