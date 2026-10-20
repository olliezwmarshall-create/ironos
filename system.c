#include "kernel.h"

void system_reboot() {
    unsigned char temp = 0x02;
    while (temp & 0x02) {
        temp = inb(0x64);
    }
    outb(0x64, 0xFE);
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

void int_to_string(unsigned long n, char *buf) {
    char tmp[12];
    int i = 0, j = 0;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

unsigned int strlen(const char *s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

void *memset(void *dst, int value, unsigned int n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

void hex_to_string(unsigned int v, char *buf) {
    const char *digits = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) buf[i] = digits[(v >> (28 - 4 * i)) & 0xF];
    buf[8] = '\0';
}

// ---- CMOS real-time clock ----
static unsigned char cmos_read(unsigned char reg) {
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}

static unsigned int bcd(unsigned char v) { return (v & 0x0F) + (v >> 4) * 10; }

int rtc_get(struct rtc_time *t) {
    unsigned char s, m, h, d, mo, y, regb;
    for (int tries = 0; tries < 3; tries++) {
        for (int guard = 0; guard < 100000 && (cmos_read(0x0A) & 0x80); guard++) { }   // wait out an update in progress
        s = cmos_read(0x00); m = cmos_read(0x02); h = cmos_read(0x04);
        d = cmos_read(0x07); mo = cmos_read(0x08); y = cmos_read(0x09);
        regb = cmos_read(0x0B);
        if (s == cmos_read(0x00)) break;                         // stable read
    }
    int pm = (h & 0x80) != 0;
    if (!(regb & 0x04)) {                                        // BCD mode (the usual case)
        s = (unsigned char)bcd(s); m = (unsigned char)bcd(m); h = (unsigned char)bcd(h & 0x7F);
        d = (unsigned char)bcd(d); mo = (unsigned char)bcd(mo); y = (unsigned char)bcd(y);
    } else {
        h &= 0x7F;
    }
    if (!(regb & 0x02)) {                                        // 12-hour mode
        h = (unsigned char)(h % 12);
        if (pm) h += 12;
    }
    t->second = s; t->minute = m; t->hour = h; t->day = d; t->month = mo;
    t->year = y < 70 ? 2000u + y : 1900u + y;
    if (s > 59 || m > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return -1;
    return 0;
}
