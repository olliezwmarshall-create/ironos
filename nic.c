#include "kernel.h"

/* Prefer Intel, then Realtek. Exactly one interface is active at a time. */
static int active;
void nic_init(void) { active=e1000_init()?2:rtl8139_init()?1:0; }
int nic_present(void) { return active!=0; }
int nic_link(void) { return active==2?e1000_link():active==1?rtl8139_link():0; }
const char *nic_name(void) { return active==2?"Intel E1000":active==1?"Realtek RTL8139":"No supported Ethernet"; }
const unsigned char *nic_mac(void) { return active==2?e1000_mac():rtl8139_mac(); }
int nic_send(const void *p,unsigned int n) { return active==2?e1000_send(p,n):active==1?rtl8139_send(p,n):-1; }
int nic_receive(unsigned char *p,unsigned int n) { return active==2?e1000_receive(p,n):active==1?rtl8139_receive(p,n):0; }
void nic_list(void) {
    print_string("Active: ");print_string(nic_name());newline();
    int count=0;
    for (unsigned int b=0;b<256;b++) for (unsigned int d=0;d<32;d++) {
        unsigned int nf=1;
        for (unsigned int f=0;f<nf;f++) {
            unsigned int id=pci_read32(b,d,f,0);
            if ((id&65535)==65535) { if (!f) break;continue; }
            if (!f && (pci_read32(b,d,f,12)&0x800000)) nf=8;
            unsigned int cls=pci_read32(b,d,f,8);
            if ((cls>>24)!=2) continue;
            char h[9];hex_to_string(id,h);print_string("PCI device:vendor ");print_string(h);
            print_string(id==0x100E8086 || id==0x100F8086?" Intel E1000 driver":id==0x813910EC?" RTL8139 driver":((cls>>16)&255)==0x80?" Wireless/other: driver unavailable":" Driver unavailable");newline();count++;
        }
    }
    if (!count) { print_string("No PCI network controllers detected.");newline(); }
    print_string("Wi-Fi firmware/authentication and USB Wi-Fi drivers are not implemented.");newline();
}
