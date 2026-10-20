#include "kernel.h"

static int batch_depth = 0;

void vga_set_cursor(int row, int col) {
    unsigned short pos = (unsigned short)(row * 80 + col);      // row 25 is off-screen, which hides the cursor
    outb(0x3D4, 0x0F); outb(0x3D5, (unsigned char)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (unsigned char)(pos >> 8));
}

void screen_batch_begin() { batch_depth++; }
void screen_batch_end() {
    if (batch_depth > 0) batch_depth--;
    if (batch_depth == 0) render_screen();
}

void print_banner() {
    char *banner[] = {
        " ___ ____  ____  _   _    ___  ____  ",
        "|_ _|  _ \\/ __ \\| \\ | |  / _ \\/ ___| ",
        " | || |_) / / D `|  \\| | | | | \\___ \\ ",
        " | ||  _ <| \\_/ /| |\\  | | |_| |___) |",
        "|___|_| \\_\\\\____/|_| \\_|  \\___/|____/ ",
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
    // 1. Clear the physical VGA screen buffer
    volatile unsigned char *vga = VGA_MEM;
    for (int i = 0; i < 4000; i += 2) {
        vga[i] = ' ';
        vga[i+1] = current_color;
    }
    
    // 2. Reset cursor position
    cursor_row = 1;
    cursor_col = 0;
    
    // 3. Reset viewport and history tracking
    top_rendered_line = 1;
    bottom_rendered_line = 25;
    total_lines_written = 0;
    
    // 4. Clear out the history buffer text
    for (int i = 0; i < MAX_HISTORY_LINES; i++) {
        for (int j = 0; j < VGA_WIDTH; j++) {
            terminal_history[i][j] = '\0';
        }
    }
}
// Retain the latest 1000 lines without writing beyond the history buffer.
static void advance_line(void) {
    cursor_col = 0;
    if (cursor_row + 1 >= MAX_HISTORY_LINES) {
        memmove(terminal_history, terminal_history + 1, (MAX_HISTORY_LINES - 1) * VGA_WIDTH);
        memset(terminal_history[MAX_HISTORY_LINES - 1], 0, VGA_WIDTH);
        if (top_rendered_line > 0) top_rendered_line--;
        if (bottom_rendered_line > 0) bottom_rendered_line--;
        if (total_lines_written > 0) total_lines_written--;
    } else cursor_row++;
    if (cursor_row > total_lines_written) total_lines_written = cursor_row;
    if (cursor_row > bottom_rendered_line) {
        bottom_rendered_line = cursor_row;
        top_rendered_line = bottom_rendered_line - 24;
    }
}
void print_char(char c) {
    if (cursor_row >= 0 && cursor_row < MAX_HISTORY_LINES)
        terminal_history[cursor_row][cursor_col] = c;
    if (++cursor_col >= VGA_WIDTH) advance_line();
    if (cursor_row > total_lines_written) total_lines_written = cursor_row;
    if (cursor_row > bottom_rendered_line) {
        bottom_rendered_line = cursor_row;
        top_rendered_line = bottom_rendered_line - 24;
    }
    if (!batch_depth) render_screen();
}
void newline(void) {
    advance_line();
    if (!batch_depth) render_screen();
}
void render_screen() {
    unsigned char *vga = VGA_MEM;
    
    // Loop through the 25 rows of the physical screen
    for (int row = 0; row < 25; row++) {
        // Find which line in our history buffer this screen row corresponds to
        int history_row = top_rendered_line + row;
        
        for (int col = 0; col < 80; col++) {
            int vga_offset = (row * 80 + col) * 2;
            
            // If the history line exists, print its characters; otherwise print blank spaces
            if (history_row >= 0 && history_row < MAX_HISTORY_LINES && history_row <= total_lines_written) {
                char c = terminal_history[history_row][col];
                // If it's a null terminator, just print a space
                vga[vga_offset] = (c != '\0') ? c : ' ';
            } else {
                vga[vga_offset] = ' ';
            }
            
            // Set the text color
            vga[vga_offset + 1] = current_color;
        }
    }
}
void print_string(const char *s) {
    screen_batch_begin();
    for (int i = 0; s[i] != '\0'; i++) print_char(s[i]);
    screen_batch_end();

    // Show the blinking cursor where the next character will go (hidden if scrolled out of view)
    int crow = cursor_row - top_rendered_line;
    if (crow >= 0 && crow < 25) vga_set_cursor(crow, cursor_col);
    else vga_set_cursor(25, 0);
}
