#include "kernel.h"
void print_banner() {
    char *banner[] = {
        "  ___ ___ ___  _  _    ___  ___ ",
        " |_ _/ _ \\ _ \\| \\| |  / _ \\/ __|",
        "  | | (_) |   /| .` | | (_) \\__ \\",
        " |___\\___/|_|_\\_|\\_|  \\___/|___/",
        "--------------------------------",
        ""
    };

    for (int i = 0; banner[i][0] != '\0'; i++) {
        char *line = banner[i];
        for (int j = 0; line[j] != '\0'; j++) {
            print_char(line[j]);
        }
        newline();
    }
}
void clear_screen() {
    volatile unsigned char *vga = (unsigned char *) VGA_ADDRESS;
    for (int i = 0; i < 4000; i += 2) {
        vga[i] = ' ';
        vga[i+1] = 0x0F;
    }
    cursor_row = 0;
    cursor_col = 0;
}
void newline() {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= 24) {
        cursor_row = 0;
        clear_screen();
    }
}
void print_char(char c) {
    unsigned char *vga = (unsigned char *) VGA_ADDRESS;
    int offset = (cursor_row * 80 + cursor_col) * 2;
    vga[offset] = c;
    vga[offset + 1] = current_color;
    cursor_col++;
    if (cursor_col >= 80) {
        cursor_col = 0;
        cursor_row++;
    }
}