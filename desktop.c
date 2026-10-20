#include "kernel.h"

/* Small keyboard-driven desktop in the existing 80x25 VGA text mode. */
#define MAX_ENTRIES 128
#define PREVIEW_SIZE 8192
static struct fat_dirent entries[MAX_ENTRIES];
static unsigned char preview[PREVIEW_SIZE];
static int count, selected, skipped, view, previewing, preview_len, preview_page;
static const char *notice;

static void fill(int x, int y, int w, int h, unsigned char color) {
    volatile unsigned char *vga = VGA_MEM;
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++) {
            unsigned int at = (row * 80 + col) * 2;
            vga[at] = ' '; vga[at + 1] = color;
        }
}
static void text(int x, int y, const char *s, int max, unsigned char color) {
    volatile unsigned char *vga = VGA_MEM;
    for (int i = 0; i < max && s[i]; i++) {
        unsigned int at = (y * 80 + x + i) * 2;
        unsigned char c = s[i];
        vga[at] = c >= 32 && c < 127 ? c : '.';
        vga[at + 1] = color;
    }
}
static void number(int x, int y, const char *label, unsigned int n, const char *unit) {
    char buf[16];
    text(x, y, label, 38, 0x1F);
    int_to_string(n, buf);
    x += strlen(label);
    text(x, y, buf, 12, 0x1F);
    text(x + strlen(buf), y, unit, 10, 0x1F);
}
static void collect(const struct fat_dirent *e, void *ctx) {
    (void)ctx;
    if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) return;
    if (count == MAX_ENTRIES) { skipped = 1; return; }
    entries[count++] = *e;
}
static void list(void) {
    count = selected = skipped = previewing = 0;
    notice = 0;
    if (!fat_mounted()) { notice = "No filesystem. Return to terminal and use mount."; return; }
    struct fat_dirent dir;
    int r = fat_lookup(".", &dir);
    if (!r) r = fat_list_dir(&dir, collect, 0);
    if (r) notice = fat_strerror(r);
    else if (skipped) notice = "Showing first 128 entries.";
}
static void file_open(void) {
    if (!count) return;
    struct fat_dirent *e = &entries[selected];
    if (e->attr & FAT_ATTR_DIR) {
        int r = fat_chdir(e->name);
        if (r) notice = fat_strerror(r); else list();
    } else {
        preview_len = fat_read_file(e, preview, sizeof(preview));
        if (preview_len < 0) { notice = "Unable to read this file."; return; }
        previewing = 1; preview_page = 0;
        notice = e->size > PREVIEW_SIZE ? "Preview limited to first 8 KiB. Esc: files" : "Esc: files | PgUp/PgDn: scroll";
    }
}
static void preview_draw(void) {
    int row = 0, col = 0;
    int first = preview_page * 16;
    for (int i = 0; i < preview_len && row < first + 16; i++) {
        unsigned char c = preview[i];
        if (c == '\r') continue;
        if (c == '\n') { row++; col = 0; continue; }
        if (row >= first) {
            char s[2] = { c == '\t' ? ' ' : (c >= 32 && c < 127 ? c : '.'), 0 };
            text(24 + col, 5 + row - first, s, 1, 0x1F);
        }
        if (++col == 53) { col = 0; row++; }
    }
}
static int last_preview_page(void) {
    int rows = 0, col = 0;
    for (int i = 0; i < preview_len; i++) {
        if (preview[i] == '\r') continue;
        if (preview[i] == '\n') { rows++; col = 0; }
        else if (++col == 53) { rows++; col = 0; }
    }
    return rows / 16;
}
static void draw(void) {
    fill(0, 0, 80, 25, 0x17);
    fill(0, 0, 80, 1, 0x30);
    text(2, 0, "IronOS Desktop", 24, 0x3F);
    char num[16]; int_to_string(uptime_ticks / TIMER_HZ, num);
    text(54, 0, "Uptime", 7, 0x3F); text(62, 0, num, 12, 0x3F);
    text(2, 2, "APPLICATIONS", 18, 0x1B);
    const char *apps[] = { "[H] Home", "[F] Files", "[S] System", "[T] Terminal" };
    for (int i = 0; i < 4; i++) {
        unsigned char color = i == view ? 0x30 : 0x1F;
        fill(1, 4 + i * 2, 20, 1, color);
        text(2, 4 + i * 2, apps[i], 18, color);
    }
    fill(22, 2, 57, 21, 0x1F);
    text(24, 3, view == 1 ? (previewing ? entries[selected].name : fat_getcwd()) :
         (view == 2 ? "System overview" : "Welcome to IronOS"), 53, 0x1E);
    if (view == 0) {
        text(24, 6, "A small desktop for your own operating system.", 53, 0x1F);
        text(24, 9, "F  Browse directories and preview files", 53, 0x1F);
        text(24, 11, "S  See memory, storage and uptime", 53, 0x1F);
        text(24, 13, "T  Return to the command terminal", 53, 0x1F);
        text(24, 17, "Keyboard controlled | VGA text mode", 53, 0x1B);
    } else if (view == 2) {
        number(24, 6, "Memory total: ", pmm_total_pages() / 256, " MiB");
        number(24, 8, "Memory free:  ", pmm_free_page_count() / 256, " MiB");
        int disks = 0;
        for (int i = 0; i < disk_count(); i++) if (disk_get(i)->usable) disks++;
        number(24, 10, "Storage disks: ", disks, "");
        number(24, 12, "FAT volumes: ", volume_count(), "");
        text(24, 15, fat_mounted() ? (fat_writable() ? "Filesystem: read-write" : "Filesystem: read-only") : "Filesystem: not mounted", 53, 0x1F);
        text(24, 18, "32-bit x86 | BIOS boot | cooperative apps", 53, 0x1B);
    } else if (previewing) preview_draw();
    else {
        int first = selected / 16 * 16;
        for (int i = first; i < count && i < first + 16; i++) {
            unsigned char color = i == selected ? 0x70 : 0x1F;
            fill(24, 5 + i - first, 53, 1, color);
            text(24, 5 + i - first, entries[i].attr & FAT_ATTR_DIR ? "[DIR] " : "      ", 6, color);
            text(30, 5 + i - first, entries[i].name, 47, color);
        }
        if (!count) text(24, 6, notice ? notice : "This directory is empty.", 53, 0x1F);
    }
    if (notice && view == 1) text(24, 22, notice, 53, 0x1E);
    fill(0, 24, 80, 1, 0x30);
    text(1, 24, view == 1 ? "H Home | S System | T Terminal | Arrows/Enter Open | Backspace Up | Esc Back" :
         "H Home | F Files | S System | T Terminal | Tab Next | Esc Terminal", 78, 0x3F);
    vga_set_cursor(25, 0);
}
void desktop_run(void) {
    char saved_dir[256];
    int restore = fat_mounted();
    if (restore) memcpy(saved_dir, fat_getcwd(), strlen(fat_getcwd()) + 1);
    view = previewing = 0; notice = 0;
    draw();
    unsigned long last = uptime_ticks / TIMER_HZ;
    for (;;) {
        int key = keyboard_getchar();
        if (key < 0) {
            if (uptime_ticks / TIMER_HZ != last) { last = uptime_ticks / TIMER_HZ; draw(); }
            halt(); continue;
        }
        if (key == 't' || key == 'T' || key == 'q' || (key == 27 && !previewing)) break;
        if (key == 27) { previewing = 0; notice = 0; }
        else if (key == 'h' || key == 'H') view = 0;
        else if (key == 'f' || key == 'F') { view = 1; list(); }
        else if (key == 's' || key == 'S') view = 2;
        else if (key == '\t') { view = (view + 1) % 3; if (view == 1) list(); }
        else if (view == 1 && previewing) {
            if ((key == KEY_PGDN || key == KEY_DOWN) && preview_page < last_preview_page()) preview_page++;
            if ((key == KEY_PGUP || key == KEY_UP) && preview_page) preview_page--;
        } else if (view == 1) {
            if (key == KEY_DOWN && selected + 1 < count) selected++;
            if (key == KEY_UP && selected) selected--;
            if (key == '\n') file_open();
            if (key == '\b' && fat_mounted()) {
                if (fat_chdir("..")) notice = "Cannot open parent directory."; else list();
            }
        }
        draw();
    }
    if (restore) fat_chdir(saved_dir);
    render_screen();
}
