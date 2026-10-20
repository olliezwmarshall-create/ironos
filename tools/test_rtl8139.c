/* Driver register and receive-ring tests. No real ports or DMA are accessed. */
#include <stdio.h>
#include <stdlib.h>
#include "../rtl8139.c"
volatile unsigned long uptime_ticks;
static unsigned char memory[5*4096], regs[256];
static unsigned int next_page;
static int reset_writes;
void *memcpy(void *d,const void *s,unsigned int n) { unsigned char *a=d;const unsigned char *b=s;for(unsigned int i=0;i<n;i++)a[i]=b[i];return d; }
void *memset(void *d,int c,unsigned int n) { unsigned char *a=d;for(unsigned int i=0;i<n;i++)a[i]=c;return d; }
unsigned int pci_read32(unsigned int b,unsigned int d,unsigned int f,unsigned int r) {
    if(b||d||f)return 0xFFFFFFFF;
    if(r==0)return 0x813910EC;
    if(r==0x10)return 0x1001;
    return 0;
}
void pci_write32(unsigned int b,unsigned int d,unsigned int f,unsigned int r,unsigned int v) { (void)b;(void)d;(void)f;(void)r;(void)v; }
void *pmm_alloc_pages(unsigned int n) { void *p=memory+next_page*4096;next_page+=n;if(next_page>5)abort();return p; }
void pmm_free_pages(void *p,unsigned int n) { (void)p;(void)n; }
unsigned char inb(unsigned short p) { return regs[p&255]; }
unsigned short inw(unsigned short p) { return inb(p)|((unsigned int)inb(p+1)<<8); }
unsigned int inl(unsigned short p) { return inw(p)|((unsigned int)inw(p+2)<<16); }
void outb(unsigned short p,unsigned char v) {
    if(p==0x80)return;
    if((p&255)==0xE0)abort(); /* touching C+ command selects the wrong QEMU datapath */
    if((p&255)==0x37 && v==0x10){v=0;reset_writes++;}
    regs[p&255]=v;
}
void outw(unsigned short p,unsigned short v) {
    if((p&255)==0x3E){regs[0x3E]=regs[0x3F]=0;return;}
    outb(p,v);outb(p+1,v>>8);
}
void outl(unsigned short p,unsigned int v) { outw(p,v);outw(p+2,v>>16); }
static void check(int ok,const char *what) { if(!ok){fprintf(stderr,"FAIL: %s\n",what);exit(1);} }
static void ring_put(unsigned int at,unsigned int value) { ((unsigned char *)rx)[at&8191]=value; }
int main(void) {
    check(rtl8139_init()&&reset_writes==1,"PCI init and legacy register selection");
    rx_at=8188;ring_put(rx_at,1);ring_put(rx_at+1,0);ring_put(rx_at+2,68);ring_put(rx_at+3,0);
    for(unsigned int i=0;i<64;i++)ring_put(rx_at+4+i,i);
    unsigned char packet[1514];
    check(rtl8139_receive(packet,sizeof(packet))==64,"packet crossing ring boundary");
    for(int i=0;i<64;i++)check(packet[i]==i,"wrapped payload preserved");
    check(rx_at==68&&inw(io+0x38)==52,"CAPR alignment and 16-byte offset");
    ring_put(rx_at,1);ring_put(rx_at+1,0);ring_put(rx_at+2,0xF0);ring_put(rx_at+3,0xFF);
    check(rtl8139_receive(packet,sizeof(packet))==0,"in-progress DMA not consumed");
    ring_put(rx_at+2,0xFF);ring_put(rx_at+3,0x7F);
    check(rtl8139_receive(packet,sizeof(packet))==-1&&rx_at==0,"oversized frame resets receive ring");
    regs[0x3E]=0x10;
    check(rtl8139_receive(packet,sizeof(packet))==-1,"overflow recovery");
    regs[0x58]=4;check(!rtl8139_link(),"link-down detection");
    puts("PASS: RTL8139 PCI/legacy init, wrapped receive, CAPR, DMA wait, length/overflow guards and link state");
    return 0;
}
