#include "kernel.h"

#define CAT_MAX (32 * 1024)
#define CP_MAX  (8 * 1024 * 1024)

static int cur_vol = -1;           // index into the volume list of the mounted volume (-1 = none)

static void say(const char *msg) {
    print_string(msg);
    newline();
}

// "label: 1234 suffix"
static void say_num(const char *label, unsigned long value, const char *suffix) {
    char num[16];
    int_to_string(value, num);
    print_string(label);
    print_string(num);
    say(suffix);
}

static void set_color(unsigned char color, const char *name) {
    current_color = color;
    print_string("Color: ");
    say(name);
}

static void print_hex64(unsigned long long v) {
    char buf[9];
    print_string("0x");
    if (v >> 32) {
        hex_to_string((unsigned int)(v >> 32), buf);
        print_string(buf);
    }
    hex_to_string((unsigned int)v, buf);
    print_string(buf);
}

static void fs_error(const char *what, int r) {
    print_string(what);
    print_string(": ");
    say(fat_strerror(r));
}

// ---------------------------------------------------------------- argument helpers

// Strip one pair of surrounding double quotes.
static char *unquote(char *s) {
    unsigned int n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s[n - 1] = '\0'; return s + 1; }
    return s;
}

// Split into up to `max` words; "quoted words" may contain spaces.
static int split_args(char *arg, char *out[], int max) {
    int n = 0;
    char *p = arg;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            out[n++] = p;
            while (*p && *p != '"') p++;
        } else {
            out[n++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = '\0';
    }
    return n;
}

// ---------------------------------------------------------------- prompt / mounting

void print_prompt(void) {
    print_string("kernel");
    if (fat_mounted()) {
        print_string(":");
        print_string(fat_getcwd());
        print_string(fat_writable() ? "#> " : "$> ");      // '#' means the disk can be written to
    } else {
        print_string("$> ");
    }
}

static void mount_volume(int index, int rw) {
    const struct volume *v = volume_get(index);
    if (!v) { say("No such volume. Type 'disks' to see the list."); return; }
    if (!v->fat) {
        print_string("Volume ");
        print_string(ptype_name(v->ptype));
        say(": the FAT driver cannot mount this.");
        return;
    }
    if (fat_mount(v->drive, v->start, v->sectors, rw) != 0) {
        print_string("Mount failed: ");
        say(fat_status());
        cur_vol = -1;
        return;
    }
    cur_vol = index;
    struct fat_info fi;
    fat_get_info(&fi);
    char num[16];
    int_to_string(fi.bits, num);
    print_string("Mounted volume ");
    int_to_string((unsigned long)index, num);
    print_string(num);
    print_string(": ");
    say(fat_status());
    if (rw && fat_writable()) {
        say("Writing is enabled. Don't power off or reset while files are being saved.");
    }
}

// At boot: mount the first FAT volume we can find, read-only.
static void automount(void) {
    for (int i = 0; i < volume_count(); i++) {
        const struct volume *v = volume_get(i);
        if (v->fat && fat_mount(v->drive, v->start, v->sectors, 0) == 0) { cur_vol = i; return; }
    }
}

void print_disk_summary(void) {
    static int done = 0;
    if (!done) { automount(); done = 1; }

    int disks = 0;
    for (int i = 0; i < disk_count(); i++) if (disk_get(i)->usable) disks++;
    if (disks == 0) {
        say("Disk: no usable disk found. (Type 'lspci' to see what controller this machine has.)");
        return;
    }
    char num[16];
    int_to_string(disks, num);
    print_string("Disk: ");
    print_string(num);
    print_string(" disk(s) (IDE/SATA/NVMe). ");
    if (fat_mounted()) {
        struct fat_info fi;
        fat_get_info(&fi);
        int_to_string(fi.bits, num);
        print_string("Volume ");
        int_to_string((unsigned long)cur_vol, num);
        print_string(num);
        print_string(" mounted read-only. ");
        say("Type 'disks' to see them all.");
    } else {
        say("No FAT volume found: type 'disks'.");
    }
}

// ---------------------------------------------------------------- memory commands

static void cmd_meminfo(void) {
    unsigned int heap_total, heap_used, heap_free;
    heap_stats(&heap_total, &heap_used, &heap_free);

    say_num("Usable RAM : ", pmm_total_pages() * 4, " KB");
    say_num("Free pages : ", pmm_free_page_count() * 4, " KB");

    char buf[9];
    print_string("Kernel     : 0x");
    hex_to_string((unsigned int)kernel_start, buf);
    print_string(buf);
    print_string(" - 0x");
    hex_to_string((unsigned int)kernel_end, buf);
    print_string(buf);
    newline();

    say_num("Heap total : ", heap_total / 1024, " KB");
    say_num("Heap used  : ", heap_used / 1024, " KB");
}

static void cmd_memmap(void) {
    static const char *names[] = { "?", "usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad" };
    int n = mem_region_count();
    if (n == 0) { say("No memory map available."); return; }

    say("base        length      type");
    for (int i = 0; i < n; i++) {
        const struct mem_region *r = mem_region_get(i);
        print_hex64(r->base);
        print_string(" ");
        print_hex64(r->len);
        print_string(" ");
        say(r->type >= 1 && r->type <= 5 ? names[r->type] : names[0]);
    }
}

// Allocates, fills, checks, frees; verifies memory is reused and the heap returns to its start state.
static void cmd_memtest(void) {
    unsigned int total, used, free_before, free_after;
    heap_stats(&total, &used, &free_before);

    unsigned char *a = kmalloc(1000);
    unsigned char *b = kmalloc(5000);
    unsigned char *c = kmalloc(64);
    if (!a || !b || !c) { say("memtest: FAILED (kmalloc returned NULL)"); return; }

    memset(a, 0xAA, 1000);
    memset(b, 0x55, 5000);
    memset(c, 0x33, 64);

    int ok = 1;
    for (int i = 0; i < 1000; i++) if (a[i] != 0xAA) ok = 0;
    for (int i = 0; i < 5000; i++) if (b[i] != 0x55) ok = 0;
    for (int i = 0; i < 64; i++)   if (c[i] != 0x33) ok = 0;
    if (!ok) { say("memtest: FAILED (data corrupted)"); return; }

    kfree(b);
    unsigned char *d = kmalloc(4000);        // should reuse the space b left behind
    int reused = (d == b);

    kfree(a);
    kfree(c);
    kfree(d);

    heap_stats(&total, &used, &free_after);
    if (!reused)                        say("memtest: FAILED (freed memory was not reused)");
    else if (free_after != free_before) say("memtest: FAILED (heap did not return to its starting size)");
    else                                say("memtest: PASSED");
}

// ---------------------------------------------------------------- disk commands

static int need_fs(void) {
    if (fat_mounted()) return 1;
    say("No filesystem mounted. Type 'disks', then 'mount <number>'.");
    return 0;
}

static void cmd_disks(void) {
    screen_batch_begin();
    for (int d = 0; d < disk_count(); d++) {
        const struct disk *a = disk_get(d);
        char num[16];
        print_string(a->name); print_string(": ");
        print_string(a->desc); print_string("  ");
        print_string(a->model[0] ? a->model : "(unnamed)");
        if (!a->usable) { say("  [CD/DVD or unsupported - not readable]"); continue; }
        int_to_string(a->sectors / 2048, num);
        print_string("  "); print_string(num); print_string(" MB");
        if (a->truncated) print_string("  [only the first 2 TB is usable]");
        newline();
        int parts = 0;
        for (int i = 0; i < volume_count(); i++) {
            const struct volume *v = volume_get(i);
            if (v->drive != d) continue;
            parts++;
            int_to_string((unsigned long)i, num);
            print_string("   ["); print_string(num); print_string("] ");
            if (v->part == 0) { print_string("whole disk  "); }
            else { print_string("partition "); int_to_string((unsigned long)v->part, num); print_string(num); print_string("  "); }
            print_string(ptype_name(v->ptype));
            int_to_string(v->sectors / 2048, num);
            print_string("  "); print_string(num); print_string(" MB");
            if (i == cur_vol && fat_mounted()) print_string(fat_writable() ? "   <- mounted read-write" : "   <- mounted read-only");
            newline();
        }
        if (!parts) say("   (no partitions found)");
    }
    if (!disk_count()) say("No disks found.");
    else say("mount <number> [rw]   -- 'rw' enables writing (be careful with real data!)");
    screen_batch_end();
}

static void cmd_mount(char *arg) {
    char *w[2];
    int n = split_args(arg, w, 2);
    if (n == 0) {
        if (fat_mounted()) {
            say_num("Mounted volume ", (unsigned long)cur_vol, fat_writable() ? " (read-write)" : " (read-only)");
        } else say("Nothing mounted. Usage: mount <number> [rw]");
        return;
    }
    int vol = 0;
    for (const char *p = w[0]; *p; p++) {
        if (*p < '0' || *p > '9') { say("Usage: mount <number> [rw]"); return; }
        vol = vol * 10 + (*p - '0');
    }
    int rw = 0;
    if (n >= 2) {
        if (strcmp(w[1], "rw") == 0) rw = 1;
        else { say("Usage: mount <number> [rw]"); return; }
    }
    mount_volume(vol, rw);
}

static void cmd_diskinfo(void) {
    if (!fat_mounted()) { need_fs(); return; }
    struct fat_info fi;
    fat_get_info(&fi);
    say_num("Filesystem : FAT", fi.bits, fi.writable ? " (read-write)" : " (read-only)");
    { print_string("Disk       : "); say(disk_get(fi.drive)->name); }
    say_num("Volume at  : sector ", fi.start_lba, "");
    say_num("Cluster    : ", fi.cluster_bytes, " bytes");
    say_num("Clusters   : ", fi.total_clusters, "");
}

static void cmd_df(void) {
    if (!need_fs()) return;
    struct fat_info fi;
    fat_get_info(&fi);
    say("Scanning the FAT...");
    unsigned int fc;
    int r = fat_free_clusters(&fc);
    if (r != 0) { fs_error("df", r); return; }
    unsigned int per = fi.cluster_bytes / 512;            // sectors per cluster
    say_num("Total : ", (fi.total_clusters * per) / 2048, " MB");
    say_num("Free  : ", (fc * per) / 2048, " MB");
}

struct ls_ctx { unsigned int files, dirs; };

static void ls_entry(const struct fat_dirent *e, void *vctx) {
    struct ls_ctx *ctx = vctx;
    if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) return;

    char num[16];
    if (e->attr & FAT_ATTR_DIR) {
        ctx->dirs++;
        print_string("   <DIR>");
    } else {
        ctx->files++;
        int_to_string(e->size, num);
        for (unsigned int i = strlen(num); i < 8; i++) print_char(' ');
        print_string(num);
    }
    print_string("  ");
    say(e->name);
}

static void cmd_ls(char *arg) {
    if (!need_fs()) return;
    arg = unquote(arg);
    struct fat_dirent e;
    if (fat_lookup(*arg ? arg : ".", &e) < 0) { say("No such file or directory."); return; }

    struct ls_ctx ctx = { 0, 0 };
    screen_batch_begin();
    if (e.attr & FAT_ATTR_DIR) fat_list_dir(&e, ls_entry, &ctx);
    else                       ls_entry(&e, &ctx);
    print_string("  ");
    say_num("", ctx.files, " file(s)");
    print_string("  ");
    say_num("", ctx.dirs, " dir(s)");
    screen_batch_end();
}

static void cmd_cd(char *arg) {
    if (!need_fs()) return;
    arg = unquote(arg);
    int r = fat_chdir(*arg ? arg : "/");
    if (r == -1) say("No such directory.");
    else if (r == -2) say("Not a directory.");
}

static void cmd_cat(char *arg) {
    if (!need_fs()) return;
    arg = unquote(arg);
    if (!*arg) { say("Usage: cat <file>"); return; }

    struct fat_dirent e;
    if (fat_lookup(arg, &e) < 0) { say("No such file."); return; }
    if (e.attr & FAT_ATTR_DIR) { say("That is a directory."); return; }
    if (e.size == 0) return;
    if (e.size > CAT_MAX) { say("File too large to display (limit 32 KB). Try 'edit' or 'hexdump'."); return; }

    char *buf = kmalloc(e.size);
    if (!buf) { say("Out of memory."); return; }

    int n = fat_read_file(&e, buf, e.size);
    if (n < 0) {
        say("Disk read error.");
    } else {
        screen_batch_begin();
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n')      newline();
            else if (c == '\r') { }
            else if (c == '\t') { print_char(' '); print_char(' '); print_char(' '); print_char(' '); }
            else if (c >= 32 && c < 127) print_char(c);
            else                print_char('.');
        }
        if (n > 0 && buf[n - 1] != '\n') newline();
        screen_batch_end();
    }
    kfree(buf);
}

static void cmd_hexdump(char *arg) {
    if (!need_fs()) return;
    arg = unquote(arg);
    if (!*arg) { say("Usage: hexdump <file>"); return; }
    struct fat_dirent e;
    if (fat_lookup(arg, &e) < 0) { say("No such file."); return; }
    if (e.attr & FAT_ATTR_DIR) { say("That is a directory."); return; }

    unsigned char data[256];
    int n = fat_read_file(&e, data, sizeof data);
    if (n < 0) { say("Disk read error."); return; }

    screen_batch_begin();
    for (int off = 0; off < n; off += 16) {
        char hx[9], line[80];
        int k = 0;
        hex_to_string((unsigned int)off, hx);
        for (int i = 4; i < 8; i++) line[k++] = hx[i];
        line[k++] = ' ';
        for (int i = 0; i < 16; i++) {
            if (off + i < n) {
                const char *d = "0123456789ABCDEF";
                line[k++] = d[data[off + i] >> 4];
                line[k++] = d[data[off + i] & 15];
            } else { line[k++] = ' '; line[k++] = ' '; }
            line[k++] = ' ';
        }
        line[k++] = ' ';
        for (int i = 0; i < 16 && off + i < n; i++) {
            unsigned char c = data[off + i];
            line[k++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[k] = '\0';
        say(line);
    }
    if (e.size > (unsigned int)n) say_num("(showing the first 256 of ", e.size, " bytes)");
    screen_batch_end();
}

// ---------------------------------------------------------------- file management

static int need_rw(void) {
    if (!need_fs()) return 0;
    if (!fat_writable()) { say("This volume is read-only. Use 'mount <number> rw' to enable writing."); return 0; }
    return 1;
}

static void cmd_edit(char *arg) {
    if (!need_fs()) return;
    arg = unquote(arg);
    if (!*arg) { say("Usage: edit <file>"); return; }
    int r = editor_run(arg);
    if (r == FAT_ERR_INVAL) say("Cannot edit: binary file, or larger than 512 KB.");
    else if (r != 0) fs_error("edit", r);
}

static void cmd_touch(char *arg) {
    if (!need_rw()) return;
    arg = unquote(arg);
    if (!*arg) { say("Usage: touch <file>"); return; }
    struct fat_dirent e;
    if (fat_lookup(arg, &e) == 0) return;                 // already exists
    int r = fat_write_file(arg, "", 0);
    if (r != 0) fs_error("touch", r);
}

static void cmd_mkdir(char *arg) {
    if (!need_rw()) return;
    arg = unquote(arg);
    if (!*arg) { say("Usage: mkdir <name>"); return; }
    int r = fat_mkdir(arg);
    if (r != 0) fs_error("mkdir", r);
}

static void cmd_rm(char *arg) {
    if (!need_rw()) return;
    arg = unquote(arg);
    if (!*arg) { say("Usage: rm <file>"); return; }
    int r = fat_remove(arg);
    if (r != 0) fs_error("rm", r);
}

static void cmd_rmdir(char *arg) {
    if (!need_rw()) return;
    arg = unquote(arg);
    if (!*arg) { say("Usage: rmdir <directory>"); return; }
    int r = fat_rmdir(arg);
    if (r != 0) fs_error("rmdir", r);
}

static void cmd_mv(char *arg) {
    if (!need_rw()) return;
    char *w[2];
    if (split_args(arg, w, 2) != 2) { say("Usage: mv <from> <to>   (use \"quotes\" around names with spaces)"); return; }
    int r = fat_rename(w[0], w[1]);
    if (r != 0) fs_error("mv", r);
}

static void cmd_cp(char *arg) {
    if (!need_rw()) return;
    char *w[2];
    if (split_args(arg, w, 2) != 2) { say("Usage: cp <from> <to>   (use \"quotes\" around names with spaces)"); return; }

    struct fat_dirent src, dst;
    if (fat_lookup(w[0], &src) < 0) { say("cp: no such file."); return; }
    if (src.attr & FAT_ATTR_DIR) { say("cp: directories are not supported."); return; }
    if (src.size > CP_MAX) { say("cp: file too large (limit 8 MB)."); return; }

    char target[256];
    unsigned int n = strlen(w[1]);
    if (n + 1 > sizeof target) { say("cp: path too long."); return; }
    memcpy(target, w[1], n + 1);
    if (fat_lookup(w[1], &dst) == 0) {
        if (dst.attr & FAT_ATTR_DIR) {                     // copy into the directory, keeping the name
            unsigned int m = strlen(src.name);
            if (n + m + 2 > sizeof target) { say("cp: path too long."); return; }
            if (n > 0 && target[n - 1] != '/') target[n++] = '/';
            memcpy(target + n, src.name, m + 1);
            if (fat_lookup(target, &dst) == 0 && (dst.attr & FAT_ATTR_DIR)) { say("cp: target is a directory."); return; }
        }
        if (dst.cluster == src.cluster && src.size > 0 && !(dst.attr & FAT_ATTR_DIR)) { say("cp: source and target are the same file."); return; }
    }

    char *buf = src.size ? kmalloc(src.size) : (char *)1;
    if (!buf) { say("cp: out of memory."); return; }
    int got = src.size ? fat_read_file(&src, buf, src.size) : 0;
    if (got < 0 || (unsigned int)got != src.size) { say("cp: read error."); if (src.size) kfree(buf); return; }
    int r = fat_write_file(target, src.size ? buf : "", src.size);
    if (src.size) kfree(buf);
    if (r != 0) fs_error("cp", r);
}

// echo text            -> prints it
// echo text > file     -> writes it to a file (replacing it);  >> appends
static void cmd_echo(char *arg) {
    char *gt = arg;
    while (*gt && *gt != '>') gt++;
    if (!*gt) { say(arg); return; }

    int append = (gt[1] == '>');
    char *file = gt + (append ? 2 : 1);
    while (*file == ' ') file++;
    *gt = '\0';
    unsigned int tl = strlen(arg);
    while (tl > 0 && arg[tl - 1] == ' ') arg[--tl] = '\0';
    unsigned int fl = strlen(file);
    while (fl > 0 && file[fl - 1] == ' ') file[--fl] = '\0';
    file = unquote(file);
    if (!*file) { say("Usage: echo text > file"); return; }
    if (!need_rw()) return;

    unsigned int old = 0;
    struct fat_dirent e;
    if (append && fat_lookup(file, &e) == 0) {
        if (e.attr & FAT_ATTR_DIR) { say("echo: that is a directory."); return; }
        if (e.size > 1024 * 1024) { say("echo: file too large to append to."); return; }
        old = e.size;
    }
    char *buf = kmalloc(old + tl + 2);
    if (!buf) { say("echo: out of memory."); return; }
    if (old && fat_read_file(&e, buf, old) != (int)old) { say("echo: read error."); kfree(buf); return; }
    memcpy(buf + old, arg, tl);
    buf[old + tl] = '\n';
    int r = fat_write_file(file, buf, old + tl + 1);
    kfree(buf);
    if (r != 0) fs_error("echo", r);
}

static void print2(unsigned int v) {
    print_char((char)('0' + (v / 10) % 10));
    print_char((char)('0' + v % 10));
}

static void cmd_date(void) {
    struct rtc_time t;
    if (rtc_get(&t) != 0) { say("The clock could not be read."); return; }
    char num[16];
    int_to_string(t.year, num);
    print_string(num); print_char('-'); print2(t.month); print_char('-'); print2(t.day);
    print_char(' '); print2(t.hour); print_char(':'); print2(t.minute); print_char(':'); print2(t.second);
    newline();
}

// ---------------------------------------------------------------- command dispatch

static void run_command(void) {
    // Split "cmd arg text" into the command word and the rest of the line.
    char *cmd = cmd_buffer;
    while (*cmd == ' ') cmd++;
    char *arg = cmd;
    while (*arg && *arg != ' ') arg++;
    if (*arg) {
        *arg++ = '\0';
        while (*arg == ' ') arg++;
    }
    unsigned int len = strlen(arg);
    if (strcmp(cmd, "echo") != 0)
        while (len > 0 && arg[len - 1] == ' ') arg[--len] = '\0';

    if (*cmd == '\0') return;

    if (strcmp(cmd, "help") == 0) {
        say("System  : help clear color banner hello uptime date reboot shutdown");
        say("Hardware: meminfo memmap memtest cpuinfo lspci");
        say("Disks   : disks mount [n] [rw]  diskinfo  df");
        say("Files   : ls cd pwd cat hexdump edit touch mkdir rm rmdir cp mv echo");
        say("Tips    : \"quote\" names with spaces; echo hi > file; PgUp/PgDn scroll.");
        say("History : Up/Down recall commands (32 entries, kept until reboot)");
        say("Network : net netcards netauto netoff netconfig dhcp ping resolve");
        say("Graphics: demo3d (software 3D, WASD movement, Esc exits)");
        say("Desktop : desktop (windows + mouse) textdesk apps calc <expression> about");
        say("Install : install <disk-name> (BIOS/legacy, erases target disk)");
        say("Disks start read-only. The prompt shows '#' once writing is enabled.");
    }
    else if (!strcmp(cmd, "net") || !strcmp(cmd, "dhcp") || !strcmp(cmd, "ping") || !strcmp(cmd, "resolve") || !strcmp(cmd,"netcards") || !strcmp(cmd,"netauto") || !strcmp(cmd,"netoff") || !strcmp(cmd,"netconfig")) net_command(cmd, arg);
    else if (!strcmp(cmd,"calc")) { int n;char b[16];if (calc_eval(arg,&n)) say("Invalid expression, overflow or division by zero.");else { signed_string(n,b);say(b); } }
    else if (!strcmp(cmd,"apps")) say("desktop: Home, Files, System, Network, Calc, Clock, Notes; demo3d; edit <file>");
    else if (!strcmp(cmd,"about")) say("IronOS: 32-bit x86 BIOS OS. CPU graphics; IPv4 Ethernet; no TCP/Wi-Fi/USB stack yet.");
    else if (strcmp(cmd, "demo3d") == 0) graphics_demo();
    else if (!strcmp(cmd, "desktop") || !strcmp(cmd, "gui")) gui_run();
    else if (!strcmp(cmd, "textdesk")) desktop_run();
    else if (strcmp(cmd, "clear") == 0)  clear_screen();
    else if (strcmp(cmd, "hello") == 0)  say("Hello from your custom kernel!");
    else if (strcmp(cmd, "color") == 0) {
        if      (strcmp(arg, "0") == 0) set_color(0x0F, "White");
        else if (strcmp(arg, "1") == 0) set_color(0x01, "Blue");
        else if (strcmp(arg, "2") == 0) set_color(0x02, "Green");
        else if (strcmp(arg, "3") == 0) set_color(0x03, "Cyan");
        else if (strcmp(arg, "4") == 0) set_color(0x04, "Red");
        else say("Usage: color 0-4");
    }
    else if (strcmp(cmd, "banner") == 0) print_banner();
    else if (strcmp(cmd, "uptime") == 0) say_num("Uptime: ", uptime_ticks / TIMER_HZ, " seconds");
    else if (strcmp(cmd, "date") == 0)     cmd_date();
    else if (strcmp(cmd, "meminfo") == 0)  cmd_meminfo();
    else if (strcmp(cmd, "memmap") == 0)   cmd_memmap();
    else if (strcmp(cmd, "memtest") == 0)  cmd_memtest();
    else if (strcmp(cmd, "cpuinfo") == 0)  hw_cpuinfo();
    else if (strcmp(cmd, "lspci") == 0)    hw_lspci();
    else if (strcmp(cmd, "disks") == 0)    cmd_disks();
    else if (strcmp(cmd, "install") == 0) {
        if (installer_command(arg)) { cur_vol = -1; disk_scan(); }
    }
    else if (strcmp(cmd, "mount") == 0)    cmd_mount(arg);
    else if (strcmp(cmd, "diskinfo") == 0) cmd_diskinfo();
    else if (strcmp(cmd, "df") == 0)       cmd_df();
    else if (strcmp(cmd, "ls") == 0)       cmd_ls(arg);
    else if (strcmp(cmd, "cd") == 0)       cmd_cd(arg);
    else if (strcmp(cmd, "cat") == 0)      cmd_cat(arg);
    else if (strcmp(cmd, "hexdump") == 0)  cmd_hexdump(arg);
    else if (strcmp(cmd, "edit") == 0)     cmd_edit(arg);
    else if (strcmp(cmd, "touch") == 0)    cmd_touch(arg);
    else if (strcmp(cmd, "mkdir") == 0)    cmd_mkdir(arg);
    else if (strcmp(cmd, "rm") == 0)       cmd_rm(arg);
    else if (strcmp(cmd, "rmdir") == 0)    cmd_rmdir(arg);
    else if (strcmp(cmd, "cp") == 0)       cmd_cp(arg);
    else if (strcmp(cmd, "mv") == 0)       cmd_mv(arg);
    else if (strcmp(cmd, "echo") == 0)     cmd_echo(arg);
    else if (strcmp(cmd, "pwd") == 0)      { if (need_fs()) say(fat_getcwd()); }
    else if (strcmp(cmd, "shutdown") == 0) hw_shutdown();
    else if (strcmp(cmd, "reboot") == 0) {
        say("Rebooting system...");
        system_reboot();
    }
    else say("Unknown command. Type 'help'.");
}

static void scroll_view(int lines) {
    while (lines < 0 && top_rendered_line > 0) { top_rendered_line--; bottom_rendered_line--; lines++; }
    while (lines > 0 && bottom_rendered_line < total_lines_written) { top_rendered_line++; bottom_rendered_line++; lines--; }
    render_screen();
}

#define HISTORY_SIZE 32
static char command_history[HISTORY_SIZE][80];
static char history_draft[80];
static int history_count, history_next, history_position;

static void remember_command(void) {
    if (!buf_index) return;
    int last = (history_next + HISTORY_SIZE - 1) % HISTORY_SIZE;
    if (history_count && !strcmp(command_history[last], cmd_buffer)) return;
    memcpy(command_history[history_next], cmd_buffer, buf_index + 1);
    history_next = (history_next + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) history_count++;
}

/* Replace only the input, including wrapped lines; preserve the prompt/history. */
static void replace_input(const char *input) {
    int start = cursor_row * VGA_WIDTH + cursor_col - buf_index;
    for (int i = 0; i < buf_index; i++) {
        int at = start + i;
        terminal_history[at / VGA_WIDTH][at % VGA_WIDTH] = 0;
    }
    cursor_row = start / VGA_WIDTH; cursor_col = start % VGA_WIDTH;
    buf_index = strlen(input);
    memcpy(cmd_buffer, input, buf_index + 1);
    print_string(cmd_buffer);
    if (cursor_row > bottom_rendered_line) {
        bottom_rendered_line = cursor_row;
        top_rendered_line = bottom_rendered_line - 24;
    }
    render_screen();
    vga_set_cursor(cursor_row - top_rendered_line, cursor_col);
}

static void recall_command(int older) {
    if (!history_count) return;
    if (older) {
        if (history_position == history_count) return;
        if (!history_position) {
            cmd_buffer[buf_index] = 0;
            memcpy(history_draft, cmd_buffer, buf_index + 1);
        }
        history_position++;
    } else {
        if (!history_position) return;
        history_position--;
    }
    int index = (history_next + HISTORY_SIZE - history_position) % HISTORY_SIZE;
    replace_input(history_position ? command_history[index] : history_draft);
}

void Cli() {
    while (1) {
        net_poll();
        int key = keyboard_getchar();

        if (key < 0) {
            halt();          // sleep until the next interrupt (timer or keyboard)
            continue;
        }

        if (key == KEY_UP)        recall_command(1);
        else if (key == KEY_DOWN) recall_command(0);
        else if (key == KEY_PGUP) scroll_view(-12);
        else if (key == KEY_PGDN) scroll_view(12);
        else if (key == '\n') {
            cmd_buffer[buf_index] = '\0';
            remember_command();
            history_position = 0;
            newline();
            run_command();
            buf_index = 0;
            print_prompt();
        }
        else if (key == '\b') {
            if (buf_index > 0) {
                cmd_buffer[buf_index - 1] = 0;
                // Use a separate copy: replace_input also updates cmd_buffer.
                char edited[80];
                memcpy(edited, cmd_buffer, buf_index);
                replace_input(edited);
                history_position = 0;
            }
        }
        else if (key >= ' ' && key < 0x7F && buf_index < 79) {
            history_position = 0;
            cmd_buffer[buf_index++] = (char)key;
            print_char((char)key);
        }
    }
}
