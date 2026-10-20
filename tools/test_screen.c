/* Host regression for bounded terminal history; run make test-screen. */
#include <stdio.h>
#include <stdlib.h>
#include "../screen.c"
unsigned char host_vga[4000];
char terminal_history[MAX_HISTORY_LINES][VGA_WIDTH];
int cursor_row, cursor_col, top_rendered_line, bottom_rendered_line, total_lines_written;
unsigned char current_color = 15;
void outb(unsigned short p, unsigned char v) { (void)p; (void)v; }
void *memset(void *dst, int v, unsigned int n) {
    unsigned char *d = dst; for (unsigned int i = 0; i < n; i++) d[i] = v; return dst;
}
void *memmove(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (d < s) for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    else while (n) { n--; d[n] = s[n]; }
    return dst;
}
static void check(int ok) { if (!ok) { fputs("Screen history bounds failed\n", stderr); exit(1); } }
int main(void) {
    clear_screen();
    screen_batch_begin();
    for (int i = 0; i < 1400; i++) {
        print_string("A line that must remain bounded."); newline();
        check(cursor_row < MAX_HISTORY_LINES);
        check(total_lines_written < MAX_HISTORY_LINES);
    }
    // Exercise automatic wrapping while the history buffer is already full.
    for (int i = 0; i < 200; i++) print_char('x');
    newline(); print_string("latest");
    screen_batch_end();
    check(cursor_col == 6 && cursor_row == MAX_HISTORY_LINES - 1);
    check(terminal_history[cursor_row][0] == 'l');
    check(host_vga[24 * 160] == 'l');
    check(bottom_rendered_line - top_rendered_line == 24);
    puts("PASS: terminal scrollback overflow, wrapped lines, latest output and VGA bounds");
    return 0;
}
