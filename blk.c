#include "kernel.h"

static struct disk disks[MAX_DISKS];
static int ndisks = 0;

int disk_add(const struct disk *d) {
    if (ndisks >= MAX_DISKS) return -1;
    disks[ndisks] = *d;
    return ndisks++;
}

int disk_count(void) { return ndisks; }

const struct disk *disk_get(int index) {
    if (index < 0 || index >= ndisks) return 0;
    return &disks[index];
}

static const struct disk *check(int disk, unsigned int lba, unsigned int count) {
    const struct disk *d = disk_get(disk);
    if (!d || !d->usable || count == 0) return 0;
    if (lba >= d->sectors || count > d->sectors - lba) return 0;
    return d;
}

int disk_read(int disk, unsigned int lba, unsigned int count, void *buf) {
    const struct disk *d = check(disk, lba, count);
    if (!d) return -1;
    switch (d->kind) {
        case DISK_IDE:  return ata_read(d->unit, lba, count, buf);
        case DISK_AHCI: return ahci_read(d->unit, lba, count, buf);
        case DISK_NVME: return nvme_read(d->unit, lba, count, buf);
    }
    return -1;
}

int disk_write(int disk, unsigned int lba, unsigned int count, const void *buf) {
    const struct disk *d = check(disk, lba, count);
    if (!d) return -1;
    switch (d->kind) {
        case DISK_IDE:  return ata_write(d->unit, lba, count, buf);
        case DISK_AHCI: return ahci_write(d->unit, lba, count, buf);
        case DISK_NVME: return nvme_write(d->unit, lba, count, buf);
    }
    return -1;
}
