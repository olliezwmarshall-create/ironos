#include "kernel.h"

int top_rendered_line = 1;
int bottom_rendered_line = 25;
char terminal_history[MAX_HISTORY_LINES][VGA_WIDTH];
int total_lines_written = 0;
int cursor_row = 1;
int cursor_col = 0;
unsigned char current_color = 0x0F;
unsigned char last_scancode = 0;
char cmd_buffer[80];
int buf_index = 0;
unsigned long uptime_ticks = 0;

unsigned char scancode_map[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

void kernel_main() {
    clear_screen();
    print_banner();
    // Print initial CLI prompt
    char *prompt = "kernel$> ";
    for(int i = 0; prompt[i] != '\0'; i++) {
        print_char(prompt[i]);
    }

    Cli(); 
}