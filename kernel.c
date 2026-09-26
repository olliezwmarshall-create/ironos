#define VGA_ADDRESS 0xB8000

int cursor_row = 0;
int cursor_col = 0;
unsigned char current_color = 0x0F;
unsigned char scancode_map[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

// Read a byte from a hardware port (like the keyboard at 0x60)
static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
void clear_screen() {
    unsigned char *vga = (unsigned char *) VGA_ADDRESS;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        vga[i] = ' ';     
        vga[i+1] = current_color;  
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

// Simple string comparison function since we don't have standard libraries (<string.h>)
int strcmp(char *s1, char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

void kernel_main() {
    clear_screen();
    
    // Print initial CLI prompt
    char *prompt = "root> ";
    for(int i = 0; prompt[i] != '\0'; i++) {
        print_char(prompt[i]);
    }

    unsigned char last_scancode = 0;
    char cmd_buffer[80];
    int buf_index = 0;

    while(1) {
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
                    char *msg = "Commands: help, clear, hello, color 0, color 1, color 2, color 3, color 4";
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
                char *p = "root> ";
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
    }
}