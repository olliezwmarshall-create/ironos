#include "kernel.h"

/* Standard three-byte PS/2 packets. IRQ12 shares the 8042 with the keyboard. */
#define MOUSE_QUEUE 64
static volatile unsigned int head, tail;
static struct mouse_event events[MOUSE_QUEUE];
static int present, x = 160, y = 100, max_x=319, max_y=199;
static unsigned char packet[3], cycle, buttons;

int mouse_present(void) { return present; }
void mouse_receive_byte(int value) {
    if (value < 0) { cycle = 0; return; }
    if (!present) return;
    unsigned char b = value;
    if (!cycle && !(b & 8)) return;
    packet[cycle++] = b;
    if (cycle != 3) return;
    cycle = 0;
    int dx = packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    int dy = packet[2] - ((packet[0] & 0x20) ? 256 : 0);
    if (packet[0] & 0xC0) dx = dy = 0; /* overflow: retain buttons, discard motion */
    x += dx; y -= dy;
    if (x < 0) x = 0;
    if (x > max_x) x = max_x;
    if (y < 0) y = 0;
    if (y > max_y) y = max_y;
    unsigned char next_buttons = packet[0] & 7;
    unsigned int next = (head + 1) % MOUSE_QUEUE;
    if (next != tail) {
        events[head] = (struct mouse_event){x, y, dx, -dy, next_buttons, next_buttons & ~buttons};
        mem_barrier(); head = next;
    }
    buttons = next_buttons;
}
int mouse_get_event(struct mouse_event *event) {
    if (head == tail) return 0;
    mem_barrier(); *event = events[tail];
    mem_barrier(); tail = (tail + 1) % MOUSE_QUEUE;
    return 1;
}
void mouse_get_state(struct mouse_event *event) {
#ifndef HOST_TEST
    unsigned int flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
#endif
    *event = (struct mouse_event){x,y,0,0,buttons,0};
#ifndef HOST_TEST
    if (flags & 0x200) sti();
#endif
}
static int input_ready(void) {
    for (unsigned int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 2)) return 1;
        io_wait();
    }
    return 0;
}
static int command(unsigned char c) {
    if (!input_ready()) return 0;
    outb(0x64, c); return 1;
}
static int data(unsigned char c) {
    if (!input_ready()) return 0;
    outb(0x60, c); return 1;
}
static int read_byte(int aux, unsigned int limit) {
    for (unsigned int i = 0; i < limit; i++) {
        unsigned char status = inb(0x64);
        if (status & 1) {
            unsigned char b = inb(0x60);
            if (status & 0xC0) continue;
            if (!!(status & 0x20) == aux) return b;
            if (!(status & 0x20)) keyboard_receive_byte(b);
        }
        io_wait();
    }
    return -1;
}
static int mouse_command(unsigned char c) {
    for (int retry = 0; retry < 3; retry++) {
        if (!command(0xD4) || !data(c)) return 0;
        for (int pending = 0; pending < 32; pending++) {
            int response = read_byte(1, 100000);
            if (response == 0xFA) return 1;
            if (response < 0) return 0;
            if (response == 0xFE) break;
        }
    }
    return 0;
}
void mouse_init(void) {
    int config = -1;
    /* Temporarily inhibit both ports so config replies cannot be mistaken for keys. */
    if (!command(0xAD)) return;
    if (!command(0xA7)) goto failed;
    ps2_irq(0); /* route pending keyboard bytes before requesting controller config */
    if (!command(0x20)) goto failed;
    config = read_byte(0, 100000);
    if (config < 0 || !command(0xA8)) goto failed;
    if (!command(0x60) || !data((config | 2) & ~0x20)) goto failed;
    /* Reset also removes any BIOS wheel/five-button negotiation: always three bytes. */
    if (!mouse_command(0xFF) || read_byte(1, 2000000) != 0xAA || read_byte(1, 100000) != 0) goto failed;
    if (!mouse_command(0xF4)) goto failed;
    present = 1;
    register_interrupt_handler(44, ps2_irq);
    outb(0x21, inb(0x21) & ~4);       /* cascade IRQ2 */
    outb(0xA1, inb(0xA1) & ~0x10);   /* mouse IRQ12 */
    command(0xAE);                    /* restore keyboard port */
    return;
failed:
    command(0xA7);
    if (config >= 0 && command(0x60)) data((config & ~2) | 0x20);
    command(0xAE); /* a missing mouse must never disable the keyboard */
}

void mouse_set_bounds(int width,int height) {
    if (width<1 || height<1 || width>4096 || height>4096) return;
#ifndef HOST_TEST
    unsigned int flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
#endif
    x=x*width/(max_x+1);y=y*height/(max_y+1);
    max_x=width-1;max_y=height-1;tail=head;
#ifndef HOST_TEST
    if (flags&0x200) sti();
#endif
}
