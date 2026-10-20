#include "kernel.h"

// Hardware information: CPU, PCI bus, power off.

static void say(const char *s) { print_string(s); newline(); }

static void hex16(unsigned int v, char *out) {      // 4 hex digits
    char tmp[9];
    hex_to_string(v, tmp);
    memcpy(out, tmp + 4, 5);
}

// ---------------------------------------------------------------- CPU

struct cpuid_regs { unsigned int eax, ebx, ecx, edx; };

static void cpuid(unsigned int leaf, struct cpuid_regs *r) {
    __asm__ volatile ("cpuid" : "=a"(r->eax), "=b"(r->ebx), "=c"(r->ecx), "=d"(r->edx) : "a"(leaf), "c"(0));
}

// CPUID exists if we can flip bit 21 of EFLAGS (486 and older may lack it).
static int cpuid_supported(void) {
    unsigned int a, b;
    __asm__ volatile (
        "pushfl\n\t"
        "popl %0\n\t"
        "movl %0, %1\n\t"
        "xorl $0x200000, %0\n\t"
        "pushl %0\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t"
        "pushl %1\n\t"
        "popfl\n\t"
        : "=&r"(a), "=&r"(b) : : "cc");
    return ((a ^ b) & 0x200000) != 0;
}

static void put_regs(char *dst, const struct cpuid_regs *r) {
    memcpy(dst, &r->eax, 4); memcpy(dst + 4, &r->ebx, 4); memcpy(dst + 8, &r->ecx, 4); memcpy(dst + 12, &r->edx, 4);
}

void hw_cpuinfo(void) {
    char num[16];
    if (!cpuid_supported()) { say("CPU: no CPUID instruction (386/486-class processor)"); return; }

    struct cpuid_regs r;
    char vendor[13];
    cpuid(0, &r);
    unsigned int maxleaf = r.eax;
    memcpy(vendor, &r.ebx, 4); memcpy(vendor + 4, &r.edx, 4); memcpy(vendor + 8, &r.ecx, 4);
    vendor[12] = '\0';
    print_string("Vendor  : "); say(vendor);

    cpuid(0x80000000u, &r);
    if (r.eax >= 0x80000004u) {                       // brand string, e.g. "Intel(R) Pentium(R) M processor 1.70GHz"
        char brand[49];
        for (unsigned int i = 0; i < 3; i++) { cpuid(0x80000002u + i, &r); put_regs(brand + i * 16, &r); }
        brand[48] = '\0';
        const char *b = brand;
        while (*b == ' ') b++;
        print_string("Name    : "); say(b);
    }

    if (maxleaf >= 1) {
        cpuid(1, &r);
        unsigned int family = (r.eax >> 8) & 0xF, model = (r.eax >> 4) & 0xF, step = r.eax & 0xF;
        if (family == 0xF) family += (r.eax >> 20) & 0xFF;
        if (family == 6 || family >= 0xF) model |= ((r.eax >> 16) & 0xF) << 4;
        print_string("Family  : "); int_to_string(family, num); print_string(num);
        print_string("  Model: "); int_to_string(model, num); print_string(num);
        print_string("  Stepping: "); int_to_string(step, num); say(num);

        static const struct { unsigned int bit; const char *name; } feats[] = {
            { 0, "FPU" }, { 4, "TSC" }, { 5, "MSR" }, { 6, "PAE" }, { 8, "CX8" }, { 9, "APIC" }, { 15, "CMOV" },
            { 23, "MMX" }, { 24, "FXSR" }, { 25, "SSE" }, { 26, "SSE2" },
        };
        print_string("Features: ");
        for (unsigned int i = 0; i < sizeof feats / sizeof feats[0]; i++) {
            if (r.edx & (1u << feats[i].bit)) { print_string(feats[i].name); print_string(" "); }
        }
        if (r.ecx & 1) print_string("SSE3 ");
        newline();
    }
}

// ---------------------------------------------------------------- PCI

static const char *pci_class_name(unsigned int cls, unsigned int sub) {
    switch ((cls << 8) | sub) {
        case 0x0100: return "SCSI controller";
        case 0x0101: return "IDE controller";
        case 0x0102: return "Floppy controller";
        case 0x0105: return "ATA controller";
        case 0x0106: return "SATA controller";
        case 0x0108: return "NVMe controller";
        case 0x0200: return "Ethernet";
        case 0x0280: return "Network (Wi-Fi?)";
        case 0x0300: return "VGA display";
        case 0x0380: return "Display";
        case 0x0401: return "Audio";
        case 0x0403: return "HD Audio";
        case 0x0500: return "RAM controller";
        case 0x0600: return "Host bridge";
        case 0x0601: return "ISA bridge";
        case 0x0604: return "PCI bridge";
        case 0x0607: return "CardBus bridge";
        case 0x0700: return "Serial controller";
        case 0x0703: return "Modem";
        case 0x0800: return "Interrupt controller";
        case 0x0880: return "System peripheral";
        case 0x0C00: return "FireWire";
        case 0x0C03: return "USB controller";
        case 0x0C05: return "SMBus";
        default:     return 0;
    }
}

void hw_lspci(void) {
    int found = 0;
    screen_batch_begin();
    say("bus:dev.fn  vendor:device  description");
    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int dev = 0; dev < 32; dev++) {
            unsigned int nfn = 1;
            for (unsigned int fn = 0; fn < nfn; fn++) {
                unsigned int id = pci_read32(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                if (fn == 0 && (pci_read32(bus, dev, 0, 0x0C) & 0x00800000u)) nfn = 8;   // multi-function device

                unsigned int cc = pci_read32(bus, dev, fn, 0x08);
                unsigned int cls = cc >> 24, sub = (cc >> 16) & 0xFF, prog = (cc >> 8) & 0xFF;
                char hx[9], line[96];
                int n = 0;

                hex_to_string(bus, hx);   line[n++] = hx[6]; line[n++] = hx[7]; line[n++] = ':';
                hex_to_string(dev, hx);   line[n++] = hx[6]; line[n++] = hx[7]; line[n++] = '.';
                line[n++] = (char)('0' + fn); line[n++] = ' '; line[n++] = ' '; line[n++] = ' ';
                hex16(id & 0xFFFF, hx);   memcpy(line + n, hx, 4); n += 4; line[n++] = ':';
                hex16(id >> 16, hx);      memcpy(line + n, hx, 4); n += 4; line[n++] = ' '; line[n++] = ' ';
                line[n] = '\0';
                print_string(line);

                const char *name = pci_class_name(cls, sub);
                if (name) print_string(name);
                else { hex16((cls << 8) | sub, hx); print_string("class "); print_string(hx); }

                if (cls == 0x01 && sub == 0x01) print_string((prog & 0x05) ? " (native mode)" : " (legacy ports)");
                if (cls == 0x01 && sub == 0x06) print_string(prog == 1 ? " (AHCI)" : " (not AHCI)");
                if (cls == 0x0C && sub == 0x03) {
                    print_string(prog == 0x00 ? " UHCI" : prog == 0x10 ? " OHCI" : prog == 0x20 ? " EHCI" : prog == 0x30 ? " xHCI" : "");
                }
                newline();
                found++;
            }
        }
    }
    if (!found) say("No PCI devices found (this machine may not have a PCI bus).");
    screen_batch_end();
}

// ---------------------------------------------------------------- power

void hw_shutdown(void) {
    say("Powering off...");
    outw(0x604, 0x2000);       // QEMU (and newer Bochs)
    outw(0xB004, 0x2000);      // older QEMU / Bochs
    outw(0x4004, 0x3400);      // VirtualBox
    // Real hardware would need ACPI/APM, which we don't have yet: stop here.
    say("It is now safe to turn off your computer.");
    cli_disable();
    for (;;) halt();
}
