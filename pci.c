#include "kernel.h"

unsigned int pci_read32(unsigned int bus, unsigned int dev, unsigned int fn, unsigned int reg) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xFC));
    return inl(0xCFC);
}

void pci_write32(unsigned int bus, unsigned int dev, unsigned int fn, unsigned int reg, unsigned int value) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (reg & 0xFC));
    outl(0xCFC, value);
}

// Find the first device of a PCI class/subclass (prog 0xFF = any programming interface).
int pci_find_class(unsigned int cls, unsigned int sub, unsigned int prog, struct pci_loc *loc) {
    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int dev = 0; dev < 32; dev++) {
            unsigned int nfn = 1;
            for (unsigned int fn = 0; fn < nfn; fn++) {
                unsigned int id = pci_read32(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                if (fn == 0 && (pci_read32(bus, dev, 0, 0x0C) & 0x00800000u)) nfn = 8;
                unsigned int cc = pci_read32(bus, dev, fn, 0x08);
                if ((cc >> 24) == cls && ((cc >> 16) & 0xFF) == sub && (prog == 0xFF || ((cc >> 8) & 0xFF) == prog)) {
                    loc->bus = (unsigned char)bus; loc->dev = (unsigned char)dev; loc->fn = (unsigned char)fn;
                    return 1;
                }
            }
        }
    }
    return 0;
}
