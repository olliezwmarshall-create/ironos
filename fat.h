#ifndef FAT_H
#define FAT_H

#define FAT_NAME_MAX  255

#define FAT_ATTR_RO      0x01
#define FAT_ATTR_HIDDEN  0x02
#define FAT_ATTR_SYSTEM  0x04
#define FAT_ATTR_DIR     0x10
#define FAT_ATTR_ARCHIVE 0x20

// Error codes (negative) returned by the functions below
#define FAT_OK            0
#define FAT_ERR_NOTFOUND -1     // no such file or directory
#define FAT_ERR_NOTDIR   -2     // a path component is not a directory
#define FAT_ERR_ISDIR    -3     // is a directory
#define FAT_ERR_EXISTS   -4     // name already in use
#define FAT_ERR_FULL     -5     // no free clusters left
#define FAT_ERR_DIRFULL  -6     // the (fixed-size) root directory is full
#define FAT_ERR_RO       -7     // volume is mounted read-only
#define FAT_ERR_IO       -8     // disk read/write error
#define FAT_ERR_NAME     -9     // invalid file name
#define FAT_ERR_NOTEMPTY -10    // directory is not empty
#define FAT_ERR_BUSY     -11    // e.g. removing the current directory
#define FAT_ERR_INVAL    -12    // invalid operation
#define FAT_ERR_PERM     -13    // file has the read-only attribute
#define FAT_ERR_NOFS     -14    // no filesystem mounted

struct fat_dirent {
    char name[FAT_NAME_MAX + 1];   // long name if the file has one, otherwise the short name
    char sname[13];                // 8.3 short name ("HELLO.TXT")
    unsigned char attr;
    unsigned int cluster;
    unsigned int size;
};

struct fat_info {
    unsigned int bits;             // 12, 16 or 32
    unsigned int start_lba;        // where the volume begins on the disk
    unsigned int cluster_bytes;
    unsigned int total_clusters;
    int drive;
    int writable;
};

typedef void (*fat_list_cb)(const struct fat_dirent *entry, void *ctx);

int fat_is_bpb(const unsigned char *sector);           // does this 512-byte sector look like a FAT boot sector?
int fat_mount(int drive, unsigned int start_lba, unsigned int vol_sectors, int want_write);   // 0 on success
int fat_mounted(void);
int fat_writable(void);
const char *fat_status(void);                          // human-readable result of the last mount
const char *fat_strerror(int err);
void fat_get_info(struct fat_info *info);
const char *fat_getcwd(void);

// Reading
int fat_lookup(const char *path, struct fat_dirent *out);           // 0 = found
int fat_chdir(const char *path);                                    // 0 ok, -1 not found, -2 not a directory
int fat_list_dir(const struct fat_dirent *dir, fat_list_cb cb, void *ctx);
int fat_read_file(const struct fat_dirent *file, void *buf, unsigned int max);   // bytes read, or -1
int fat_free_clusters(unsigned int *free_count);                    // scans the FAT; 0 ok

// Writing (all need a volume mounted read-write; not supported on FAT12)
int fat_write_file(const char *path, const void *data, unsigned int size);   // create or replace a file
int fat_mkdir(const char *path);
int fat_remove(const char *path);                                   // delete a file
int fat_rmdir(const char *path);                                    // delete an empty directory
int fat_rename(const char *src, const char *dst);                   // rename or move (files and directories)

#endif
