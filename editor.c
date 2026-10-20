#include "kernel.h"

// Full-screen text editor. Takes over the whole screen (row 0 = title bar, rows 1-23 = text, row 24 = status).
// Ctrl+S saves, Ctrl+Q quits. Files with Windows (CRLF) line endings are converted in memory and saved back as CRLF.

#define ED_CAP    (512 * 1024)
#define TEXT_ROWS 23
#define COL_TEXT  0x1F          // white on blue
#define COL_BAR   0x70          // black on light grey

static char *buf;
static unsigned int len, pos, top_pos, left_col, want_col;
static int modified, crlf;
static char message[48];
static const char *file_path;

static void set_msg(const char *m) {
    unsigned int i = 0;
    for (; m[i] && i < sizeof message - 1; i++) message[i] = m[i];
    message[i] = '\0';
}

static unsigned int line_start(unsigned int p) { while (p > 0 && buf[p - 1] != '\n') p--; return p; }
static unsigned int line_end(unsigned int p)   { while (p < len && buf[p] != '\n') p++; return p; }

// Screen column of position p on the line starting at ls (tabs expand to multiples of 4).
static unsigned int disp_col(unsigned int ls, unsigned int p) {
    unsigned int c = 0;
    for (unsigned int i = ls; i < p; i++) c = (buf[i] == '\t') ? (c / 4 + 1) * 4 : c + 1;
    return c;
}

static void cell(int row, int col, char ch, unsigned char attr) {
    VGA_MEM[(row * 80 + col) * 2] = (unsigned char)ch;
    VGA_MEM[(row * 80 + col) * 2 + 1] = attr;
}

static void text_at(int row, int col, const char *s, unsigned char attr) {
    for (; *s && col < 80; s++, col++) cell(row, col, *s, attr);
}

static void fill_row(int row, unsigned char attr) {
    for (int c = 0; c < 80; c++) cell(row, c, ' ', attr);
}

// Keep the cursor inside the visible window.
static void fix_view(void) {
    unsigned int ls = line_start(pos);
    if (pos < top_pos) {
        top_pos = ls;
    } else {
        unsigned int rows = 0;
        for (unsigned int i = top_pos; i < pos; i++) if (buf[i] == '\n') rows++;
        while (rows >= TEXT_ROWS) { top_pos = line_end(top_pos) + 1; rows--; }
    }
    unsigned int dc = disp_col(ls, pos);
    if (dc < left_col) left_col = dc;
    else if (dc >= left_col + 80) left_col = dc - 79;
}

static void draw(void) {
    // title bar
    fill_row(0, COL_BAR);
    text_at(0, 1, "IronOS Edit - ", COL_BAR);
    text_at(0, 15, file_path, COL_BAR);
    if (modified) text_at(0, 70, "[modified]", COL_BAR);

    // text area
    unsigned int p = top_pos;
    int past_end = 0;
    for (int row = 1; row <= TEXT_ROWS; row++) {
        fill_row(row, COL_TEXT);
        if (past_end) continue;
        unsigned int q = p, col = 0;
        while (q < len && buf[q] != '\n') {
            unsigned char ch = (unsigned char)buf[q];
            if (ch == '\t') {
                unsigned int next = (col / 4 + 1) * 4;
                for (; col < next; col++)
                    if (col >= left_col && col < left_col + 80) cell(row, (int)(col - left_col), ' ', COL_TEXT);
            } else {
                if (ch < 32 || ch == 127) ch = '.';
                if (col >= left_col && col < left_col + 80) cell(row, (int)(col - left_col), (char)ch, COL_TEXT);
                col++;
            }
            q++;
        }
        if (q >= len) past_end = 1;          // this was the last line
        else p = q + 1;
    }

    // status bar: position + message (or the key help)
    unsigned int line = 1;
    for (unsigned int i = 0; i < pos; i++) if (buf[i] == '\n') line++;
    unsigned int col = disp_col(line_start(pos), pos) + 1;
    fill_row(24, COL_BAR);
    char num[16], status[80];
    unsigned int n = 0;
    const char *a = "Ln ";
    for (; *a; a++) status[n++] = *a;
    int_to_string(line, num); for (char *c = num; *c; c++) status[n++] = *c;
    a = ", Col "; for (; *a; a++) status[n++] = *a;
    int_to_string(col, num); for (char *c = num; *c; c++) status[n++] = *c;
    status[n] = '\0';
    text_at(24, 1, "^S Save  ^Q Quit", COL_BAR);
    text_at(24, 20, status, COL_BAR);
    if (message[0]) text_at(24, 36, message, COL_BAR);

    // hardware cursor
    unsigned int ls = line_start(pos), rows = 0;
    for (unsigned int i = top_pos; i < ls; i++) if (buf[i] == '\n') rows++;
    vga_set_cursor(1 + (int)rows, (int)(disp_col(ls, pos) - left_col));
}

static int insert_char(char c) {
    if (len + 1 >= ED_CAP) { set_msg("Buffer full (512 KB limit)"); return 0; }
    memmove(buf + pos + 1, buf + pos, len - pos);
    buf[pos++] = c;
    len++;
    modified = 1;
    return 1;
}

static void delete_at(unsigned int p) {
    memmove(buf + p, buf + p + 1, len - p - 1);
    len--;
    modified = 1;
}

static void remember_col(void) { want_col = pos - line_start(pos); }

static void move_up(void) {
    unsigned int ls = line_start(pos);
    if (ls == 0) return;
    unsigned int pls = line_start(ls - 1);
    unsigned int plen = (ls - 1) - pls;
    pos = pls + (want_col < plen ? want_col : plen);
}

static void move_down(void) {
    unsigned int le = line_end(pos);
    if (le >= len) return;
    unsigned int nls = le + 1, nle = line_end(nls);
    unsigned int nlen = nle - nls;
    pos = nls + (want_col < nlen ? want_col : nlen);
}

static int save(void) {
    if (!fat_writable()) { set_msg("READ-ONLY: use 'mount N rw' first"); return -1; }
    int r;
    if (!crlf) {
        r = fat_write_file(file_path, buf, len);
    } else {
        unsigned int nls = 0;
        for (unsigned int i = 0; i < len; i++) if (buf[i] == '\n') nls++;
        char *out = kmalloc(len + nls + 1);
        if (!out) { set_msg("Out of memory"); return -1; }
        unsigned int j = 0;
        for (unsigned int i = 0; i < len; i++) { if (buf[i] == '\n') out[j++] = '\r'; out[j++] = buf[i]; }
        r = fat_write_file(file_path, out, j);
        kfree(out);
    }
    if (r != 0) { set_msg(fat_strerror(r)); return -1; }
    modified = 0;
    set_msg("Saved");
    return 0;
}

int editor_run(const char *path) {
    file_path = path;
    buf = kmalloc(ED_CAP);
    if (!buf) return FAT_ERR_FULL;
    len = pos = top_pos = left_col = want_col = 0;
    modified = crlf = 0;
    message[0] = '\0';

    struct fat_dirent e;
    if (fat_lookup(path, &e) == 0) {
        if (e.attr & FAT_ATTR_DIR) { kfree(buf); return FAT_ERR_ISDIR; }
        if (e.size >= ED_CAP) { kfree(buf); return FAT_ERR_INVAL; }
        int n = e.size ? fat_read_file(&e, buf, e.size) : 0;
        if (n < 0) { kfree(buf); return FAT_ERR_IO; }
        len = (unsigned int)n;
        for (unsigned int i = 0; i < len; i++) if (buf[i] == '\0') { kfree(buf); return FAT_ERR_INVAL; }   // binary file

        unsigned int crs = 0, nls = 0;
        for (unsigned int i = 0; i < len; i++)
            if (buf[i] == '\n') { nls++; if (i > 0 && buf[i - 1] == '\r') crs++; }
        if (crs > 0 && crs * 2 >= nls) {                  // Windows line endings: edit as LF, save back as CRLF
            crlf = 1;
            unsigned int j = 0;
            for (unsigned int i = 0; i < len; i++) {
                if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n') continue;
                buf[j++] = buf[i];
            }
            len = j;
        }
    } else {
        set_msg("New file");
    }
    if (!fat_writable()) set_msg("READ-ONLY volume ('mount N rw' to save)");

    int quit_armed = 0;
    for (;;) {
        fix_view();
        draw();

        int k;
        while ((k = keyboard_getchar()) < 0) { net_poll(); halt(); }
        int was_armed = quit_armed;
        set_msg("");                 // a message stays until the next key press

        if (k == KEY_CTRL('q') || k == KEY_CTRL('x')) {
            if (modified && !was_armed) {
                set_msg("Unsaved! Ctrl+Q again to discard");
                quit_armed = 1;
                continue;
            }
            break;
        }
        quit_armed = 0;

        if (k == KEY_CTRL('s')) save();
        else if (k == KEY_LEFT)  { if (pos > 0) pos--; remember_col(); }
        else if (k == KEY_RIGHT) { if (pos < len) pos++; remember_col(); }
        else if (k == KEY_UP)    move_up();
        else if (k == KEY_DOWN)  move_down();
        else if (k == KEY_HOME)  { pos = line_start(pos); remember_col(); }
        else if (k == KEY_END)   { pos = line_end(pos); remember_col(); }
        else if (k == KEY_PGUP)  { for (int i = 0; i < TEXT_ROWS - 1; i++) move_up(); }
        else if (k == KEY_PGDN)  { for (int i = 0; i < TEXT_ROWS - 1; i++) move_down(); }
        else if (k == KEY_DELETE) { if (pos < len) delete_at(pos); remember_col(); }
        else if (k == '\b')      { if (pos > 0) { pos--; delete_at(pos); } remember_col(); }
        else if (k == '\n')      { if (insert_char('\n')) remember_col(); }
        else if (k == '\t')      { for (int i = 0; i < 4; i++) insert_char(' '); remember_col(); }
        else if (k >= ' ' && k < 0x7F) { if (insert_char((char)k)) remember_col(); }
    }

    kfree(buf);
    render_screen();            // give the terminal back
    return 0;
}
