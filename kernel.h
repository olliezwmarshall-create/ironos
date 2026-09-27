#ifndef KERNEL_H
#define KERNEL_H

#define VGA_ADDRESS 0xB8000

// Shared Global Variables (extern tells other files these exist elsewhere)
extern int cursor_row;
extern int cursor_col;
extern unsigned char current_color;
extern unsigned char scancode_map[];
extern unsigned char last_scancode;
extern char cmd_buffer[];
extern int buf_index;

// Hardware Port Read Function
static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// Function Prototypes
int keyboard_is_ready();
void clear_screen();
void newline();
void print_char(char c);
int strcmp(char *s1, char *s2);
void system_reboot();
void system_shutdown();
void Cli();
void kernel_main();
void print_banner();
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#endif