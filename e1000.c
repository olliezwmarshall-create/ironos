#include "kernel.h"

/* Intel 82540EM/82545EM legacy descriptors, polling, identity-mapped DMA. */
struct rx_desc { unsigned int addr, high; unsigned short len, checksum; unsigned char status, errors; unsigned short special; } __attribute__((packed));
struct tx_desc { unsigned int addr, high; unsigned short len; unsigned char cso, cmd, status, css; unsigned short special; } __attribute__((packed));
static volatile unsigned int *regs;
static volatile struct rx_desc *rx_ring;
static volatile struct tx_desc *tx_ring;
static unsigned char *rx_data, *tx_data, mac[6];
static unsigned int rx_head, tx_head;
static unsigned int rd(unsigned int off) { return regs[off/4]; }
static void wr(unsigned int off, unsigned int value) { regs[off/4]=value; }
int e1000_present(void) { return regs!=0; }
int e1000_link(void) { return regs && (rd(8)&2); }
const unsigned char *e1000_mac(void) { return mac; }
int e1000_init(void) {
    struct pci_loc loc;
    int found=0;
    for (unsigned int bus=0;bus<256 && !found;bus++)
        for (unsigned int dev=0;dev<32 && !found;dev++) {
            unsigned int nf=1;
            for (unsigned int fn=0;fn<nf;fn++) {
                unsigned int id=pci_read32(bus,dev,fn,0);
                if ((id&65535)==65535) { if (!fn) break;continue; }
                if (!fn && (pci_read32(bus,dev,fn,12)&0x800000)) nf=8;
                if (id!=0x100E8086 && id!=0x100F8086) continue;
                loc=(struct pci_loc){bus,dev,fn}; found=1;break;
            }
        }
    if (!found) return 0;
    unsigned int bar=pci_read32(loc.bus,loc.dev,loc.fn,16);
    if ((bar&1) || !(bar&~15u) || ((bar&6)==4 && pci_read32(loc.bus,loc.dev,loc.fn,20))) return 0;
    rx_ring=pmm_alloc_pages(1);tx_ring=pmm_alloc_pages(1);
    rx_data=pmm_alloc_pages(16);tx_data=pmm_alloc_pages(4);
    if (!rx_ring || !tx_ring || !rx_data || !tx_data) {
        if (rx_ring) pmm_free_pages((void *)rx_ring,1);
        if (tx_ring) pmm_free_pages((void *)tx_ring,1);
        if (rx_data) pmm_free_pages(rx_data,16);
        if (tx_data) pmm_free_pages(tx_data,4);
        return 0;
    }
    pci_write32(loc.bus,loc.dev,loc.fn,4,(pci_read32(loc.bus,loc.dev,loc.fn,4)&65535)|6);
    regs=(volatile unsigned int *)(bar&~15u);
    wr(0xD8,0xFFFFFFFF);wr(0,rd(0)|(1u<<26));
    unsigned int spin;
    for (spin=0;spin<1000000 && (rd(0)&(1u<<26));spin++) io_wait();
    if (spin==1000000) { regs=0;return 0; } /* preserve memory if reset failed */
    wr(0xD8,0xFFFFFFFF);wr(0,rd(0)|(1u<<6)); /* auto speed, link up */
    unsigned int low=rd(0x5400), high=rd(0x5404);
    for (int i=0;i<4;i++) mac[i]=low>>(i*8);
    mac[4]=high;mac[5]=high>>8;
    if (!(high&0x80000000) || (mac[0]&1) || !(low|(high&65535))) { regs=0;return 0; }
    for (int i=0;i<128;i++) wr(0x5200+i*4,0);
    memset((void *)rx_ring,0,4096);memset((void *)tx_ring,0,4096);
    for (int i=0;i<32;i++) rx_ring[i].addr=(unsigned int)(rx_data+i*2048);
    for (int i=0;i<8;i++) { tx_ring[i].addr=(unsigned int)(tx_data+i*2048);tx_ring[i].status=1; }
    rx_head=tx_head=0;
    wr(0x2800,(unsigned int)rx_ring);wr(0x2804,0);wr(0x2808,32*16);wr(0x2810,0);wr(0x2818,31);
    wr(0x3800,(unsigned int)tx_ring);wr(0x3804,0);wr(0x3808,8*16);wr(0x3810,0);wr(0x3818,0);
    wr(0x410,10|(8<<10)|(6<<20));
    mem_barrier();wr(0x400,2|8|(15<<4)|(64<<12));wr(0x100,2|(1<<15)|(1<<26));
    return 1;
}
int e1000_send(const void *packet,unsigned int n) {
    if (!e1000_link() || n<14 || n>1514) return -1;
    volatile struct tx_desc *d=&tx_ring[tx_head];
    if (!(d->status&1)) return -1;
    memcpy(tx_data+tx_head*2048,packet,n);
    d->len=n;d->cmd=0xB;d->status=0;
    mem_barrier();tx_head=(tx_head+1)%8;wr(0x3818,tx_head);
    unsigned int start=uptime_ticks;
    for (unsigned int spin=0;spin<1000000;spin++) {
        if (d->status&1) return (d->status&0xE)?-1:0;
        if (uptime_ticks-start>TIMER_HZ) break;
    }
    return -1;
}
int e1000_receive(unsigned char *packet,unsigned int capacity) {
    if (!regs) return 0;
    volatile struct rx_desc *d=&rx_ring[rx_head];
    if (!(d->status&1)) return 0;
    mem_barrier();
    int n=d->len;
    if (!(d->status&2) || d->errors || n<14 || n>1514 || (unsigned int)n>capacity) n=-1;
    else memcpy(packet,rx_data+rx_head*2048,n);
    d->status=0;mem_barrier();wr(0x2818,rx_head);rx_head=(rx_head+1)%32;
    return n;
}
