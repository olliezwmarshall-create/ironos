#include "kernel.h"

/* VGA mode 13h: a first graphics backend, not an accelerated GPU driver. */
static unsigned char saved_regs[61], saved_palette[768], saved_font[65536];
static unsigned char pixels[320 * 200];
static unsigned char text_screen[4000];
static void reg(unsigned short port, unsigned char index, unsigned char value) {
    outb(port, index); outb(port + 1, value);
}
static unsigned char read_reg(unsigned short port, unsigned char index) {
    outb(port, index); return inb(port + 1);
}
static void read_regs(unsigned char *r) {
    *r++ = inb(0x3CC);
    for (int i = 0; i < 5; i++) *r++ = read_reg(0x3C4, i);
    for (int i = 0; i < 25; i++) *r++ = read_reg(0x3D4, i);
    for (int i = 0; i < 9; i++) *r++ = read_reg(0x3CE, i);
    for (int i = 0; i < 21; i++) { inb(0x3DA); outb(0x3C0, i); *r++ = inb(0x3C1); }
    inb(0x3DA); outb(0x3C0, 0x20);
}
static void write_regs(const unsigned char *r) {
    outb(0x3C2, *r++);
    for (int i = 0; i < 5; i++) reg(0x3C4, i, *r++);
    reg(0x3D4, 0x03, read_reg(0x3D4, 0x03) | 0x80);
    reg(0x3D4, 0x11, read_reg(0x3D4, 0x11) & ~0x80);
    for (int i = 0; i < 25; i++) {
        unsigned char v = *r++;
        if (i == 3) v |= 0x80;
        if (i == 17) v &= ~0x80;
        reg(0x3D4, i, v);
    }
    for (int i = 0; i < 9; i++) reg(0x3CE, i, *r++);
    for (int i = 0; i < 21; i++) { inb(0x3DA); outb(0x3C0, i); outb(0x3C0, *r++); }
    inb(0x3DA); outb(0x3C0, 0x20);
}
static void font_plane(void) {
    reg(0x3C4, 2, 4); reg(0x3C4, 4, 6);
    reg(0x3CE, 4, 2); reg(0x3CE, 5, 0); reg(0x3CE, 6, 4);
}
static const unsigned char mode13[61] = {
    0x63, 0x03,0x01,0x0F,0x00,0x0E,
    0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,
    0x00,0x00,0x00,0x00,0x00,0x00,0x9C,0x0E,0x8F,0x28,
    0x40,0x96,0xB9,0xA3,0xFF,
    0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,
    0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x41,0x00,0x0F,0x00,0x00
};
static const char world[12][13] = {
    "111111111111", "100000000001", "100020000001", "100020000001",
    "100020000001", "100000000001", "100000003301", "100000000001",
    "104440000001", "100000000001", "100000000001", "111111111111"
};
static const short sine[256] = {
0,25,50,75,100,125,150,175,200,224,249,273,297,321,345,369,392,415,438,460,483,505,526,548,569,590,610,630,650,669,688,706,724,742,759,775,792,807,822,837,851,865,878,891,903,915,926,936,946,955,964,972,980,987,993,999,1004,1009,1013,1016,1019,1021,1023,1024,1024,1024,1023,1021,1019,1016,1013,1009,1004,999,993,987,980,972,964,955,946,936,926,915,903,891,878,865,851,837,822,807,792,775,759,742,724,706,688,669,650,630,610,590,569,548,526,505,483,460,438,415,392,369,345,321,297,273,249,224,200,175,150,125,100,75,50,25,0,-25,-50,-75,-100,-125,-150,-175,-200,-224,-249,-273,-297,-321,-345,-369,-392,-415,-438,-460,-483,-505,-526,-548,-569,-590,-610,-630,-650,-669,-688,-706,-724,-742,-759,-775,-792,-807,-822,-837,-851,-865,-878,-891,-903,-915,-926,-936,-946,-955,-964,-972,-980,-987,-993,-999,-1004,-1009,-1013,-1016,-1019,-1021,-1023,-1024,-1024,-1024,-1023,-1021,-1019,-1016,-1013,-1009,-1004,-999,-993,-987,-980,-972,-964,-955,-946,-936,-926,-915,-903,-891,-878,-865,-851,-837,-822,-807,-792,-775,-759,-742,-724,-706,-688,-669,-650,-630,-610,-590,-569,-548,-526,-505,-483,-460,-438,-415,-392,-369,-345,-321,-297,-273,-249,-224,-200,-175,-150,-125,-100,-75,-50,-25
};
static int wall(int x, int y) {
    if (x < 0 || y < 0 || x >= 12 || y >= 12) return 1;
    return world[y][x] - '0';
}
static int free_at(int x, int y) {
    for (int dy = -160; dy <= 160; dy += 320)
        for (int dx = -160; dx <= 160; dx += 320)
            if (wall((x + dx) / 1024, (y + dy) / 1024)) return 0;
    return 1;
}
static void label(int x, int y, const char *s) {
    for (; *s && x <= 312; s++, x += 8)
        for (int row = 0; row < 8; row++) {
            unsigned char bits = saved_font[(unsigned char)*s * 32 + row * 2];
            for (int col = 0; col < 8; col++)
                pixels[(y + row) * 320 + x + col] = bits & (0x80 >> col) ? 255 : 0;
        }
}
static void frame(int px, int py, unsigned int angle) {
    int dirx = sine[(angle + 64) & 255], diry = sine[angle];
    for (int y = 0; y < 200; y++)
        memset(pixels + y * 320, y < 100 ? 1 + y / 8 : 16 + (y - 100) / 7, 320);
    for (int x = 0; x < 320; x++) {
        int camera = (2 * x - 320) * 676 / 320;
        int rx = dirx - diry * camera / 1024, ry = diry + dirx * camera / 1024;
        int ax = rx < 0 ? -rx : rx, ay = ry < 0 ? -ry : ry;
        int mx = px / 1024, my = py / 1024;
        int sx = rx < 0 ? -1 : 1, sy = ry < 0 ? -1 : 1;
        int dx = ax ? 1048576 / ax : 0x1000000;
        int dy = ay ? 1048576 / ay : 0x1000000;
        int tx = ax ? (rx < 0 ? px % 1024 : 1024 - px % 1024) * 1024 / ax : dx;
        int ty = ay ? (ry < 0 ? py % 1024 : 1024 - py % 1024) * 1024 / ay : dy;
        int distance = 1, side = 0, hit = 1;
        for (int step = 0; step < 32; step++) {
            if (tx < ty) { distance = tx; tx += dx; mx += sx; side = 0; }
            else { distance = ty; ty += dy; my += sy; side = 1; }
            hit = wall(mx, my);
            if (hit) break;
        }
        if (distance < 64) distance = 64;
        int height = 200 * 1024 / distance;
        int top = (200 - height) / 2, bottom = (200 + height) / 2;
        if (top < 0) top = 0;
        if (bottom > 200) bottom = 200;
        int shade = 15 - distance / 2048 - side * 3;
        if (shade < 3) shade = 3;
        int along = side ? px + rx * distance / 1024 : py + ry * distance / 1024;
        int u = (along & 1023) / 32;
        int texstep = 64 * 65536 / height;
        int texpos = (top - (200 - height) / 2) * texstep;
        for (int y = top; y < bottom; y++, texpos += texstep) {
            int v = texpos >> 16;
            int mortar = !(v & 15) || !((u + ((v >> 4) & 1) * 8) & 15);
            pixels[y * 320 + x] = hit * 32 + (mortar ? 1 : shade);
        }
    }
    // Small top-down map and a center crosshair; walls match collision geometry.
    for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++)
        for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++)
            pixels[(y * 3 + j + 4) * 320 + x * 3 + i + 4] = wall(x, y) ? 240 : 8;
    pixels[(py * 3 / 1024 + 4) * 320 + px * 3 / 1024 + 4] = 255;
    for (int i = -3; i <= 3; i++) { pixels[100 * 320 + 160 + i] = 255; pixels[(100 + i) * 320 + 160] = 255; }
    int mapx = px * 3 / 1024 + 4, mapy = py * 3 / 1024 + 4;
    pixels[(mapy + diry * 2 / 1024) * 320 + mapx + dirx * 2 / 1024] = 255;
    label(152, 4, "IRONOS | CPU 3D");
    label(8, 188, "W/S MOVE A/D TURN Q/E STRAFE ESC EXIT");
    volatile unsigned char *vga = (volatile unsigned char *)0xA0000;
    for (int i = 0; i < 64000; i++) vga[i] = pixels[i];
}
void graphics_demo(void) {
    print_string("3D demo: W/S move, A/D turn, Q/E strafe. Esc exits."); newline();
    print_string("Standard VGA required. Software rendering; no GPU acceleration."); newline();
    memcpy(text_screen, (const void *)VGA_ADDRESS, 4000);
    read_regs(saved_regs);
    outb(0x3C7, 0);
    for (int i = 0; i < 768; i++) saved_palette[i] = inb(0x3C9);
    font_plane();
    volatile unsigned char *vga = (volatile unsigned char *)0xA0000;
    for (int i = 0; i < 65536; i++) saved_font[i] = vga[i];
    write_regs(mode13);
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        if (i < 16) { r = 5 + i; g = 12 + i; b = 24 + i; }
        else if (i < 32) { r = 10 + (i - 16); g = 12 + (i - 16); b = 14 + (i - 16); }
        else if (i < 160) {
            static const unsigned char hues[4][3] = {{52,30,20},{24,42,54},{26,44,26},{54,46,26}};
            int material = i / 32 - 1, light = (i & 31) + 8;
            if (light > 24) light = 24;
            r = hues[material][0] * light / 24;
            g = hues[material][1] * light / 24;
            b = hues[material][2] * light / 24;
        } else { r = g = b = i == 255 ? 63 : 42; }
        outb(0x3C9, r); outb(0x3C9, g); outb(0x3C9, b);
    }
    int px = 2560, py = 5632;
    unsigned int angle = 0;
    unsigned long last = uptime_ticks;
    frame(px, py, angle);
    for (;;) {
        int k = keyboard_getchar();
        if (k == 27) break;
        unsigned long now = uptime_ticks;
        if (now - last < 3) { halt(); continue; }
        unsigned long elapsed = now - last;
        if (elapsed > 10) elapsed = 10;
        last = now;
        int turn = keyboard_key_down('d');
        turn -= keyboard_key_down('a');
        angle = (angle + turn * elapsed) & 255;
        int forward = keyboard_key_down('w');
        forward -= keyboard_key_down('s');
        int strafe = keyboard_key_down('e') - keyboard_key_down('q');
        int dx = sine[(angle + 64) & 255], dy = sine[angle];
        if (forward && strafe) { dx = dx * 707 / 1000; dy = dy * 707 / 1000; }
        int nx = px + (dx * forward - dy * strafe) * (int)elapsed / 40;
        int ny = py + (dy * forward + dx * strafe) * (int)elapsed / 40;
        if (free_at(nx, py)) px = nx;
        if (free_at(px, ny)) py = ny;
        frame(px, py, angle);
    }
    font_plane();
    for (int i = 0; i < 65536; i++) vga[i] = saved_font[i];
    write_regs(saved_regs);
    outb(0x3C8, 0);
    for (int i = 0; i < 768; i++) outb(0x3C9, saved_palette[i]);
    memcpy((void *)VGA_ADDRESS, text_screen, 4000);
    render_screen();
    print_string("3D demo closed."); newline();
}
