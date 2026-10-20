#include "kernel.h"

/* RTL8139 legacy PCI I/O driver. DMA uses identity-mapped physical pages.
 * RX is polled in the main loop; no shared IRQ handler or PCI interrupt changes.
 * RCR.WRAP is clear: packets wrap inside the 8 KiB ring, copied modulo its size.
 */
#define RX_SIZE 8192
static unsigned short io;
static volatile unsigned char *rx;
static unsigned char *tx;
static unsigned int rx_at, tx_at;
static unsigned char mac[6];

int rtl8139_present(void) { return io != 0; }
int rtl8139_link(void) { return io && !(inb(io + 0x58) & 4); }
const unsigned char *rtl8139_mac(void) { return mac; }

int rtl8139_init(void) {
    struct pci_loc loc;
    int found = 0;
    for (unsigned int bus = 0; bus < 256 && !found; bus++)
        for (unsigned int dev = 0; dev < 32 && !found; dev++) {
            unsigned int functions = 1;
            for (unsigned int fn = 0; fn < functions; fn++) {
                unsigned int id = pci_read32(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (!fn) break; continue; }
                if (!fn && (pci_read32(bus, dev, fn, 12) & 0x800000)) functions = 8;
                if (id != 0x813910EC) continue;
                loc.bus = bus; loc.dev = dev; loc.fn = fn; found = 1; break;
            }
        }
    if (!found) return 0;
    unsigned int bar = pci_read32(loc.bus, loc.dev, loc.fn, 0x10);
    if (!(bar & 1) || !(bar & ~3u) || (bar & ~3u) > 0xFF00) return 0;
    rx = pmm_alloc_pages(3); tx = pmm_alloc_pages(2);
    if (!rx || !tx) {
        if (rx) pmm_free_pages((void *)rx, 3);
        if (tx) pmm_free_pages(tx, 2);
        return 0;
    }
    io = bar & ~3u;
    unsigned int command = pci_read32(loc.bus, loc.dev, loc.fn, 4) & 0xFFFF;
    pci_write32(loc.bus, loc.dev, loc.fn, 4, command | 5); /* I/O + bus mastering */
    outb(io + 0x52, 0); outb(io + 0x37, 0x10);
    unsigned int tries;
    for (tries = 0; tries < 1000000 && (inb(io + 0x37) & 0x10); tries++) io_wait();
    if (tries == 1000000) { io = 0; return 0; } /* keep DMA memory reserved */
    for (int i = 0; i < 6; i++) mac[i] = inb(io + i);
    /* Reset selects the legacy datapath. Do not touch C+ descriptor registers. */
    outw(io + 0x3C, 0); /* polling; leave PIC masks untouched */
    outw(io + 0x3E, 0xFFFF);
    outl(io + 0x30, (unsigned int)rx);
    for (int i = 0; i < 4; i++) outl(io + 0x20 + i * 4, (unsigned int)(tx + i * 2048));
    outb(io + 0x37, 0x0C);
    outl(io + 0x40, 0x03000700); /* 96-bit IFG, unlimited DMA burst */
    outl(io + 0x44, 0x0000E70A); /* 8 KiB, physical + broadcast, normal wrapping */
    outw(io + 0x38, 0xFFF0);
    return 1;
}

int rtl8139_send(const void *packet, unsigned int n) {
    if (!rtl8139_link() || n < 14 || n > 1514) return -1;
    unsigned short status = io + 0x10 + tx_at * 4;
    if (!(inl(status) & 0x2000)) return -1;
    unsigned int bytes = n < 60 ? 60 : n;
    memset(tx + tx_at * 2048, 0, bytes);
    memcpy(tx + tx_at * 2048, packet, n);
    mem_barrier();
    outl(status, bytes);
    unsigned int start = uptime_ticks;
    for (unsigned int spin = 0; spin < 1000000; spin++) {
        unsigned int state = inl(status);
        if (state & 0x60004000) return -1;
        if (state & 0x8000) { tx_at = (tx_at + 1) % 4; return 0; }
        if (uptime_ticks - start > TIMER_HZ) break;
    }
    return -1;
}

static unsigned int ring16(unsigned int at) {
    return rx[at & (RX_SIZE - 1)] | ((unsigned int)rx[(at + 1) & (RX_SIZE - 1)] << 8);
}
static int reset_rx(void) {
    outb(io + 0x37, 4);
    rx_at = 0;
    outl(io + 0x30, (unsigned int)rx);
    outl(io + 0x44, 0x0000E70A);
    outw(io + 0x38, 0xFFF0);
    outw(io + 0x3E, 0xFFFF);
    outb(io + 0x37, 0x0C);
    return -1;
}
int rtl8139_receive(unsigned char *packet, unsigned int capacity) {
    if (!io) return 0;
    if (inw(io + 0x3E) & 0x50) return reset_rx();
    if (inb(io + 0x37) & 1) return 0;
    mem_barrier();
    unsigned int state = ring16(rx_at), len = ring16(rx_at + 2);
    if (len == 0xFFF0) return 0; /* DMA in progress */
    if (!(state & 1) || (state & 0x3E) || len < 18 || len > 1518 || len - 4 > capacity) return reset_rx();
    for (unsigned int i = 0; i < len - 4; i++) packet[i] = rx[(rx_at + 4 + i) & (RX_SIZE - 1)];
    rx_at = ((rx_at + len + 4 + 3) & ~3u) & (RX_SIZE - 1);
    mem_barrier();
    outw(io + 0x38, (unsigned short)(rx_at - 16));
    outw(io + 0x3E, 5); /* RX/TX completion */
    return len - 4;
}
