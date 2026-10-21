/* Malformed DMA descriptors must never copy outside a caller's receive buffer. */
#include <stdio.h>
#include <stdlib.h>
#include "../e1000.c"
volatile unsigned long uptime_ticks;
unsigned int pci_read32(unsigned int b,unsigned int d,unsigned int f,unsigned int r) { (void)b;(void)d;(void)f;(void)r;return 0xFFFFFFFF; }
void pci_write32(unsigned int b,unsigned int d,unsigned int f,unsigned int r,unsigned int v) { (void)b;(void)d;(void)f;(void)r;(void)v; }
void *pmm_alloc_pages(unsigned int n) { (void)n;return 0; }
void pmm_free_pages(void *p,unsigned int n) { (void)p;(void)n; }
unsigned char inb(unsigned short p) { (void)p;return 0; }
void outb(unsigned short p,unsigned char v) { (void)p;(void)v; }
void *memcpy(void *d,const void *s,unsigned int n) { unsigned char *a=d;const unsigned char *b=s;for(unsigned int i=0;i<n;i++) a[i]=b[i];return d; }
void *memset(void *d,int v,unsigned int n) { unsigned char *a=d;for(unsigned int i=0;i<n;i++) a[i]=v;return d; }
static void check(int ok,const char *msg) { if (!ok) { puts(msg);abort(); } }
int main(void) {
    static unsigned int registers[0x6000/4];
    static struct rx_desc descriptors[32];static unsigned char data[32*2048];
    unsigned char packet[64];
    check(!e1000_init(),"missing Intel card");
    regs=registers;rx_ring=descriptors;rx_data=data;
    check(!e1000_receive(packet,sizeof(packet)),"incomplete DMA left alone");
    for (int i=0;i<96;i++) {
        unsigned int index=rx_head;data[index*2048]=0xA5;
        descriptors[index].status=3;descriptors[index].len=60;descriptors[index].errors=0;
        check(e1000_receive(packet,sizeof(packet))==60 && packet[0]==0xA5,"ring wrap receive");
        check(!descriptors[index].status && registers[0x2818/4]==index,"descriptor returned to NIC");
    }
    const unsigned int lengths[]={0,13,65,1515,65535};
    for (unsigned int i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
        descriptors[rx_head].status=3;descriptors[rx_head].len=lengths[i];
        check(e1000_receive(packet,sizeof(packet))==-1,"bad DMA length rejected");
    }
    descriptors[rx_head].status=1;descriptors[rx_head].len=60;
    check(e1000_receive(packet,sizeof(packet))==-1,"incomplete frame rejected");
    descriptors[rx_head].status=3;descriptors[rx_head].len=60;descriptors[rx_head].errors=1;
    check(e1000_receive(packet,sizeof(packet))==-1,"hardware frame error rejected");
    check(e1000_send(packet,60)==-1,"link down transmit rejected");
    puts("PASS: Intel receive ring wrap, DMA length/capacity/EOP/error guards, missing card and link down");
}
