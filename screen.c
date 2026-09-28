#include "kernel.h"

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
    volatile unsigned char *vga = (unsigned char *) VGA_ADDRESS;
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
void print_char(char c) {
    // 1. Save character to history buffer (with safety boundary check)
    if (cursor_row < MAX_HISTORY_LINES) {
        terminal_history[cursor_row][cursor_col] = c;
    }

    // 2. Advance column and handle line wrapping
    cursor_col++;
    if (cursor_col >= 80) {
        cursor_col = 0;
        // Only increment row if we haven't hit the absolute history ceiling
        if (cursor_row < MAX_HISTORY_LINES - 1) {
            cursor_row++;
        }
    }

    // 3. Track total lines written (capped at history limit)
    if (cursor_row > total_lines_written && cursor_row < MAX_HISTORY_LINES) {
        total_lines_written = cursor_row;
    }

    // 4. Auto-follow: if we are at the bottom, push the viewport down
    if (cursor_row > bottom_rendered_line) {
        top_rendered_line++;
        bottom_rendered_line++;
    }

    // 5. Redraw the screen so changes show up instantly
    render_screen();
}

void newline() {
    cursor_col = 0;
    cursor_row++;
    
    if (cursor_row > total_lines_written) {
        total_lines_written = cursor_row;
    }

    if (cursor_row > bottom_rendered_line) {
        top_rendered_line++;
        bottom_rendered_line++;
    }

    render_screen();
}
void render_screen() {
    unsigned char *vga = (unsigned char *) 0xB8000;
    
    // Loop through the 25 rows of the physical screen
    for (int row = 0; row < 25; row++) {
        // Find which line in our history buffer this screen row corresponds to
        int history_row = top_rendered_line + row;
        
        for (int col = 0; col < 80; col++) {
            int vga_offset = (row * 80 + col) * 2;
            
            // If the history line exists, print its characters; otherwise print blank spaces
            if (history_row <= total_lines_written) {
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