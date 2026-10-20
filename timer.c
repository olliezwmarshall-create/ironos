#include "kernel.h"

static void timer_callback(struct registers *r) {
    (void)r;
    uptime_ticks++;
}

void timer_init(unsigned int hz) {
    unsigned int divisor = 1193182 / hz;
    outb(0x43, 0x36);                       // channel 0, lo/hi byte, square wave
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    register_interrupt_handler(32, timer_callback);   // IRQ0
}
