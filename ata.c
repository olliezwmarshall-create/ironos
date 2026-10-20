#include "kernel.h"

// IDE (ATA) driver, PIO mode. 28-bit LBA, plus 48-bit LBA for disks over 128 GB.
// Drive index 0..3 = primary master/slave, secondary master/slave. Handles both the legacy ports
// (0x1F0/0x170) and controllers in PCI native mode (ports taken from the controller's BARs).
#define R_DATA    0
#define R_COUNT   2
#define R_LBA_LO  3
#define R_LBA_MID 4
#define R_LBA_HI  5
#define R_DRIVE   6
#define R_STATUS  7
#define R_COMMAND 7

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

#define WAIT_LONG  4000000     // used while a drive may still be spinning up
#define WAIT_SHORT 1000000
#define LBA28_END  0x10000000ULL    // 28-bit commands can address LBAs below this

static struct ata_drive drives[ATA_MAX_DRIVES];
static unsigned short id_serial[ATA_MAX_DRIVES][10];

static struct { unsigned short io, ctl; int slave; } slots[ATA_MAX_DRIVES] = {
    { 0x1F0, 0x3F6, 0 }, { 0x1F0, 0x3F6, 1 },
    { 0x170, 0x376, 0 }, { 0x170, 0x376, 1 },
};

// Reading the alternate status register four times gives the drive ~400ns to update its status.
static void delay400(const struct ata_drive *d) {
    for (int i = 0; i < 4; i++) inb(d->ctl);
}

static void select_drive(const struct ata_drive *d, unsigned int bits) {
    outb(d->io + R_DRIVE, (unsigned char)(bits | (d->slave ? 0x10 : 0)));
    delay400(d);
}

static int wait_not_busy(const struct ata_drive *d, int limit) {
    for (int i = 0; i < limit; i++) {
        if (!(inb(d->io + R_STATUS) & ST_BSY)) return 0;
    }
    return -1;
}

static int wait_drq(const struct ata_drive *d) {
    for (int i = 0; i < WAIT_SHORT; i++) {
        unsigned char st = inb(d->io + R_STATUS);
        if (st & (ST_ERR | ST_DF)) return -1;
        if (!(st & ST_BSY) && (st & ST_DRQ)) return 0;
    }
    return -1;
}

static void copy_model(struct ata_drive *d, const unsigned short *id) {
    for (int i = 0; i < 20; i++) {                     // words 27..46: 40 ASCII chars, high byte first
        d->model[i * 2]     = (char)(id[27 + i] >> 8);
        d->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    d->model[40] = '\0';
    int n = 39;
    while (n >= 0 && (d->model[n] == ' ' || d->model[n] == '\0')) d->model[n--] = '\0';
    for (int i = 0; d->model[i]; i++)
        if (d->model[i] < 32 || d->model[i] > 126) d->model[i] = '?';
}

static void identify(int index) {
    struct ata_drive *d = &drives[index];
    memset(d, 0, sizeof *d);
    d->io = slots[index].io;
    d->ctl = slots[index].ctl;
    d->slave = slots[index].slave;

    if (inb(d->io + R_STATUS) == 0xFF) return;        // floating bus: no channel here

    outb(d->ctl, 0x02);                               // nIEN: poll, no interrupts
    select_drive(d, 0xE0);
    outb(d->io + R_COUNT, 0);
    outb(d->io + R_LBA_LO, 0);
    outb(d->io + R_LBA_MID, 0);
    outb(d->io + R_LBA_HI, 0);
    outb(d->io + R_COMMAND, 0xEC);                    // IDENTIFY DEVICE

    unsigned char first = inb(d->io + R_STATUS);
    if (first == 0 || first == 0xFF) return;          // nothing answered on this position
    if (wait_not_busy(d, WAIT_LONG) != 0) return;

    unsigned char mid = inb(d->io + R_LBA_MID), hi = inb(d->io + R_LBA_HI);
    unsigned short id[256];

    if (mid == 0x14 && hi == 0xEB) {                  // ATAPI signature: CD/DVD drive
        outb(d->io + R_COMMAND, 0xA1);                // IDENTIFY PACKET DEVICE, just to get the name
        delay400(d);
        d->present = 1;
        d->atapi = 1;
        if (wait_drq(d) == 0) { insw(d->io + R_DATA, id, 256); copy_model(d, id); }
        return;
    }
    if (mid != 0 || hi != 0) return;                  // some other device: not supported
    if (wait_drq(d) != 0) return;
    insw(d->io + R_DATA, id, 256);

    if (!(id[49] & (1 << 9))) return;                 // no LBA support: too old for this driver
    unsigned int lba28 = id[60] | ((unsigned int)id[61] << 16);
    unsigned long long total = lba28;
    if (id[83] & (1 << 10)) {                         // 48-bit LBA
        d->lba48 = 1;
        total = (unsigned long long)id[100] | ((unsigned long long)id[101] << 16) |
                ((unsigned long long)id[102] << 32) | ((unsigned long long)id[103] << 48);
        if (total < lba28) total = lba28;
    }
    if (total == 0) return;

    for (int i = 0; i < 10; i++) id_serial[index][i] = id[10 + i];
    d->present = 1;
    d->sectors = total > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (unsigned int)total;
    d->truncated = total > 0xFFFFFFFFULL;
    copy_model(d, id);

    // A missing slave can "echo" the master's registers. If it claims to be the same disk, ignore it.
    if (d->slave && drives[index - 1].present && !drives[index - 1].atapi &&
        memcmp(id_serial[index], id_serial[index - 1], sizeof id_serial[0]) == 0 &&
        strcmp(d->model, drives[index - 1].model) == 0) {
        memset(d, 0, sizeof *d);
    }
}

// Controllers in PCI native mode: the channel ports come from the BARs instead of 0x1F0/0x170.
static void detect_native_ide(void) {
    struct pci_loc l;
    if (!pci_find_class(0x01, 0x01, 0xFF, &l)) return;
    unsigned int prog = (pci_read32(l.bus, l.dev, l.fn, 0x08) >> 8) & 0xFF;
    if (!(prog & 0x05)) return;                          // both channels in legacy-compatible mode
    unsigned int cmd = pci_read32(l.bus, l.dev, l.fn, 0x04) & 0xFFFFu;
    pci_write32(l.bus, l.dev, l.fn, 0x04, cmd | 0x01);   // enable I/O space

    for (int ch = 0; ch < 2; ch++) {
        if (!(prog & (ch == 0 ? 0x01 : 0x04))) continue;
        unsigned int cmdbar = pci_read32(l.bus, l.dev, l.fn, ch == 0 ? 0x10 : 0x18);
        unsigned int ctlbar = pci_read32(l.bus, l.dev, l.fn, ch == 0 ? 0x14 : 0x1C);
        if (!(cmdbar & 1) || !(ctlbar & 1)) continue;
        unsigned short io = (unsigned short)(cmdbar & 0xFFFC);
        unsigned short ctl = (unsigned short)((ctlbar & 0xFFFC) + 2);       // device-control register is at +2
        if (io < 0x100) continue;
        slots[ch * 2].io = slots[ch * 2 + 1].io = io;
        slots[ch * 2].ctl = slots[ch * 2 + 1].ctl = ctl;
    }
}

int ata_init(void) {
    int usable = 0;
    detect_native_ide();
    for (int i = 0; i < ATA_MAX_DRIVES; i++) {
        identify(i);
        const struct ata_drive *d = &drives[i];
        if (!d->present) continue;

        struct disk bd;
        memset(&bd, 0, sizeof bd);
        bd.kind = d->atapi ? DISK_ATAPI : DISK_IDE;
        bd.unit = i;
        bd.usable = !d->atapi;
        bd.name[0] = 'i'; bd.name[1] = 'd'; bd.name[2] = 'e'; bd.name[3] = (char)('0' + i);
        const char *where = (i < 2 ? "primary " : "secondary ");
        unsigned int n = 0;
        for (; where[n]; n++) bd.desc[n] = where[n];
        const char *role = d->slave ? "slave" : "master";
        for (unsigned int k = 0; role[k]; k++) bd.desc[n++] = role[k];
        memcpy(bd.model, d->model, sizeof bd.model);
        bd.sectors = d->sectors;
        bd.truncated = d->truncated;
        disk_add(&bd);
        if (!d->atapi) usable++;
    }
    return usable;
}

const struct ata_drive *ata_drive_get(int index) {
    if (index < 0 || index >= ATA_MAX_DRIVES) return 0;
    return &drives[index];
}

// Program one read/write command (28-bit, or 48-bit when the range goes past 128 GB).
static void issue(const struct ata_drive *d, unsigned int lba, unsigned int n, int write) {
    int use48 = d->lba48 && (unsigned long long)lba + n > LBA28_END;
    if (use48) {
        select_drive(d, 0x40);
        outb(d->io + R_COUNT, 0);                        // high bytes first
        outb(d->io + R_LBA_LO, (unsigned char)(lba >> 24));
        outb(d->io + R_LBA_MID, 0);
        outb(d->io + R_LBA_HI, 0);
        outb(d->io + R_COUNT, (unsigned char)n);         // then the low bytes
        outb(d->io + R_LBA_LO, (unsigned char)lba);
        outb(d->io + R_LBA_MID, (unsigned char)(lba >> 8));
        outb(d->io + R_LBA_HI, (unsigned char)(lba >> 16));
        outb(d->io + R_COMMAND, write ? 0x34 : 0x24);    // WRITE/READ SECTORS EXT
    } else {
        select_drive(d, 0xE0 | ((lba >> 24) & 0x0F));
        outb(d->io + R_COUNT, (unsigned char)n);
        outb(d->io + R_LBA_LO, (unsigned char)lba);
        outb(d->io + R_LBA_MID, (unsigned char)(lba >> 8));
        outb(d->io + R_LBA_HI, (unsigned char)(lba >> 16));
        outb(d->io + R_COMMAND, write ? 0x30 : 0x20);
    }
}

static const struct ata_drive *usable_drive(int unit) {
    if (unit < 0 || unit >= ATA_MAX_DRIVES) return 0;
    const struct ata_drive *d = &drives[unit];
    return (d->present && !d->atapi) ? d : 0;
}

int ata_read(int unit, unsigned int lba, unsigned int count, void *buf) {
    const struct ata_drive *d = usable_drive(unit);
    if (!d) return -1;
    unsigned short *p = (unsigned short *)buf;
    while (count > 0) {
        unsigned int n = count > 255 ? 255 : count;
        if (wait_not_busy(d, WAIT_SHORT) != 0) return -1;
        issue(d, lba, n, 0);
        for (unsigned int s = 0; s < n; s++) {
            delay400(d);
            if (wait_drq(d) != 0) return -1;
            insw(d->io + R_DATA, p, 256);
            p += 256;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int ata_write(int unit, unsigned int lba, unsigned int count, const void *buf) {
    const struct ata_drive *d = usable_drive(unit);
    if (!d) return -1;
    const unsigned short *p = (const unsigned short *)buf;
    while (count > 0) {
        unsigned int n = count > 255 ? 255 : count;
        if (wait_not_busy(d, WAIT_SHORT) != 0) return -1;
        issue(d, lba, n, 1);
        for (unsigned int s = 0; s < n; s++) {
            delay400(d);
            if (wait_drq(d) != 0) return -1;
            outsw(d->io + R_DATA, p, 256);
            p += 256;
            delay400(d);
            if (wait_not_busy(d, WAIT_LONG) != 0) return -1;
            if (inb(d->io + R_STATUS) & (ST_ERR | ST_DF)) return -1;
        }
        lba += n;
        count -= n;
    }
    outb(d->io + R_COMMAND, d->lba48 ? 0xEA : 0xE7);     // CACHE FLUSH: make sure it reached the platter
    delay400(d);
    if (wait_not_busy(d, WAIT_LONG) != 0) return -1;
    if (inb(d->io + R_STATUS) & (ST_ERR | ST_DF)) return -1;
    return 0;
}
