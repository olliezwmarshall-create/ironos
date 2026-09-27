#include "kernel.h"
void system_reboot() {
    unsigned char temp = 0x02;
    while (temp & 0x02) {
        temp = inb(0x64);
    }
    outb(0x64, 0xFE);
}
int keyboard_is_ready() {
    return inb(0x64) & 1;
}
int strcmp(char *s1, char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
void system_shutdown() {
    // Send shutdown signal using outw (16-bit word) instead of outb
    outw(0xB004, 0x2000);

    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}