#ifndef BLK_H
#define BLK_H

// Block-device registry: every storage driver (IDE, AHCI/SATA, NVMe) registers its disks here.
// Everything above (partition scanner, FAT) talks to disks only through disk_read/disk_write.

#define MAX_DISKS 16

enum { DISK_IDE = 1, DISK_ATAPI, DISK_AHCI, DISK_AHCI_ATAPI, DISK_NVME };

struct disk {
    int kind;                  // DISK_*
    int unit;                  // driver-specific index
    int usable;                // 1 = a real disk we can read/write (0 for CD/DVD drives)
    char name[12];             // "ide0", "sata1", "nvme0"
    char desc[24];             // "primary master", "port 2", ...
    char model[41];
    unsigned int sectors;      // 512-byte sectors reachable with 32-bit LBAs (2 TB max)
    int truncated;             // the device is bigger than that
};

int disk_add(const struct disk *d);                   // returns its index, or -1 if the table is full
int disk_count(void);
const struct disk *disk_get(int index);
int disk_read(int disk, unsigned int lba, unsigned int count, void *buf);              // 0 = ok
int disk_write(int disk, unsigned int lba, unsigned int count, const void *buf);      // 0 = ok

// Drivers (each registers what it finds with disk_add)
int ata_init(void);                                   // IDE: legacy ports or PCI native mode, 28/48-bit LBA
int ata_read(int unit, unsigned int lba, unsigned int count, void *buf);
int ata_write(int unit, unsigned int lba, unsigned int count, const void *buf);
int ahci_init(void);                                  // SATA in AHCI mode
int ahci_read(int unit, unsigned int lba, unsigned int count, void *buf);
int ahci_write(int unit, unsigned int lba, unsigned int count, const void *buf);
int nvme_init(void);                                  // NVMe SSDs
int nvme_read(int unit, unsigned int lba, unsigned int count, void *buf);
int nvme_write(int unit, unsigned int lba, unsigned int count, const void *buf);

#endif
