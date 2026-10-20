#include "kernel.h"

struct idt_entry {
    unsigned short base_lo;
    unsigned short sel;
    unsigned char  always0;
    unsigned char  flags;
    unsigned short base_hi;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr ip;
static irq_handler_t handlers[256];

extern unsigned int isr_stub_table[];        // isr.s: addresses of stubs for vectors 0..47
extern void idt_flush(unsigned int ptr);     // isr.s

static const char *exception_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check", "Machine check",
    "SIMD FP exception", "Virtualization", "Control protection", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Security exception", "Reserved"
};

static void idt_set_gate(int n, unsigned int base) {
    idt[n].base_lo = base & 0xFFFF;
    idt[n].base_hi = (base >> 16) & 0xFFFF;
    idt[n].sel     = 0x08;    // kernel code segment
    idt[n].always0 = 0;
    idt[n].flags   = 0x8E;    // present, ring 0, 32-bit interrupt gate
}

// Move the PIC's IRQs to vectors 32..47 so they don't collide with CPU exceptions.
static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();   // ICW1: init, expect ICW4
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   // ICW2: master offset 32
    outb(0xA1, 0x28); io_wait();   // ICW2: slave offset 40
    outb(0x21, 0x04); io_wait();   // ICW3: slave on IRQ2
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();   // ICW4: 8086 mode
    outb(0xA1, 0x01); io_wait();

    outb(0x21, 0xFC);              // unmask IRQ0 (timer) + IRQ1 (keyboard) only
    outb(0xA1, 0xFF);
}

void register_interrupt_handler(int int_no, irq_handler_t handler) {
    handlers[int_no] = handler;
}

void interrupts_init(void) {
    for (int i = 0; i < 48; i++)
        idt_set_gate(i, isr_stub_table[i]);

    ip.limit = sizeof(idt) - 1;
    ip.base  = (unsigned int)&idt;
    idt_flush((unsigned int)&ip);

    pic_remap();
}

// Draw a red banner on the top row and stop. Writes VGA directly so it works
// even if the terminal state is corrupted.
static void panic(const char *what, unsigned int eip) {
    volatile unsigned char *vga = (unsigned char *)VGA_ADDRESS;
    const char *prefix = "EXCEPTION: ";
    int pos = 0;
    for (int i = 0; prefix[i]; i++) { vga[pos++] = prefix[i]; vga[pos++] = 0x4F; }
    for (int i = 0; what[i]; i++)   { vga[pos++] = what[i];   vga[pos++] = 0x4F; }
    const char *at = " at EIP 0x";
    for (int i = 0; at[i]; i++)     { vga[pos++] = at[i];     vga[pos++] = 0x4F; }
    for (int shift = 28; shift >= 0; shift -= 4) {
        unsigned int d = (eip >> shift) & 0xF;
        vga[pos++] = d < 10 ? '0' + d : 'A' + d - 10;
        vga[pos++] = 0x4F;
    }
    for (;;) { cli_disable(); halt(); }
}

// Called from isr_common in isr.s for every vector 0..47.
void isr_handler(struct registers *r) {
    if (r->int_no < 32) {
        panic(exception_names[r->int_no], r->eip);
    }

    if (handlers[r->int_no])
        handlers[r->int_no](r);

    // Acknowledge the interrupt (slave first if it came from there)
    if (r->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
