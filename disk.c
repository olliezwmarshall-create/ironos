#include "kernel.h"

// Partition discovery: bare FAT disks, MBR (primary + extended/logical), and GPT.

#define MAX_VOLUMES 32

static struct volume vols[MAX_VOLUMES];
static int nvols = 0;

static unsigned int le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned long long le64(const unsigned char *p) { return le32(p) | ((unsigned long long)le32(p + 4) << 32); }

static int is_fat_type(unsigned char t) { return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E; }
static int is_extended(unsigned char t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static void add(int drive, unsigned int start, unsigned int sectors, unsigned char type, int part, int fat) {
    if (nvols >= MAX_VOLUMES) return;
    struct volume *v = &vols[nvols++];
    v->drive = drive; v->start = start; v->sectors = sectors; v->ptype = type; v->part = part; v->fat = fat;
}

// GPT partitions use pseudo types: 0xEF = EFI System Partition, 0xFA = FAT data partition.
const char *ptype_name(unsigned char t) {
    switch (t) {
        case 0x00: return "whole disk";
        case 0x01: return "FAT12";
        case 0x04: case 0x06: case 0x0E: return "FAT16";
        case 0x0B: case 0x0C: return "FAT32";
        case 0x05: case 0x0F: case 0x85: return "extended";
        case 0x07: return "NTFS/exFAT (not supported)";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux (not supported)";
        case 0xEE: return "GPT protective (GPT unreadable)";
        case 0xEF: return "EFI System";
        case 0xFA: return "FAT";
        case 0x12: case 0x1B: case 0x1C: case 0xDE: case 0xA0: return "utility/hibernation";
        case 0x27: return "Windows recovery";
        default:   return "unknown type";
    }
}

// ---- GPT ----

static unsigned int gpt_crc32(const unsigned char *p, unsigned int n) {
    unsigned int c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static const unsigned char GUID_ESP[16]    = { 0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const unsigned char GUID_MSDATA[16] = { 0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 };
static const unsigned char GUID_LINUX[16]  = { 0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4 };

// Try the primary GPT header (LBA 1), then the backup one at the end of the disk. Both are CRC-checked.
static int scan_gpt(int drive, unsigned int total) {
    unsigned char hdr[512], tmp[512], sec[512];
    for (int t = 0; t < 2; t++) {
        unsigned int at = t == 0 ? 1 : total - 1;
        if (t == 1 && total < 3) break;
        if (disk_read(drive, at, 1, hdr) != 0 || memcmp(hdr, "EFI PART", 8) != 0) continue;
        unsigned int hsz = le32(hdr + 12);
        if (hsz < 92 || hsz > 512) continue;
        memcpy(tmp, hdr, hsz);
        tmp[16] = tmp[17] = tmp[18] = tmp[19] = 0;
        if (gpt_crc32(tmp, hsz) != le32(hdr + 16)) continue;

        unsigned long long elba = le64(hdr + 72);
        unsigned int n = le32(hdr + 80), esz = le32(hdr + 84), ecrc = le32(hdr + 88);
        if (esz < 128 || esz > 512 || (esz & (esz - 1)) || n == 0 || n > 256 || (elba >> 32)) continue;
        unsigned int bytes = n * esz, secs = (bytes + 511) / 512;
        if ((unsigned long long)elba + secs > total) continue;
        unsigned char *tab = kmalloc(secs * 512);
        if (!tab) continue;
        if (disk_read(drive, (unsigned int)elba, secs, tab) != 0 || gpt_crc32(tab, bytes) != ecrc) { kfree(tab); continue; }

        for (unsigned int i = 0; i < n; i++) {
            const unsigned char *e = tab + i * esz;
            int empty = 1;
            for (int k = 0; k < 16; k++) if (e[k]) empty = 0;
            if (empty) continue;
            unsigned long long first = le64(e + 32), last = le64(e + 40);
            if ((first >> 32) || (last >> 32) || last < first || first >= total) continue;     // beyond 2 TB: unreachable
            unsigned long long size = last - first + 1;
            if (first + size > total) size = total - first;

            int esp = !memcmp(e, GUID_ESP, 16), data = !memcmp(e, GUID_MSDATA, 16), lin = !memcmp(e, GUID_LINUX, 16);
            unsigned char type = 0xFE;
            int fat = 0;
            if (esp || data) {
                if (disk_read(drive, (unsigned int)first, 1, sec) == 0) fat = fat_is_bpb(sec);
                type = esp ? 0xEF : (fat ? 0xFA : 0x07);
            } else if (lin) type = 0x83;
            add(drive, (unsigned int)first, (unsigned int)size, type, (int)i + 1, fat);
        }
        kfree(tab);
        return 1;
    }
    return 0;
}

// ---- per-disk scan ----

static void scan_drive(int drive) {
    unsigned char sec[512];
    const struct disk *d = disk_get(drive);

    if (disk_read(drive, 0, 1, sec) != 0) return;
    if (fat_is_bpb(sec)) {                                  // a bare FAT volume with no partition table
        add(drive, 0, d->sectors, 0, 0, 1);
        return;
    }
    if (sec[510] != 0x55 || sec[511] != 0xAA) return;       // no partition table at all

    for (int i = 0; i < 4; i++)                             // protective MBR -> the real table is a GPT
        if (sec[446 + i * 16 + 4] == 0xEE && scan_gpt(drive, d->sectors)) return;

    unsigned int ext_base = 0;
    for (int i = 0; i < 4; i++) {
        const unsigned char *pe = sec + 446 + i * 16;
        unsigned char type = pe[4];
        unsigned int start = le32(pe + 8), size = le32(pe + 12);
        if (type == 0 || size == 0) continue;
        if (is_extended(type)) { if (!ext_base) ext_base = start; continue; }
        add(drive, start, size, type, i + 1, is_fat_type(type));
    }

    // Logical partitions live in a chain of extended boot records inside the extended partition.
    if (ext_base) {
        unsigned int ebr = ext_base;
        int part = 5;
        for (int hops = 0; hops < 32 && ebr != 0; hops++) {
            if (disk_read(drive, ebr, 1, sec) != 0) break;
            if (sec[510] != 0x55 || sec[511] != 0xAA) break;
            const unsigned char *e0 = sec + 446, *e1 = sec + 446 + 16;
            if (e0[4] != 0 && le32(e0 + 12) != 0)
                add(drive, ebr + le32(e0 + 8), le32(e0 + 12), e0[4], part++, is_fat_type(e0[4]));
            if (is_extended(e1[4]) && le32(e1 + 8) != 0) ebr = ext_base + le32(e1 + 8);
            else break;
        }
    }
}

int disk_scan(void) {
    nvols = 0;
    for (int i = 0; i < disk_count(); i++) {
        const struct disk *d = disk_get(i);
        if (d && d->usable) scan_drive(i);
    }
    return nvols;
}

int volume_count(void) { return nvols; }

const struct volume *volume_get(int index) {
    if (index < 0 || index >= nvols) return 0;
    return &vols[index];
}
