#include "kernel.h"

#define PATH_MAX_LEN 128
#define NO_SECTOR    0xFFFFFFFFu

// ---- volume layout (filled in by fat_mount) ----
static int mounted = 0;
static int writable = 0;
static const char *status = "not mounted";
static int cur_drive;
static unsigned int fat_bits;
static unsigned int part_lba;         // first sector of the volume on the disk
static unsigned int part_sectors;     // size of the partition (writes are never allowed outside it)
static unsigned int fat_start;        // first sector of the (first) FAT
static unsigned int fat_size;         // sectors per FAT
static unsigned int nfats;
static unsigned int root_start;       // FAT12/16: first sector of the fixed root directory
static unsigned int root_sectors;     // FAT12/16: length of the fixed root directory
static unsigned int data_start;       // first sector of cluster 2
static unsigned int spc;              // sectors per cluster
static unsigned int root_cluster;     // FAT32: root directory cluster; 0 on FAT12/16 (fixed root)
static unsigned int total_clusters;
static unsigned int fsinfo_sector;    // FAT32 FSInfo sector (relative to the volume), 0 if none
static int fsinfo_done;
static unsigned int next_hint = 2;

static unsigned int cwd_cluster = 0;
static char cwd_path[PATH_MAX_LEN] = "/";

// One-sector caches: one for directory/file data, one for the FAT itself.
static unsigned char sec_buf[512];
static unsigned int sec_lba = NO_SECTOR;
static unsigned char fat_buf[512];
static unsigned int fat_lba = NO_SECTOR;
static const unsigned char zero_sec[512];

void fat_unmount(void) {
    mounted = writable = 0;
    status = "not mounted";
    sec_lba = fat_lba = NO_SECTOR;
    cwd_cluster = 0;
    cwd_path[0] = '/'; cwd_path[1] = 0;
}

static unsigned int rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned int rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}
static void wr16(unsigned char *p, unsigned int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

const char *fat_strerror(int err) {
    switch (err) {
        case FAT_OK:           return "ok";
        case FAT_ERR_NOTFOUND: return "no such file or directory";
        case FAT_ERR_NOTDIR:   return "not a directory";
        case FAT_ERR_ISDIR:    return "is a directory";
        case FAT_ERR_EXISTS:   return "already exists";
        case FAT_ERR_FULL:     return "disk is full";
        case FAT_ERR_DIRFULL:  return "root directory is full";
        case FAT_ERR_RO:       return "volume is read-only (use 'mount N rw')";
        case FAT_ERR_IO:       return "disk I/O error";
        case FAT_ERR_NAME:     return "invalid file name";
        case FAT_ERR_NOTEMPTY: return "directory is not empty";
        case FAT_ERR_BUSY:     return "that is the current directory";
        case FAT_ERR_INVAL:    return "invalid operation";
        case FAT_ERR_PERM:     return "file is marked read-only";
        case FAT_ERR_NOFS:     return "no filesystem mounted";
        default:               return "error";
    }
}

// ---- block access (confined to the mounted partition) ----

static int blk_read(unsigned int lba, unsigned int count, void *buf) {
    return disk_read(cur_drive, lba, count, buf);
}

static int blk_write(unsigned int lba, unsigned int count, const void *buf) {
    if (!writable) return -1;
    if (lba < part_lba || count == 0 || count > part_sectors || lba - part_lba > part_sectors - count) return -1;
    return disk_write(cur_drive, lba, count, buf);
}

static const unsigned char *load_sector(unsigned int lba) {
    if (lba != sec_lba) {
        if (blk_read(lba, 1, sec_buf) != 0) { sec_lba = NO_SECTOR; return 0; }
        sec_lba = lba;
    }
    return sec_buf;
}

// Modify one sector in place (read-modify-write through the cache).
static int patch_sector(unsigned int lba, unsigned int offset, const unsigned char *bytes, unsigned int n) {
    if (!load_sector(lba)) return -1;
    memcpy(sec_buf + offset, bytes, n);
    if (blk_write(lba, 1, sec_buf) != 0) { sec_lba = NO_SECTOR; return -1; }
    return 0;
}

// ---- the FAT ----

static int fat_byte(unsigned int off) {
    unsigned int sector = off / 512;
    if (sector >= fat_size) return -1;
    unsigned int lba = fat_start + sector;
    if (lba != fat_lba) {
        if (blk_read(lba, 1, fat_buf) != 0) { fat_lba = NO_SECTOR; return -1; }
        fat_lba = lba;
    }
    return fat_buf[off % 512];
}

// Read the FAT entry for cluster c. 0 on success.
static int fat_entry(unsigned int c, unsigned int *out) {
    if (fat_bits == 12) {
        unsigned int off = c + c / 2;
        int b0 = fat_byte(off), b1 = fat_byte(off + 1);
        if (b0 < 0 || b1 < 0) return -1;
        unsigned int v = (unsigned int)b0 | ((unsigned int)b1 << 8);
        *out = (c & 1) ? (v >> 4) : (v & 0xFFF);
        return 0;
    }
    if (fat_bits == 16) {
        unsigned int off = c * 2;
        int b0 = fat_byte(off), b1 = fat_byte(off + 1);
        if (b0 < 0 || b1 < 0) return -1;
        *out = (unsigned int)b0 | ((unsigned int)b1 << 8);
        return 0;
    }
    unsigned int off = c * 4;
    int b0 = fat_byte(off), b1 = fat_byte(off + 1), b2 = fat_byte(off + 2), b3 = fat_byte(off + 3);
    if (b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0) return -1;
    *out = ((unsigned int)b0 | ((unsigned int)b1 << 8) | ((unsigned int)b2 << 16) | ((unsigned int)b3 << 24)) & 0x0FFFFFFF;
    return 0;
}

static int valid_cluster(unsigned int c) {
    return c >= 2 && c < total_clusters + 2;
}

// Next cluster in a chain. Anything outside the valid range (end-of-chain, bad cluster, 0, I/O error) ends it.
static unsigned int fat_next(unsigned int c) {
    unsigned int v;
    return fat_entry(c, &v) == 0 ? v : 0;
}

// Write one FAT entry (FAT16/32 only) to every copy of the FAT.
static int fat_set(unsigned int c, unsigned int value) {
    unsigned int off = (fat_bits == 16) ? c * 2 : c * 4;
    unsigned int sector = off / 512, pos = off % 512;
    if (fat_bits == 12 || sector >= fat_size) return -1;

    unsigned int lba = fat_start + sector;
    if (lba != fat_lba) {
        if (blk_read(lba, 1, fat_buf) != 0) { fat_lba = NO_SECTOR; return -1; }
        fat_lba = lba;
    }
    if (fat_bits == 16) wr16(fat_buf + pos, value);
    else wr32(fat_buf + pos, (rd32(fat_buf + pos) & 0xF0000000u) | (value & 0x0FFFFFFFu));

    for (unsigned int k = 0; k < nfats; k++) {
        if (blk_write(fat_start + k * fat_size + sector, 1, fat_buf) != 0) { fat_lba = NO_SECTOR; return -1; }
    }
    return 0;
}

static unsigned int eoc_mark(void) { return fat_bits == 16 ? 0xFFFF : 0x0FFFFFFF; }

static unsigned int cluster_lba(unsigned int c) {
    return data_start + (c - 2) * spc;
}

// Find a free cluster and mark it end-of-chain. Returns 0 if the disk is full (or on I/O error).
static unsigned int alloc_cluster(void) {
    for (unsigned int i = 0; i < total_clusters; i++) {
        unsigned int c = 2 + (next_hint - 2 + i) % total_clusters;
        unsigned int v;
        if (fat_entry(c, &v) != 0) return 0;               // never guess on a read error
        if (v == 0) {
            if (fat_set(c, eoc_mark()) != 0) return 0;
            next_hint = c + 1;
            if (next_hint >= total_clusters + 2) next_hint = 2;
            return c;
        }
    }
    return 0;
}

static void free_chain(unsigned int c) {
    unsigned int steps = 0;
    while (valid_cluster(c) && steps++ <= total_clusters) {
        unsigned int next;
        if (fat_entry(c, &next) != 0) return;
        if (fat_set(c, 0) != 0) return;
        if (c < next_hint) next_hint = c;
        c = next;
    }
}

// Allocate `count` clusters linked into one chain. Returns the first cluster, or 0 (nothing left allocated).
static unsigned int alloc_chain(unsigned int count) {
    unsigned int first = 0, prev = 0;
    for (unsigned int i = 0; i < count; i++) {
        unsigned int c = alloc_cluster();
        if (!c || (prev && fat_set(prev, c) != 0)) {
            if (first) free_chain(first);
            return 0;
        }
        if (!first) first = c;
        prev = c;
    }
    return first;
}

static int zero_cluster(unsigned int c) {
    for (unsigned int s = 0; s < spc; s++)
        if (blk_write(cluster_lba(c) + s, 1, zero_sec) != 0) return -1;
    return 0;
}

// Write `size` bytes of data into an already-allocated chain (last sector zero-padded).
static int write_data(unsigned int c, const unsigned char *data, unsigned int size) {
    unsigned int remaining = size, steps = 0;
    while (remaining > 0) {
        if (!valid_cluster(c) || steps++ > total_clusters) return -1;
        unsigned int lba = cluster_lba(c);
        unsigned int n = remaining < spc * 512 ? remaining : spc * 512;
        unsigned int full = n / 512;
        if (full > 0) {
            if (blk_write(lba, full, data) != 0) return -1;
            data += full * 512;
        }
        unsigned int rem = n - full * 512;
        if (rem > 0) {
            unsigned char tmp[512];
            memset(tmp, 0, sizeof tmp);
            memcpy(tmp, data, rem);
            if (blk_write(lba + full, 1, tmp) != 0) return -1;
            data += rem;
        }
        remaining -= n;
        if (remaining > 0) c = fat_next(c);
    }
    return 0;
}

// FAT32: the FSInfo free-cluster count and "next free" hint go stale when we allocate; mark them unknown.
static int invalidate_fsinfo(void) {
    if (fsinfo_done) return 0;
    fsinfo_done = 1;
    if (fat_bits != 32 || fsinfo_sector == 0) return 0;
    unsigned int lba = part_lba + fsinfo_sector;
    unsigned char b[512];
    if (blk_read(lba, 1, b) != 0) return -1;
    if (rd32(b) != 0x41615252u || rd32(b + 484) != 0x61417272u) return 0;   // not a valid FSInfo sector
    wr32(b + 488, 0xFFFFFFFFu);
    wr32(b + 492, 0xFFFFFFFFu);
    return blk_write(lba, 1, b);
}

// ---- mounting ----

// A FAT boot sector (as opposed to an MBR) has sane BPB fields at fixed offsets.
int fat_is_bpb(const unsigned char *s) {
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    if (rd16(s + 11) != 512) return 0;
    unsigned int c = s[13];
    if (c == 0 || (c & (c - 1)) != 0) return 0;
    if (rd16(s + 14) == 0) return 0;
    if (s[16] == 0 || s[16] > 2) return 0;
    return 1;
}

int fat_mount(int drive, unsigned int start_lba, unsigned int vol_sectors, int want_write) {
    mounted = 0;
    writable = 0;
    fsinfo_done = 0;
    next_hint = 2;
    sec_lba = NO_SECTOR;
    fat_lba = NO_SECTOR;
    cur_drive = drive;
    part_lba = start_lba;
    part_sectors = vol_sectors;

    const struct disk *d = disk_get(drive);
    if (!d || !d->usable) { status = "no such disk"; return -1; }

    const unsigned char *s = load_sector(start_lba);
    if (!s) { status = "disk read error"; return -1; }
    if (!fat_is_bpb(s)) { status = "not a valid FAT volume"; return -1; }

    unsigned int reserved = rd16(s + 14);
    nfats = s[16];
    unsigned int root_entries = rd16(s + 17);
    unsigned int total = rd16(s + 19);
    if (total == 0) total = rd32(s + 32);
    unsigned int fatsz = rd16(s + 22);
    if (fatsz == 0) fatsz = rd32(s + 36);
    unsigned int fat32_root = rd32(s + 44);
    unsigned int fsinfo = rd16(s + 48);
    spc = s[13];

    if (fatsz == 0 || total == 0) { status = "corrupt FAT header"; return -1; }
    if (total > vol_sectors) { status = "filesystem is larger than its partition"; return -1; }

    root_sectors = (root_entries * 32 + 511) / 512;
    fat_start = start_lba + reserved;
    fat_size = fatsz;
    root_start = fat_start + nfats * fatsz;
    data_start = root_start + root_sectors;

    unsigned int overhead = reserved + nfats * fatsz + root_sectors;
    if (total <= overhead) { status = "corrupt FAT header"; return -1; }
    total_clusters = (total - overhead) / spc;

    fat_bits = total_clusters < 4085 ? 12 : (total_clusters < 65525 ? 16 : 32);
    if (root_entries == 0 && rd16(s + 22) == 0) fat_bits = 32;   // FAT32 headers have no fixed root / 16-bit FAT size
    root_cluster = (fat_bits == 32) ? fat32_root : 0;
    if (fat_bits == 32 && root_cluster < 2) { status = "corrupt FAT32 root"; return -1; }
    fsinfo_sector = (fat_bits == 32 && fsinfo != 0 && fsinfo != 0xFFFF) ? fsinfo : 0;

    cwd_cluster = root_cluster;
    cwd_path[0] = '/';
    cwd_path[1] = '\0';

    mounted = 1;
    if (want_write && fat_bits == 12) {
        status = "mounted read-only (FAT12 is read-only)";
    } else if (want_write) {
        writable = 1;
        status = "mounted read-write";
    } else {
        status = "mounted read-only";
    }
    return 0;
}

int fat_mounted(void) { return mounted; }
int fat_writable(void) { return mounted && writable; }
const char *fat_status(void) { return status; }
const char *fat_getcwd(void) { return cwd_path; }

void fat_get_info(struct fat_info *info) {
    info->bits = fat_bits;
    info->start_lba = part_lba;
    info->cluster_bytes = spc * 512;
    info->total_clusters = total_clusters;
    info->drive = cur_drive;
    info->writable = writable;
}

int fat_free_clusters(unsigned int *free_count) {
    if (!mounted) return FAT_ERR_NOFS;
    unsigned int n = 0;
    for (unsigned int c = 2; c < total_clusters + 2; c++) {
        unsigned int v;
        if (fat_entry(c, &v) != 0) return FAT_ERR_IO;
        if (v == 0) n++;
    }
    *free_count = n;
    return 0;
}

// ---- directory reading ----

struct dir_iter {
    unsigned int start;      // 0 = fixed root directory (FAT12/16), otherwise first cluster
    unsigned int cluster;
    unsigned int sec;        // sector within the current cluster (or within the fixed root)
    unsigned int idx;        // 32-byte entry within the current sector
    unsigned int steps;      // clusters walked, to stop on a looping FAT chain
    int done;
    unsigned int last_lba;   // where the entry most recently returned by iter_raw lives
    unsigned int last_idx;
};

// Where the directory entries of one file live (long-name fragments first, the 8.3 entry last).
struct entry_loc {
    unsigned int lba[22];
    unsigned char idx[22];
    int n;
};

static void iter_init(struct dir_iter *it, unsigned int start) {
    memset(it, 0, sizeof *it);
    it->start = start;
    it->cluster = start;
}

// Next raw 32-byte directory entry (valid until the next data read), or 0 at the end.
static const unsigned char *iter_raw(struct dir_iter *it) {
    while (!it->done) {
        unsigned int lba;
        if (it->start == 0) {
            if (it->sec >= root_sectors) { it->done = 1; break; }
            lba = root_start + it->sec;
        } else {
            if (!valid_cluster(it->cluster)) { it->done = 1; break; }
            lba = cluster_lba(it->cluster) + it->sec;
        }

        const unsigned char *b = load_sector(lba);
        if (!b) { it->done = 1; break; }

        if (it->idx < 16) {
            const unsigned char *e = b + it->idx * 32;
            it->last_lba = lba;
            it->last_idx = it->idx;
            it->idx++;
            if (e[0] == 0x00) { it->done = 1; break; }   // end of directory
            return e;
        }

        it->idx = 0;
        it->sec++;
        if (it->start != 0 && it->sec >= spc) {
            it->sec = 0;
            it->cluster = fat_next(it->cluster);
            if (++it->steps > total_clusters) it->done = 1;
        }
    }
    return 0;
}

static unsigned char short_checksum(const unsigned char *name11) {
    unsigned char sum = 0;
    for (int i = 0; i < 11; i++) sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

// Next real entry (skips deleted entries and the volume label), assembling long names on the way.
// If loc is given it receives the on-disk positions of all entries belonging to this file.
static int iter_next(struct dir_iter *it, struct fat_dirent *out, struct entry_loc *loc) {
    static const unsigned char lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    char lfn[FAT_NAME_MAX + 1];
    unsigned int glba[20];
    unsigned char gidx[20];
    int grp_n = 0;
    int have_lfn = 0;
    unsigned char lfn_sum = 0;
    const unsigned char *e;

    while ((e = iter_raw(it)) != 0) {
        if (e[0] == 0xE5) { have_lfn = 0; grp_n = 0; continue; }   // deleted
        unsigned char attr = e[11];

        if (attr == 0x0F) {                                     // long-name fragment
            unsigned int order = e[0] & 0x1F;
            if (e[0] & 0x40) {                                  // the last fragment comes first on disk
                memset(lfn, 0, sizeof lfn);
                have_lfn = 1;
                lfn_sum = e[13];
                grp_n = 0;
            }
            if (!have_lfn || order == 0 || order > 20 || e[13] != lfn_sum) { have_lfn = 0; grp_n = 0; continue; }
            if (grp_n < 20) { glba[grp_n] = it->last_lba; gidx[grp_n] = (unsigned char)it->last_idx; grp_n++; }
            unsigned int base = (order - 1) * 13;
            for (int i = 0; i < 13; i++) {
                unsigned int ch = rd16(e + lfn_off[i]);
                if (ch == 0x0000 || ch == 0xFFFF) break;
                if (base + i < FAT_NAME_MAX) lfn[base + i] = ch < 0x80 ? (char)ch : '?';
            }
            continue;
        }

        if (attr & 0x08) { have_lfn = 0; grp_n = 0; continue; } // volume label

        // Short (8.3) entry. Bits 3/4 of byte 12 say the base name / extension are lower case.
        unsigned char nt = e[12];
        int n = 0;
        for (int i = 0; i < 8 && e[i] != ' '; i++) {
            char c = (i == 0 && e[0] == 0x05) ? (char)0xE5 : (char)e[i];
            if ((nt & 0x08) && c >= 'A' && c <= 'Z') c += 32;
            out->sname[n++] = c;
        }
        if (e[8] != ' ') {
            out->sname[n++] = '.';
            for (int i = 8; i < 11 && e[i] != ' '; i++) {
                char c = (char)e[i];
                if ((nt & 0x10) && c >= 'A' && c <= 'Z') c += 32;
                out->sname[n++] = c;
            }
        }
        out->sname[n] = '\0';

        int lfn_ok = have_lfn && lfn_sum == short_checksum(e) && lfn[0] != '\0';
        if (lfn_ok) {
            memcpy(out->name, lfn, sizeof lfn);
            out->name[FAT_NAME_MAX] = '\0';
        } else {
            memcpy(out->name, out->sname, (unsigned int)n + 1);
        }

        if (loc) {
            loc->n = 0;
            if (lfn_ok) {
                for (int i = 0; i < grp_n; i++) { loc->lba[loc->n] = glba[i]; loc->idx[loc->n] = gidx[i]; loc->n++; }
            }
            loc->lba[loc->n] = it->last_lba;
            loc->idx[loc->n] = (unsigned char)it->last_idx;
            loc->n++;
        }

        out->attr = attr;
        out->cluster = rd16(e + 26) | (fat_bits == 32 ? (rd16(e + 20) << 16) : 0);
        out->size = rd32(e + 28);
        return 1;
    }
    return 0;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static char upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int streq_ic(const char *a, const char *b) {
    while (*a && *b) {
        if (lower(*a) != lower(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int dir_find_loc(unsigned int dir, const char *name, struct fat_dirent *out, struct entry_loc *loc) {
    struct dir_iter it;
    iter_init(&it, dir);
    while (iter_next(&it, out, loc)) {
        if (streq_ic(out->name, name) || streq_ic(out->sname, name)) return 1;
    }
    return 0;
}

static int dir_find(unsigned int dir, const char *name, struct fat_dirent *out) {
    return dir_find_loc(dir, name, out, 0);
}

int fat_list_dir(const struct fat_dirent *dir, fat_list_cb cb, void *ctx) {
    if (!mounted || !(dir->attr & FAT_ATTR_DIR)) return -1;
    struct dir_iter it;
    struct fat_dirent e;
    iter_init(&it, dir->cluster ? dir->cluster : root_cluster);
    while (iter_next(&it, &e, 0)) cb(&e, ctx);
    return 0;
}

// ---- paths ----

static void make_dir_entry(struct fat_dirent *out, unsigned int cluster, const char *name) {
    memset(out, 0, sizeof *out);
    unsigned int n = strlen(name);
    memcpy(out->name, name, n + 1);
    memcpy(out->sname, name, n + 1);
    out->attr = FAT_ATTR_DIR;
    out->cluster = cluster;
}

static int path_push(char *path, const char *name) {
    unsigned int len = strlen(path), nlen = strlen(name);
    if (len + nlen + 2 > PATH_MAX_LEN) return -1;
    if (len > 1) path[len++] = '/';
    memcpy(path + len, name, nlen + 1);
    return 0;
}

static void path_pop(char *path) {
    unsigned int len = strlen(path);
    while (len > 1 && path[len - 1] != '/') len--;
    if (len > 1) len--;
    path[len] = '\0';
}

// Resolve `path` (absolute, or relative to the current directory). If newpath is given it receives the
// normalised absolute path, built from the real names on disk.
static int walk(const char *path, struct fat_dirent *out, char *newpath) {
    int absolute = (path[0] == '/');
    unsigned int dir = absolute ? root_cluster : cwd_cluster;

    if (newpath) {
        if (absolute) { newpath[0] = '/'; newpath[1] = '\0'; }
        else memcpy(newpath, cwd_path, strlen(cwd_path) + 1);
    }
    make_dir_entry(out, dir, absolute ? "/" : ".");

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[FAT_NAME_MAX + 1];
        unsigned int n = 0;
        while (*p && *p != '/') {
            if (n < FAT_NAME_MAX) comp[n++] = *p;
            p++;
        }
        comp[n] = '\0';

        if (!(out->attr & FAT_ATTR_DIR)) return -1;             // trying to descend into a file
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0 && dir == root_cluster) {   // the root has no parent: stay put
            make_dir_entry(out, dir, "/");
            continue;
        }
        if (!dir_find(dir, comp, out)) return -1;

        if (newpath) {
            if (strcmp(comp, "..") == 0) path_pop(newpath);
            else if (path_push(newpath, out->name) < 0) return -1;
        }
        dir = out->cluster ? out->cluster : root_cluster;       // ".." of a top-level dir says cluster 0
    }
    return 0;
}

int fat_lookup(const char *path, struct fat_dirent *out) {
    if (!mounted) return -1;
    return walk(path, out, 0);
}

int fat_chdir(const char *path) {
    if (!mounted) return -1;
    struct fat_dirent e;
    char np[PATH_MAX_LEN];
    if (walk(path, &e, np) < 0) return -1;
    if (!(e.attr & FAT_ATTR_DIR)) return -2;
    cwd_cluster = e.cluster ? e.cluster : root_cluster;
    memcpy(cwd_path, np, strlen(np) + 1);
    return 0;
}

// ---- file reading ----

int fat_read_file(const struct fat_dirent *file, void *buf, unsigned int max) {
    if (!mounted) return -1;
    unsigned int want = file->size < max ? file->size : max;
    unsigned char *out = (unsigned char *)buf;
    unsigned int done = 0, c = file->cluster, steps = 0;

    while (done < want && valid_cluster(c) && steps++ <= total_clusters) {
        unsigned int lba = cluster_lba(c);

        if (want - done >= spc * 512) {                         // whole cluster: one big read
            if (blk_read(lba, spc, out + done) != 0) return -1;
            done += spc * 512;
        } else {                                                // tail of the file: sector by sector
            for (unsigned int s = 0; s < spc && done < want; s++) {
                unsigned int chunk = want - done < 512 ? want - done : 512;
                const unsigned char *b = load_sector(lba + s);
                if (!b) return -1;
                memcpy(out + done, b, chunk);
                done += chunk;
            }
        }
        c = fat_next(c);
    }
    return (int)done;
}

// ======================================================================================
// Writing
// ======================================================================================

static void stamp(unsigned int *date, unsigned int *time) {
    struct rtc_time t;
    if (rtc_get(&t) != 0 || t.year < 1980 || t.year > 2107) { *date = (1 << 5) | 1; *time = 0; return; }
    *date = ((t.year - 1980) << 9) | (t.month << 5) | t.day;
    *time = (t.hour << 11) | (t.minute << 5) | (t.second / 2);
}

static int valid_short_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    for (const char *p = "$%'-_@~!(){}^#&"; *p; p++) if (*p == c) return 1;
    return 0;
}

static int name_ok(const char *name) {
    unsigned int len = strlen(name);
    if (len == 0 || len > FAT_NAME_MAX) return 0;
    if (name[0] == ' ' || name[len - 1] == ' ' || name[len - 1] == '.') return 0;
    for (unsigned int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7F) return 0;
        for (const char *p = "\"*/:<>?\\|"; *p; p++) if ((char)c == *p) return 0;
    }
    return 1;
}

struct new_name {
    unsigned char short11[11];
    unsigned char nt;
    int lfn;                     // needs long-name entries
};

// Can `name` be stored as a plain 8.3 entry (optionally with the lower-case flags)?
static int classify_83(const char *name, struct new_name *nn) {
    unsigned int len = strlen(name);
    int dots = 0;
    unsigned int dotpos = 0;
    for (unsigned int i = 0; i < len; i++) if (name[i] == '.') { dots++; dotpos = i; }
    if (dots > 1) return 0;
    unsigned int blen = dots ? dotpos : len;
    unsigned int estart = dots ? dotpos + 1 : len;
    unsigned int elen = len - estart;
    if (blen == 0 || blen > 8 || elen > 3) return 0;

    int bl = 0, bu = 0, el = 0, eu = 0;
    for (unsigned int i = 0; i < blen; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') bl = 1; else if (c >= 'A' && c <= 'Z') bu = 1;
        if (!valid_short_char(upper(c))) return 0;
    }
    for (unsigned int i = 0; i < elen; i++) {
        char c = name[estart + i];
        if (c >= 'a' && c <= 'z') el = 1; else if (c >= 'A' && c <= 'Z') eu = 1;
        if (!valid_short_char(upper(c))) return 0;
    }
    if ((bl && bu) || (el && eu)) return 0;                 // mixed case needs a long name

    memset(nn->short11, ' ', 11);
    for (unsigned int i = 0; i < blen; i++) nn->short11[i] = (unsigned char)upper(name[i]);
    for (unsigned int i = 0; i < elen; i++) nn->short11[8 + i] = (unsigned char)upper(name[estart + i]);
    nn->nt = (unsigned char)((bl ? 0x08 : 0) | (el ? 0x10 : 0));
    nn->lfn = 0;
    return 1;
}

static int short_exists(unsigned int dir, const unsigned char *short11) {
    struct dir_iter it;
    iter_init(&it, dir);
    const unsigned char *e;
    while ((e = iter_raw(&it)) != 0) {
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
        if (memcmp(e, short11, 11) == 0) return 1;
    }
    return 0;
}

// Build a unique "LONGNA~1.TXT"-style alias for a long name.
static int gen_alias(unsigned int dir, const char *name, struct new_name *nn) {
    unsigned int len = strlen(name);
    int lastdot = -1;
    for (unsigned int i = 0; i < len; i++) if (name[i] == '.') lastdot = (int)i;
    unsigned int bend = lastdot >= 0 ? (unsigned int)lastdot : len;

    char base[9];
    unsigned int bn = 0;
    for (unsigned int i = 0; i < bend && bn < 8; i++) {
        char c = upper(name[i]);
        if (valid_short_char(c)) base[bn++] = c;
    }
    if (bn == 0) base[bn++] = '_';
    base[bn] = '\0';

    char ext[4];
    unsigned int en = 0;
    if (lastdot >= 0) {
        for (unsigned int i = (unsigned int)lastdot + 1; i < len && en < 3; i++) {
            char c = upper(name[i]);
            if (valid_short_char(c)) ext[en++] = c;
        }
    }

    for (unsigned int n = 1; n < 1000000; n++) {
        char tail[8];
        int tl = 0;
        tail[tl++] = '~';
        char digits[8];
        int dn = 0;
        for (unsigned int v = n; v > 0; v /= 10) digits[dn++] = (char)('0' + v % 10);
        while (dn > 0) tail[tl++] = digits[--dn];

        unsigned int keep = 8 - (unsigned int)tl;
        if (keep > bn) keep = bn;
        memset(nn->short11, ' ', 11);
        memcpy(nn->short11, base, keep);
        memcpy(nn->short11 + keep, tail, (unsigned int)tl);
        memcpy(nn->short11 + 8, ext, en);

        if (!short_exists(dir, nn->short11)) { nn->nt = 0; nn->lfn = 1; return 0; }
    }
    return FAT_ERR_EXISTS;
}

static int make_names(unsigned int dir, const char *name, struct new_name *nn) {
    if (classify_83(name, nn) && !short_exists(dir, nn->short11)) return 0;
    return gen_alias(dir, name, nn);
}

// Fill `ents` with the directory entries for a new file. Returns how many (long-name fragments + 1).
static int build_entries(const char *name, const struct new_name *nn, unsigned char attr,
                         unsigned int cluster, unsigned int size, unsigned char ents[22][32]) {
    static const unsigned char lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int n = 0;
    unsigned int len = strlen(name);

    if (nn->lfn) {
        unsigned int pieces = (len + 12) / 13;
        unsigned char sum = short_checksum(nn->short11);
        for (unsigned int k = pieces; k >= 1; k--) {
            unsigned char *e = ents[n++];
            memset(e, 0, 32);
            e[0] = (unsigned char)(k | (k == pieces ? 0x40 : 0));
            e[11] = 0x0F;
            e[13] = sum;
            for (int i = 0; i < 13; i++) {
                unsigned int idx = (k - 1) * 13 + (unsigned int)i;
                unsigned int ch = idx < len ? (unsigned char)name[idx] : (idx == len ? 0x0000 : 0xFFFF);
                wr16(e + lfn_off[i], ch);
            }
        }
    }

    unsigned int date, time;
    stamp(&date, &time);
    unsigned char *e = ents[n++];
    memset(e, 0, 32);
    memcpy(e, nn->short11, 11);
    e[11] = attr;
    e[12] = nn->nt;
    wr16(e + 14, time);  wr16(e + 16, date);      // created
    wr16(e + 18, date);                            // last accessed
    wr16(e + 20, cluster >> 16);
    wr16(e + 22, time);  wr16(e + 24, date);       // last written
    wr16(e + 26, cluster & 0xFFFF);
    wr32(e + 28, size);
    return n;
}

// Find `need` consecutive free slots in a directory, growing the directory if it is a cluster chain.
static int dir_alloc_slots(unsigned int dir, int need, struct entry_loc *loc) {
    int run = 0;

    if (dir == 0) {                                         // FAT12/16 fixed root: cannot grow
        for (unsigned int s = 0; s < root_sectors; s++) {
            unsigned int lba = root_start + s;
            const unsigned char *b = load_sector(lba);
            if (!b) return FAT_ERR_IO;
            for (unsigned int i = 0; i < 16; i++) {
                if (b[i * 32] == 0xE5 || b[i * 32] == 0x00) {
                    loc->lba[run] = lba; loc->idx[run] = (unsigned char)i;
                    if (++run == need) { loc->n = need; return 0; }
                } else run = 0;
            }
        }
        return FAT_ERR_DIRFULL;
    }

    unsigned int cluster = dir, last = dir, steps = 0;
    while (valid_cluster(cluster) && steps++ <= total_clusters) {
        for (unsigned int s = 0; s < spc; s++) {
            unsigned int lba = cluster_lba(cluster) + s;
            const unsigned char *b = load_sector(lba);
            if (!b) return FAT_ERR_IO;
            for (unsigned int i = 0; i < 16; i++) {
                if (b[i * 32] == 0xE5 || b[i * 32] == 0x00) {
                    loc->lba[run] = lba; loc->idx[run] = (unsigned char)i;
                    if (++run == need) { loc->n = need; return 0; }
                } else run = 0;
            }
        }
        last = cluster;
        cluster = fat_next(cluster);
    }

    for (;;) {                                              // out of room: append a fresh, zeroed cluster
        unsigned int nc = alloc_cluster();
        if (!nc) return FAT_ERR_FULL;
        if (zero_cluster(nc) != 0 || fat_set(last, nc) != 0) { free_chain(nc); return FAT_ERR_IO; }
        last = nc;
        for (unsigned int s = 0; s < spc; s++) {
            for (unsigned int i = 0; i < 16; i++) {
                loc->lba[run] = cluster_lba(nc) + s; loc->idx[run] = (unsigned char)i;
                if (++run == need) { loc->n = need; return 0; }
            }
        }
    }
}

static int write_entries(const struct entry_loc *loc, unsigned char ents[22][32], int n) {
    for (int i = 0; i < n; i++)
        if (patch_sector(loc->lba[i], loc->idx[i] * 32u, ents[i], 32) != 0) return FAT_ERR_IO;
    return 0;
}

static int remove_entries(const struct entry_loc *loc) {
    unsigned char del = 0xE5;
    for (int i = 0; i < loc->n; i++)
        if (patch_sector(loc->lba[i], loc->idx[i] * 32u, &del, 1) != 0) return FAT_ERR_IO;
    return 0;
}

// Change the first cluster / size / times of an existing 8.3 entry.
static int update_short(const struct entry_loc *loc, unsigned int cluster, unsigned int size) {
    unsigned int lba = loc->lba[loc->n - 1];
    unsigned int off = loc->idx[loc->n - 1] * 32u;
    unsigned int date, time;
    stamp(&date, &time);
    const unsigned char *b = load_sector(lba);
    if (!b) return FAT_ERR_IO;
    unsigned char e[32];
    memcpy(e, b + off, 32);
    e[11] |= FAT_ATTR_ARCHIVE;
    wr16(e + 18, date);
    wr16(e + 20, cluster >> 16);
    wr16(e + 22, time);
    wr16(e + 24, date);
    wr16(e + 26, cluster & 0xFFFF);
    wr32(e + 28, size);
    return patch_sector(lba, off, e, 32) == 0 ? 0 : FAT_ERR_IO;
}

// Split "dir/leaf" and resolve the directory part. Trailing slashes are ignored.
static int resolve_parent(const char *path, unsigned int *dir, char *leaf) {
    char parent[PATH_MAX_LEN * 2];
    unsigned int len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;

    int slash = -1;
    for (unsigned int i = 0; i < len; i++) if (path[i] == '/') slash = (int)i;

    const char *lf;
    unsigned int lflen;
    if (slash < 0) {
        parent[0] = '.'; parent[1] = '\0';
        lf = path; lflen = len;
    } else {
        unsigned int plen = slash == 0 ? 1 : (unsigned int)slash;
        if (plen + 1 > sizeof parent) return FAT_ERR_NAME;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
        lf = path + slash + 1;
        lflen = len - (unsigned int)slash - 1;
    }
    if (lflen == 0 || lflen > FAT_NAME_MAX) return FAT_ERR_NAME;
    memcpy(leaf, lf, lflen);
    leaf[lflen] = '\0';
    if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) return FAT_ERR_NAME;

    struct fat_dirent pe;
    if (walk(parent, &pe, 0) < 0) return FAT_ERR_NOTFOUND;
    if (!(pe.attr & FAT_ATTR_DIR)) return FAT_ERR_NOTDIR;
    *dir = pe.cluster ? pe.cluster : root_cluster;
    return 0;
}

static int check_writable(void) {
    if (!mounted) return FAT_ERR_NOFS;
    if (!writable) return FAT_ERR_RO;
    return 0;
}

int fat_write_file(const char *path, const void *data, unsigned int size) {
    int r = check_writable();
    if (r) return r;

    char leaf[FAT_NAME_MAX + 1];
    unsigned int dir;
    if ((r = resolve_parent(path, &dir, leaf)) != 0) return r;
    if (!name_ok(leaf)) return FAT_ERR_NAME;
    if (invalidate_fsinfo() != 0) return FAT_ERR_IO;

    struct fat_dirent old;
    struct entry_loc oloc;
    int exists = dir_find_loc(dir, leaf, &old, &oloc);
    if (exists && (old.attr & FAT_ATTR_DIR)) return FAT_ERR_ISDIR;
    if (exists && (old.attr & FAT_ATTR_RO)) return FAT_ERR_PERM;

    // 1. write the data into freshly allocated clusters (the old copy stays intact until the very end)
    unsigned int first = 0;
    if (size > 0) {
        unsigned int cb = spc * 512;
        first = alloc_chain((size + cb - 1) / cb);
        if (!first) return FAT_ERR_FULL;
        if (write_data(first, (const unsigned char *)data, size) != 0) { free_chain(first); return FAT_ERR_IO; }
    }

    // 2. point the directory entry at it (or create the entry)
    if (exists) {
        if ((r = update_short(&oloc, first, size)) != 0) { free_chain(first); return r; }
        free_chain(old.cluster);                            // 3. only now release the old data
        return 0;
    }

    struct new_name nn;
    unsigned char ents[22][32];
    struct entry_loc nloc;
    if ((r = make_names(dir, leaf, &nn)) != 0) { free_chain(first); return r; }
    int n = build_entries(leaf, &nn, FAT_ATTR_ARCHIVE, first, size, ents);
    if ((r = dir_alloc_slots(dir, n, &nloc)) != 0) { free_chain(first); return r; }
    if ((r = write_entries(&nloc, ents, n)) != 0) { free_chain(first); return r; }
    return 0;
}

int fat_mkdir(const char *path) {
    int r = check_writable();
    if (r) return r;

    char leaf[FAT_NAME_MAX + 1];
    unsigned int dir;
    if ((r = resolve_parent(path, &dir, leaf)) != 0) return r;
    if (!name_ok(leaf)) return FAT_ERR_NAME;
    if (invalidate_fsinfo() != 0) return FAT_ERR_IO;

    struct fat_dirent tmp;
    if (dir_find(dir, leaf, &tmp)) return FAT_ERR_EXISTS;

    unsigned int c = alloc_cluster();
    if (!c) return FAT_ERR_FULL;
    if (zero_cluster(c) != 0) { free_chain(c); return FAT_ERR_IO; }

    // "." and ".." go into the first sector of the new directory
    struct new_name dot = { { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' }, 0, 0 };
    struct new_name dotdot = { { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' }, 0, 0 };
    unsigned char dots[22][32];
    unsigned char sec[512];
    memset(sec, 0, sizeof sec);
    build_entries(".", &dot, FAT_ATTR_DIR, c, 0, dots);
    memcpy(sec, dots[0], 32);
    build_entries("..", &dotdot, FAT_ATTR_DIR, dir == root_cluster ? 0 : dir, 0, dots);
    memcpy(sec + 32, dots[0], 32);
    if (blk_write(cluster_lba(c), 1, sec) != 0) { free_chain(c); return FAT_ERR_IO; }

    struct new_name nn;
    unsigned char ents[22][32];
    struct entry_loc nloc;
    if ((r = make_names(dir, leaf, &nn)) != 0) { free_chain(c); return r; }
    int n = build_entries(leaf, &nn, FAT_ATTR_DIR, c, 0, ents);
    if ((r = dir_alloc_slots(dir, n, &nloc)) != 0) { free_chain(c); return r; }
    if ((r = write_entries(&nloc, ents, n)) != 0) { free_chain(c); return r; }
    return 0;
}

int fat_remove(const char *path) {
    int r = check_writable();
    if (r) return r;

    char leaf[FAT_NAME_MAX + 1];
    unsigned int dir;
    if ((r = resolve_parent(path, &dir, leaf)) != 0) return r;
    if (invalidate_fsinfo() != 0) return FAT_ERR_IO;

    struct fat_dirent e;
    struct entry_loc loc;
    if (!dir_find_loc(dir, leaf, &e, &loc)) return FAT_ERR_NOTFOUND;
    if (e.attr & FAT_ATTR_DIR) return FAT_ERR_ISDIR;
    if (e.attr & FAT_ATTR_RO) return FAT_ERR_PERM;

    if ((r = remove_entries(&loc)) != 0) return r;          // entry first, then the data: a crash leaks space, never corrupts
    free_chain(e.cluster);
    return 0;
}

int fat_rmdir(const char *path) {
    int r = check_writable();
    if (r) return r;

    char leaf[FAT_NAME_MAX + 1];
    unsigned int dir;
    if ((r = resolve_parent(path, &dir, leaf)) != 0) return r;
    if (invalidate_fsinfo() != 0) return FAT_ERR_IO;

    struct fat_dirent e;
    struct entry_loc loc;
    if (!dir_find_loc(dir, leaf, &e, &loc)) return FAT_ERR_NOTFOUND;
    if (!(e.attr & FAT_ATTR_DIR)) return FAT_ERR_NOTDIR;
    if (e.attr & FAT_ATTR_RO) return FAT_ERR_PERM;
    unsigned int id = e.cluster ? e.cluster : root_cluster;
    if (id == cwd_cluster) return FAT_ERR_BUSY;

    struct dir_iter it;
    struct fat_dirent child;
    iter_init(&it, id);
    while (iter_next(&it, &child, 0)) {
        if (strcmp(child.name, ".") != 0 && strcmp(child.name, "..") != 0) return FAT_ERR_NOTEMPTY;
    }

    if ((r = remove_entries(&loc)) != 0) return r;
    free_chain(e.cluster);
    return 0;
}

// Is `dir` the same as, or somewhere below, `ancestor`? (Follows the ".." entries up to the root.)
static int is_inside(unsigned int dir, unsigned int ancestor) {
    struct fat_dirent t;
    unsigned int cur = dir;
    for (int i = 0; i < 128; i++) {
        if (cur == ancestor) return 1;
        if (cur == root_cluster) return 0;
        if (!dir_find(cur, "..", &t)) return 0;
        cur = t.cluster ? t.cluster : root_cluster;
    }
    return 1;      // a loop: refuse to be safe
}

int fat_rename(const char *src, const char *dst) {
    int r = check_writable();
    if (r) return r;

    char sleaf[FAT_NAME_MAX + 1], dleaf[FAT_NAME_MAX + 1];
    unsigned int sdir, ddir;
    if ((r = resolve_parent(src, &sdir, sleaf)) != 0) return r;

    struct fat_dirent se;
    struct entry_loc sloc;
    if (!dir_find_loc(sdir, sleaf, &se, &sloc)) return FAT_ERR_NOTFOUND;
    int is_dir = (se.attr & FAT_ATTR_DIR) != 0;
    if (is_dir && (se.cluster == cwd_cluster)) return FAT_ERR_BUSY;

    // Destination: an existing directory means "move into it, keep the name"; otherwise dst is the new path.
    struct fat_dirent de;
    if (walk(dst, &de, 0) == 0 && (de.attr & FAT_ATTR_DIR)) {
        ddir = de.cluster ? de.cluster : root_cluster;
        memcpy(dleaf, se.name, strlen(se.name) + 1);
    } else {
        if ((r = resolve_parent(dst, &ddir, dleaf)) != 0) return r;
    }
    if (!name_ok(dleaf)) return FAT_ERR_NAME;

    struct fat_dirent ex;
    if (dir_find(ddir, dleaf, &ex)) {
        if (sdir == ddir && strcmp(se.name, dleaf) == 0) return 0;       // renaming onto itself
        return FAT_ERR_EXISTS;
    }
    if (is_dir && is_inside(ddir, se.cluster)) return FAT_ERR_INVAL;      // a folder can't move into itself

    if (invalidate_fsinfo() != 0) return FAT_ERR_IO;

    struct new_name nn;
    unsigned char ents[22][32];
    struct entry_loc nloc;
    if ((r = make_names(ddir, dleaf, &nn)) != 0) return r;
    int n = build_entries(dleaf, &nn, se.attr, se.cluster, se.size, ents);

    // keep the original timestamps
    const unsigned char *old = load_sector(sloc.lba[sloc.n - 1]);
    if (!old) return FAT_ERR_IO;
    memcpy(ents[n - 1] + 13, old + sloc.idx[sloc.n - 1] * 32u + 13, 7);
    memcpy(ents[n - 1] + 22, old + sloc.idx[sloc.n - 1] * 32u + 22, 4);

    if ((r = dir_alloc_slots(ddir, n, &nloc)) != 0) return r;
    if ((r = write_entries(&nloc, ents, n)) != 0) return r;

    if (is_dir && ddir != sdir) {                           // the moved folder's ".." must point at its new parent
        struct fat_dirent dd;
        struct entry_loc dloc;
        if (dir_find_loc(se.cluster, "..", &dd, &dloc)) {
            unsigned int lba = dloc.lba[dloc.n - 1], off = dloc.idx[dloc.n - 1] * 32u;
            unsigned int pc = (ddir == root_cluster) ? 0 : ddir;
            unsigned char b[8];
            wr16(b, pc >> 16);                              // bytes 20..21
            if (patch_sector(lba, off + 20, b, 2) != 0) return FAT_ERR_IO;
            wr16(b, pc & 0xFFFF);                           // bytes 26..27
            if (patch_sector(lba, off + 26, b, 2) != 0) return FAT_ERR_IO;
        }
    }

    if ((r = remove_entries(&sloc)) != 0) return r;
    if (is_dir) { cwd_cluster = root_cluster; cwd_path[0] = '/'; cwd_path[1] = '\0'; }   // paths may have changed
    return 0;
}
