#include "kernel.h"

/* GRUB module: envelope followed by an MBR disk image. Retained in reserved RAM. */
static const unsigned char *image;
static unsigned int image_bytes;
static unsigned int image_crc;
static int pending = -1;
static unsigned char verify_buf[32 * 512];

static unsigned int rd32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned int crc32(const unsigned char *p, unsigned int n) {
    unsigned int crc = ~0u;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
static void say(const char *s) { print_string(s); newline(); }

void installer_init(unsigned int magic, unsigned int mbi) {
    if (magic != 0x2BADB002 || !mbi) return;
    const unsigned int *mb = (const unsigned int *)(unsigned long)mbi;
    if (!(mb[0] & (1u << 3)) || mb[5] > 32) return;
    const unsigned int *mods = (const unsigned int *)(unsigned long)mb[6];
    for (unsigned int i = 0; i < mb[5]; i++, mods += 4) {
        unsigned int start = mods[0], end = mods[1];
        if (end <= start || end - start < 16) continue;
        const unsigned char *p = (const unsigned char *)(unsigned long)start;
        if (memcmp(p, "IRONINST", 8)) continue;
        unsigned int size = rd32(p + 8);
        if (size != end - start - 16 || size < 2049 * 512 || (size & 511)) continue;
        const unsigned char *mbr = p + 16;
        unsigned int first = rd32(mbr + 454), count = rd32(mbr + 458);
        if (mbr[510] != 0x55 || mbr[511] != 0xAA || mbr[446] != 0x80 ||
            (mbr[450] != 0x06 && mbr[450] != 0x0E) || first != 2048 || count != size / 512 - first) continue;
        image = mbr; image_bytes = size; image_crc = rd32(p + 12);
        return;
    }
}

/* Returns 1 once disk contents may have changed, so the CLI drops stale mounts. */
int installer_command(const char *arg) {
    int old_pending = pending;
    pending = -1;
    if (!image) { say("Installer unavailable. Boot the live ISO to install IronOS."); return 0; }
    if (!*arg) {
        say("BIOS/legacy installer. Usage: install <disk-name> (see 'disks').");
        say("Replaces the selected disk's partition table and system area.");
        say("All existing partitions/data become inaccessible. Back up first.");
        return 0;
    }
    int target = -1;
    const char *tail = arg;
    while (*tail && *tail != ' ') tail++;
    unsigned int len = (unsigned int)(tail - arg);
    for (int i = 0; i < disk_count(); i++) {
        const struct disk *d = disk_get(i);
        if (strlen(d->name) == len && !memcmp(arg, d->name, len)) target = i;
    }
    const struct disk *d = disk_get(target);
    if (!d || !d->usable) { say("Not a writable disk. Use the disk name, e.g. install ide0."); return 0; }
    unsigned int sectors = image_bytes / 512;
    if (d->sectors < sectors) { say("Disk too small: installer needs at least 65 MiB."); return 0; }
    while (*tail == ' ') tail++;
    if (!*tail) {
        char num[16];
        print_string("Target: "); print_string(d->name); print_string("  ");
        print_string(d->model); print_string("  ");
        int_to_string(d->sectors / 2048, num); print_string(num); say(" MiB");
        say("WARNING: this destroys existing partitions on this disk.");
        say("BIOS/legacy boot only. A 64 MiB FAT partition is created; remainder unused.");
        print_string("To confirm type: install "); print_string(d->name); say(" ERASE");
        pending = target;
        return 0;
    }
    if (strcmp(tail, "ERASE") || target != old_pending) {
        say("Not confirmed. First run install <disk-name> and review the target."); return 0;
    }
    say("Checking installation payload...");
    if (crc32(image, image_bytes) != image_crc) { say("Invalid payload checksum. Disk untouched."); return 0; }
    /* Stop using any cached FAT state before raw writes. MBR is committed last. */
    fat_unmount();
    say("Writing and verifying IronOS. Do not power off...");
    for (unsigned int lba = 1; lba < sectors;) {
        unsigned int n = sectors - lba;
        if (n > 32) n = 32;
        if (disk_write(target, lba, n, image + lba * 512) ||
            disk_read(target, lba, n, verify_buf) ||
            memcmp(verify_buf, image + lba * 512, n * 512)) {
            say("INSTALL FAILED: disk I/O or verification error. Target may be incomplete."); return 1;
        }
        lba += n;
    }
    /* Remove the old backup GPT header; old partitions are deliberately discarded. */
    if (d->sectors > sectors) {
        memset(verify_buf, 0, 512);
        if (disk_write(target, d->sectors - 1, 1, verify_buf)) {
            say("INSTALL FAILED: cannot clear old backup partition table."); return 1;
        }
    }
    if (disk_write(target, 0, 1, image) || disk_read(target, 0, 1, verify_buf) || memcmp(verify_buf, image, 512)) {
        say("INSTALL FAILED: boot sector write/verification failed."); return 1;
    }
    say("Installation complete (verified). Shut down, remove live media, boot this disk.");
    say("Use 'disks' and 'mount <volume>' to inspect the installed FAT partition.");
    return 1;
}
