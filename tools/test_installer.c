/* Host fault-injection tests. gcc -std=gnu99 -fno-builtin -DHOST_TEST
 * tools/test_installer.c -o /tmp/test-installer && /tmp/test-installer */
#include <stdio.h>
#include <stdlib.h>
#include "../install.c"

static unsigned char media[4096], payload[1024];
static int writes, unmounts, fail_write, corrupt_read, wrote_mbr;
static struct disk test_disk = { .usable = 1, .name = "ide0", .model = "test", .sectors = 8 };
static char output[4096];
static unsigned int output_size;
void *memcpy(void *d, const void *s, unsigned int n) {
    unsigned char *a = d; const unsigned char *b = s;
    for (unsigned int i = 0; i < n; i++) a[i] = b[i];
    return d;
}
void *memset(void *d, int c, unsigned int n) {
    unsigned char *a = d; for (unsigned int i = 0; i < n; i++) a[i] = c; return d;
}
int memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *p = a, *q = b;
    for (unsigned int i = 0; i < n; i++) if (p[i] != q[i]) return p[i] - q[i];
    return 0;
}
unsigned int strlen(const char *s) { unsigned int n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a - *b; }
void print_string(const char *s) {
    unsigned int n = strlen(s);
    if (output_size + n + 1 >= sizeof(output)) abort();
    memcpy(output + output_size, s, n + 1); output_size += n;
}
void newline(void) { print_string("\n"); }
void int_to_string(unsigned long n, char *buf) { sprintf(buf, "%lu", n); }
int disk_count(void) { return 1; }
const struct disk *disk_get(int n) { return n == 0 ? &test_disk : 0; }
void fat_unmount(void) { unmounts++; }
int disk_write(int d, unsigned int lba, unsigned int n, const void *p) {
    if (d != 0 || lba + n > 8) abort();
    writes++;
    if (fail_write == writes) return -1;
    if (!lba) wrote_mbr = 1;
    memcpy(media + lba * 512, p, n * 512); return 0;
}
int disk_read(int d, unsigned int lba, unsigned int n, void *p) {
    if (d != 0 || lba + n > 8) abort();
    memcpy(p, media + lba * 512, n * 512);
    if (corrupt_read) ((unsigned char *)p)[0] ^= 1;
    return 0;
}
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n%s", what, output); exit(1); }
}
static void reset(void) {
    writes = unmounts = fail_write = corrupt_read = wrote_mbr = 0;
    pending = -1; output_size = 0; output[0] = 0;
    memset(media, 0xA5, sizeof(media));
    for (unsigned int i = 0; i < sizeof(payload); i++) payload[i] = i;
    image = payload; image_bytes = sizeof(payload); image_crc = crc32(image, image_bytes);
}
static void arm(void) { check(!installer_command("ide0"), "preview does not alter disk"); }
int main(void) {
    reset();
    check(!installer_command("ide0 ERASE") && !writes && !unmounts, "cannot bypass preview");
    arm(); installer_command("ide0 erase");
    check(!writes && !unmounts, "exact confirmation required");
    check(!installer_command("ide0 ERASE") && !writes, "bad confirmation consumes preview");
    arm(); image_crc ^= 1;
    check(!installer_command("ide0 ERASE") && !writes && !unmounts, "bad CRC leaves disk and mount untouched");
    reset(); arm(); fail_write = 1;
    check(installer_command("ide0 ERASE") && unmounts == 1 && !wrote_mbr, "write failure stops before MBR");
    reset(); arm(); corrupt_read = 1;
    check(installer_command("ide0 ERASE") && writes == 1 && !wrote_mbr, "readback failure stops before MBR");
    reset(); arm(); fail_write = 2;
    check(installer_command("ide0 ERASE") && !wrote_mbr, "backup GPT clear failure stops before MBR");
    reset(); arm();
    check(installer_command("ide0 ERASE") && writes == 3 && wrote_mbr, "success commits MBR last");
    check(!memcmp(media, payload, sizeof(payload)), "written image matches payload");
    for (int i = 3584; i < 4096; i++) check(media[i] == 0, "backup GPT header cleared");
    reset(); image = 0;
    check(!installer_command("ide0 ERASE") && !writes, "installed mode refuses installer");
    puts("PASS: installer confirmation, CRC, I/O failure, readback, MBR commit and installed-mode guards");
    return 0;
}
