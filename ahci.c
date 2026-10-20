#include "kernel.h"

// AHCI (SATA native mode) driver: polling, one command slot, DMA through a bounce buffer.
// Used by most PCs from ~2008 on when the BIOS "SATA mode" is AHCI.

#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define HBA_CAP2  0x24
#define HBA_BOHC  0x28
#define PORT_BASE(n) (0x100 + (n) * 0x80)

#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SERR  0x30
#define P_CI    0x38

#define CMD_ST  0x00000001
#define CMD_FRE 0x00000010
#define CMD_FR  0x00004000
#define CMD_CR  0x00008000
#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80
#define IS_TFES (1u << 30)

#define SIG_ATA   0x00000101u
#define SIG_ATAPI 0xEB140101u

#define SPIN   5000000
#define CHUNK  128                       // sectors per command (64 KB bounce buffer)

struct port {
    volatile unsigned char *regs;
    unsigned char *clb, *fb, *ctba, *bounce;
    int usable;
};

static struct port ports[32];
static int nports = 0;
static volatile unsigned char *abar;

static unsigned int rd(volatile unsigned char *base, unsigned int off) { return mmio_read32(base + off); }
static void wr(volatile unsigned char *base, unsigned int off, unsigned int v) { mmio_write32(base + off, v); }

static int port_stop(volatile unsigned char *p) {
    wr(p, P_CMD, rd(p, P_CMD) & ~(CMD_ST | CMD_FRE));
    for (int i = 0; i < SPIN; i++)
        if (!(rd(p, P_CMD) & (CMD_FR | CMD_CR))) return 0;
    return -1;
}

static void port_start(volatile unsigned char *p) {
    for (int i = 0; i < SPIN && (rd(p, P_CMD) & CMD_CR); i++) { }
    wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
}

// Issue one command in slot 0 and wait. fis = 20-byte register FIS; nbytes = data length (0 = none).
static int run(struct port *pt, int is_write, const unsigned char *fis, unsigned int nbytes) {
    volatile unsigned char *p = pt->regs;
    int i;
    for (i = 0; i < SPIN; i++) if (!(rd(p, P_TFD) & (TFD_BSY | TFD_DRQ))) break;
    if (i == SPIN) return -1;

    memset(pt->ctba, 0, 128 + 16);
    memcpy(pt->ctba, fis, 20);
    unsigned int *hdr = (unsigned int *)pt->clb;
    hdr[0] = 5u | (is_write ? 0x40u : 0u) | (nbytes ? (1u << 16) : 0u);     // 5-dword FIS, one PRDT entry
    hdr[1] = 0;
    hdr[2] = (unsigned int)(unsigned long)pt->ctba;
    hdr[3] = 0;
    if (nbytes) {
        unsigned int *prd = (unsigned int *)(pt->ctba + 128);
        prd[0] = (unsigned int)(unsigned long)pt->bounce;
        prd[1] = 0;
        prd[2] = 0;
        prd[3] = nbytes - 1;
    }
    mem_barrier();
    wr(p, P_IS, 0xFFFFFFFFu);
    wr(p, P_CI, 1);

    for (i = 0; i < SPIN * 4; i++) {
        if (!(rd(p, P_CI) & 1)) break;
        if (rd(p, P_IS) & IS_TFES) return -1;
    }
    if (i == SPIN * 4) return -1;
    if ((rd(p, P_IS) & IS_TFES) || (rd(p, P_TFD) & TFD_ERR)) return -1;
    return 0;
}

static void make_fis(unsigned char *f, unsigned char cmd, unsigned int lba, unsigned int count) {
    memset(f, 0, 20);
    f[0] = 0x27;                       // host-to-device register FIS
    f[1] = 0x80;                       // this is a command
    f[2] = cmd;
    f[4] = (unsigned char)lba;  f[5] = (unsigned char)(lba >> 8);  f[6] = (unsigned char)(lba >> 16);
    f[7] = 0x40;                       // LBA mode
    f[8] = (unsigned char)(lba >> 24);
    f[12] = (unsigned char)count;  f[13] = (unsigned char)(count >> 8);
}

static void copy_model(char *dst, const unsigned short *id) {
    for (int i = 0; i < 20; i++) { dst[i * 2] = (char)(id[27 + i] >> 8); dst[i * 2 + 1] = (char)(id[27 + i] & 0xFF); }
    dst[40] = '\0';
    int n = 39;
    while (n >= 0 && (dst[n] == ' ' || dst[n] == '\0')) dst[n--] = '\0';
    for (int i = 0; dst[i]; i++) if (dst[i] < 32 || dst[i] > 126) dst[i] = '?';
}

static void name_disk(struct disk *d, int port) {
    d->name[0] = 's'; d->name[1] = 'a'; d->name[2] = 't'; d->name[3] = 'a';
    d->name[4] = (char)('0' + (nports < 10 ? nports : 9)); d->name[5] = '\0';
    const char *w = "port ";
    unsigned int n = 0;
    for (; w[n]; n++) d->desc[n] = w[n];
    if (port >= 10) d->desc[n++] = (char)('0' + port / 10);
    d->desc[n++] = (char)('0' + port % 10);
    d->desc[n] = '\0';
}

static void probe_port(unsigned int n) {
    volatile unsigned char *p = abar + PORT_BASE(n);
    unsigned int ssts = rd(p, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) return;      // no device / link not active
    unsigned int sig = rd(p, P_SIG);
    if (sig != SIG_ATA && sig != SIG_ATAPI) return;

    struct disk d;
    memset(&d, 0, sizeof d);
    d.unit = nports;
    name_disk(&d, (int)n);
    struct port *pt = &ports[nports];
    memset(pt, 0, sizeof *pt);

    if (sig == SIG_ATAPI) {                                         // SATA CD/DVD drive: listed, not usable
        d.kind = DISK_AHCI_ATAPI;
        const char *m = "SATA optical drive";
        for (int i = 0; m[i]; i++) d.model[i] = m[i];
        nports++;
        disk_add(&d);
        return;
    }

    unsigned char *mem = (unsigned char *)pmm_alloc_pages(1);
    unsigned char *bounce = (unsigned char *)pmm_alloc_pages(CHUNK * 512 / 4096);
    if (!mem || !bounce) return;
    memset(mem, 0, 4096);
    pt->regs = p;
    pt->clb = mem;                 // command list: 32 headers x 32 bytes = 1 KB
    pt->fb = mem + 1024;           // received-FIS area (256-byte aligned)
    pt->ctba = mem + 2048;         // command table for slot 0
    pt->bounce = bounce;

    if (port_stop(p) != 0) return;
    wr(p, P_CLB, (unsigned int)(unsigned long)pt->clb);   wr(p, P_CLBU, 0);
    wr(p, P_FB, (unsigned int)(unsigned long)pt->fb);     wr(p, P_FBU, 0);
    wr(p, P_SERR, 0xFFFFFFFFu);
    wr(p, P_IE, 0);                                       // polling only
    wr(p, P_IS, 0xFFFFFFFFu);
    port_start(p);

    unsigned char fis[20];
    make_fis(fis, 0xEC, 0, 0);                            // IDENTIFY DEVICE
    fis[7] = 0;
    if (run(pt, 0, fis, 512) != 0) return;

    unsigned short id[256];
    memcpy(id, pt->bounce, 512);
    unsigned long long total;
    if (id[83] & (1 << 10))
        total = (unsigned long long)id[100] | ((unsigned long long)id[101] << 16) |
                ((unsigned long long)id[102] << 32) | ((unsigned long long)id[103] << 48);
    else
        total = id[60] | ((unsigned long long)id[61] << 16);
    if (total == 0) return;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1 << 12))) {            // logical sector size reported
        unsigned int words = id[117] | ((unsigned int)id[118] << 16);
        if (words != 256) return;                                            // 4Kn disks: not supported yet
    }

    d.kind = DISK_AHCI;
    d.usable = 1;
    d.sectors = total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)total;
    d.truncated = total > 0xFFFFFFFFull;
    copy_model(d.model, id);
    pt->usable = 1;
    nports++;
    disk_add(&d);
}

int ahci_init(void) {
    struct pci_loc loc;
    if (!pci_find_class(0x01, 0x06, 0x01, &loc)) return 0;

    unsigned int bar5 = pci_read32(loc.bus, loc.dev, loc.fn, 0x24);
    if (bar5 & 1) return 0;                                            // must be memory-mapped
    abar = (volatile unsigned char *)(unsigned long)(bar5 & ~0xFu);
    if (!abar) return 0;

    unsigned int cmd = pci_read32(loc.bus, loc.dev, loc.fn, 0x04) & 0xFFFFu;
    pci_write32(loc.bus, loc.dev, loc.fn, 0x04, cmd | 0x06);           // memory space + bus master

    if (rd(abar, HBA_CAP2) & 1) {                                      // BIOS/OS handoff: take the controller from the BIOS
        unsigned int b = rd(abar, HBA_BOHC);
        if (b & 1) {
            wr(abar, HBA_BOHC, b | 2);
            for (int i = 0; i < SPIN && (rd(abar, HBA_BOHC) & 1); i++) { }
        }
    }
    wr(abar, HBA_GHC, (rd(abar, HBA_GHC) | 0x80000000u) & ~0x2u);      // AHCI enable, interrupts off

    unsigned int pi = rd(abar, HBA_PI);
    nports = 0;
    int found = 0;
    for (unsigned int n = 0; n < 32 && nports < 32; n++) {
        if (!(pi & (1u << n))) continue;
        int before = nports;
        probe_port(n);
        if (nports > before && ports[before].usable) found++;          // count real disks, not CD drives
    }
    return found;
}

static int xfer(int unit, int is_write, unsigned int lba, unsigned int count, void *buf) {
    if (unit < 0 || unit >= nports || !ports[unit].usable) return -1;
    struct port *pt = &ports[unit];
    unsigned char *b = (unsigned char *)buf;
    unsigned char fis[20];

    while (count > 0) {
        unsigned int n = count > CHUNK ? CHUNK : count;
        make_fis(fis, is_write ? 0x35 : 0x25, lba, n);          // WRITE/READ DMA EXT
        if (is_write) memcpy(pt->bounce, b, n * 512);
        if (run(pt, is_write, fis, n * 512) != 0) return -1;
        if (!is_write) memcpy(b, pt->bounce, n * 512);
        b += n * 512;
        lba += n;
        count -= n;
    }
    if (is_write) {
        make_fis(fis, 0xEA, 0, 0);                              // FLUSH CACHE EXT
        fis[7] = 0;
        if (run(pt, 0, fis, 0) != 0) return -1;
    }
    return 0;
}

int ahci_read(int unit, unsigned int lba, unsigned int count, void *buf) { return xfer(unit, 0, lba, count, buf); }
int ahci_write(int unit, unsigned int lba, unsigned int count, const void *buf) { return xfer(unit, 1, lba, count, (void *)buf); }
