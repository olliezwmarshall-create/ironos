#include "kernel.h"
void Cli(){

while(1) {
        if (!keyboard_is_ready()) {
            uptime_ticks++;
            continue;
        }
        
        unsigned char scancode = inb(0x60);
        
        if (scancode > 0 && scancode != last_scancode && !(scancode & 0x80)) {
            char letter = scancode_map[scancode];
            
            if (letter == '\n') {
                // User pressed Enter! Let's process the command.
                cmd_buffer[buf_index] = '\0'; // End the string
                
                // Print a newline for the response
                newline();
                
                // Check what command they typed
                if (strcmp(cmd_buffer, "help") == 0) {
                    char *msg = "Commands: help, clear, hello, reboot, color 0-4, banner";
                    newline();
                    for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                    newline();
                } 
                else if (strcmp(cmd_buffer, "clear") == 0) {
                    clear_screen();
                } 
                else if (strcmp(cmd_buffer, "hello") == 0) {
                    if(cursor_row !=0){
                        char *msg = "Hello from your custom kernel!";
                        newline();
                        for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                        newline();
                    }
                } 
                else if (strcmp(cmd_buffer, "color 0") == 0) {
                    current_color = 0x0F; // White
                    char *msg = "Color: White";
                    newline();
                    for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                    newline();
                }
                else if (strcmp(cmd_buffer, "color 1") == 0) {
                    current_color = 0x01; // Blue
                    char *msg = "Color: Blue";
                    newline();
                    for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                    newline();
                }
                else if (strcmp(cmd_buffer, "color 2") == 0) {
                    current_color = 0x02; // Green
                    char *msg = "Color: Green";
                    newline();
                    for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                    newline();
                }
                else if (strcmp(cmd_buffer, "color 3") == 0) {
                    current_color = 0x03; // Cyan
                    char *msg = "Color: Cyan";
                    newline();
                    for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                    newline();
                }
                else if (strcmp(cmd_buffer, "color 4") == 0) {
                    current_color = 0x04; // Red
                    char *msg = "Color: Red";
                    newline();
                    for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                    newline();
                }
                else if (strcmp(cmd_buffer, "reboot") == 0) {
                    char *msg = "Rebooting system...";
                    for(int i = 0; msg[i] != '\0'; i++) {
                        print_char(msg[i]);
                    }
                    newline();
                    system_reboot();
                }
                else if (strcmp(cmd_buffer, "banner") == 0) {
                    print_banner();
                }
                else if (buf_index == 0) {
                    // If the user just pressed Enter without typing anything, do nothing
                }
                else if (buf_index > 0) {
                    newline();
                    if(cursor_row !=0){
                        char *msg = "Unknown command. Type 'help'.";
                        for(int i = 0; msg[i] != '\0'; i++) print_char(msg[i]);
                        newline();
                    }
                }
                
                // Reset buffer and print new prompt
                buf_index = 0;
                char *p = "kernel$> ";
                for(int i = 0; p[i] != '\0'; i++) {
                    print_char(p[i]);
                }
            }
            else if (letter == '\b') {
                if (buf_index > 0) {
                    buf_index--; // Remove from buffer
                    if (cursor_col > 0) {
                        cursor_col--;
                        print_char(' ');
                        cursor_col--;
                    }
                }
            } 
            else if (letter != 0 && buf_index < 79) {
                // Add letter to buffer and print it on screen
                cmd_buffer[buf_index++] = letter;
                print_char(letter);
            }
            
            last_scancode = scancode;
        }
        
        if (scancode & 0x80) {
            last_scancode = 0;
        }
        if (scancode == 0x48) { // Up Arrow
            if (top_rendered_line > 0) {
                top_rendered_line--;
                bottom_rendered_line--;
                render_screen();
            }
        } 
        else if (scancode == 0x50) { // Down Arrow
            if (bottom_rendered_line < total_lines_written) {
                top_rendered_line++;
                bottom_rendered_line++;
                render_screen();
            }
        }
    }
}