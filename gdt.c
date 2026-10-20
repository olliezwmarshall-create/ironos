#include "kernel.h"

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  granularity;
    unsigned char  base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr gp;

extern void gdt_flush(unsigned int gdt_ptr_addr);   // isr.s

static void gdt_set(int i, unsigned int base, unsigned int limit,
                    unsigned char access, unsigned char gran) {
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (unsigned int)&gdt;

    gdt_set(0, 0, 0, 0, 0);                    // null descriptor
    gdt_set(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);     // 0x08: kernel code, flat 4GB
    gdt_set(2, 0, 0xFFFFFFFF, 0x92, 0xCF);     // 0x10: kernel data, flat 4GB

    gdt_flush((unsigned int)&gp);
}
