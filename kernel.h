#ifndef KERNEL_H
#define KERNEL_H

#include "fat.h"
#include "blk.h"

#define VGA_ADDRESS 0xB8000
#define MAX_HISTORY_LINES 1000
#define VGA_WIDTH 80
#define TIMER_HZ 100

// Video memory. (Host-side tests point this at a fake buffer.)
#ifdef HOST_TEST
extern unsigned char host_vga[];
#define VGA_MEM host_vga
#else
#define VGA_MEM ((unsigned char *)VGA_ADDRESS)
#endif

// Special key codes returned by keyboard_getchar() (regular keys are plain ASCII)
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_HOME   0x104
#define KEY_END    0x105
#define KEY_PGUP   0x106
#define KEY_PGDN   0x107
#define KEY_DELETE 0x108
#define KEY_CTRL(c) (0x200 + (c))      // Ctrl + lowercase letter, e.g. KEY_CTRL('s')

// Shared global variables
extern int cursor_row;
extern int cursor_col;
extern unsigned char current_color;
extern char cmd_buffer[];
extern int buf_index;
extern int top_rendered_line;
extern int bottom_rendered_line;
extern char terminal_history[MAX_HISTORY_LINES][VGA_WIDTH];
extern int total_lines_written;
extern volatile unsigned long uptime_ticks;   // incremented by the timer IRQ, TIMER_HZ per second

// Register snapshot pushed by the ISR stubs (see isr.s)
struct registers {
    unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax;  // pusha
    unsigned int int_no, err_code;
    unsigned int eip, cs, eflags;                         // pushed by the CPU
};
typedef void (*irq_handler_t)(struct registers *);

// Port I/O
#ifdef HOST_TEST
unsigned char inb(unsigned short port);
void outb(unsigned short port, unsigned char val);
unsigned short inw(unsigned short port);
void outw(unsigned short port, unsigned short val);
unsigned int inl(unsigned short port);
void outl(unsigned short port, unsigned int val);
void insw(unsigned short port, void *buf, unsigned int count);
void outsw(unsigned short port, const void *buf, unsigned int count);
#else
static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned short inw(unsigned short port) {
    unsigned short result;
    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
static inline void outw(unsigned short port, unsigned short val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned int inl(unsigned short port) {
    unsigned int result;
    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
static inline void outl(unsigned short port, unsigned int val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
// Read/write `count` 16-bit words between an I/O port and memory.
static inline void insw(unsigned short port, void *buf, unsigned int count) {
    __asm__ volatile ("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}
static inline void outsw(unsigned short port, const void *buf, unsigned int count) {
    __asm__ volatile ("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}
#endif
// Memory-mapped I/O (AHCI and NVMe registers). Host-side tests swap these for an emulated device.
#ifdef HOST_TEST
unsigned int mmio_read32(const volatile void *addr);
void mmio_write32(volatile void *addr, unsigned int value);
#else
static inline unsigned int mmio_read32(const volatile void *addr) { return *(const volatile unsigned int *)addr; }
static inline void mmio_write32(volatile void *addr, unsigned int value) { *(volatile unsigned int *)addr = value; }
#endif
#define mem_barrier() __asm__ volatile ("" ::: "memory")

static inline void io_wait(void) { outb(0x80, 0); }
static inline void sti(void) { __asm__ volatile ("sti"); }
static inline void cli_disable(void) { __asm__ volatile ("cli"); }
static inline void halt(void) { __asm__ volatile ("hlt"); }

// Physical memory regions reported by the bootloader
struct mem_region {
    unsigned long long base;
    unsigned long long len;
    unsigned int type;        // 1 = usable RAM, 2 = reserved, 3 = ACPI reclaimable, 4 = ACPI NVS, 5 = bad
};

// Linker-provided symbols (see linker.ld)
extern char kernel_start[];
extern char kernel_end[];

// kernel.c
void kernel_main(unsigned int magic, unsigned int mbi);

// screen.c
void clear_screen();
void newline();
void print_char(char c);
void print_string(const char *s);
void print_banner();
void render_screen();
void screen_batch_begin();   // hold off redrawing while printing lots of text...
void screen_batch_end();     // ...then redraw once
void vga_set_cursor(int row, int col);   // hardware (blinking) cursor; row 25 hides it

// system.c
int strcmp(const char *s1, const char *s2);
unsigned int strlen(const char *s);
void *memset(void *dst, int value, unsigned int n);
void *memcpy(void *dst, const void *src, unsigned int n);
void *memmove(void *dst, const void *src, unsigned int n);
int memcmp(const void *a, const void *b, unsigned int n);
void hex_to_string(unsigned int v, char *buf);   // 8 uppercase hex digits, no prefix
void int_to_string(unsigned long n, char *buf);
void system_reboot();

// Real-time clock (CMOS)
struct rtc_time { unsigned int year, month, day, hour, minute, second; };
int rtc_get(struct rtc_time *t);                 // 0 on success

// hw.c - hardware information and power
void hw_cpuinfo(void);
void hw_lspci(void);
void hw_shutdown(void);

// gdt.c
void gdt_init(void);

// interrupts.c
void interrupts_init(void);
void register_interrupt_handler(int int_no, irq_handler_t handler);

// timer.c
void timer_init(unsigned int hz);

// keyboard.c
struct mouse_event { int x, y, dx, dy; unsigned int buttons, pressed; };
void mouse_init(void);
int mouse_present(void);
void mouse_receive_byte(int value);
int mouse_get_event(struct mouse_event *event);
void mouse_get_state(struct mouse_event *event);
void ps2_irq(struct registers *r);
void keyboard_receive_byte(unsigned char sc);
void keyboard_init(void);
int keyboard_key_down(int key);   // held ASCII key (lowercase), independent of repeat
void graphics_demo(void);
void graphics_begin(void);
void graphics_end(void);
void graphics_rect(int x, int y, int width, int height, unsigned char color);
void graphics_text(int x, int y, const char *s, unsigned char color, int max);
void graphics_present(void);
void gui_run(void);
int keyboard_getchar(void);   // returns -1 if no key is waiting

// pmm.c - physical page allocator (1 page = 4096 bytes; no paging yet, so physical == virtual)
unsigned int pmm_init(unsigned int mbi);          // returns number of usable pages found
void *pmm_alloc_pages(unsigned int count);        // contiguous; returns 0 on failure
void pmm_free_pages(void *addr, unsigned int count);
unsigned int pmm_total_pages(void);
unsigned int pmm_free_page_count(void);
int mem_region_count(void);
const struct mem_region *mem_region_get(int index);

// heap.c - kernel heap
void heap_init(void);
void *kmalloc(unsigned int size);
void kfree(void *ptr);
void heap_stats(unsigned int *total, unsigned int *used, unsigned int *free_bytes);

// ata.c - IDE (PIO): drive index 0..3 = primary master/slave, secondary master/slave
#define ATA_MAX_DRIVES 4
struct ata_drive {
    int present;               // something answered on this position
    int atapi;                 // CD/DVD-type device (not usable as a disk)
    int lba48;                 // supports 48-bit LBA (disks over 128 GB)
    unsigned short io, ctl;    // base I/O port and control port of the channel
    int slave;
    unsigned int sectors;      // sectors reachable with 32-bit LBAs
    int truncated;             // disk is bigger than that
    char model[41];
};
const struct ata_drive *ata_drive_get(int index);

// rtl8139.c / net.c (polled Ethernet + IPv4)
int rtl8139_init(void);
int rtl8139_present(void);
int rtl8139_link(void);
const unsigned char *rtl8139_mac(void);
int rtl8139_send(const void *packet, unsigned int n);
int rtl8139_receive(unsigned char *packet, unsigned int capacity);
void net_poll(void);
void net_command(const char *cmd, const char *arg);

// pci.c
struct pci_loc { unsigned char bus, dev, fn; };
unsigned int pci_read32(unsigned int bus, unsigned int dev, unsigned int fn, unsigned int reg);
void pci_write32(unsigned int bus, unsigned int dev, unsigned int fn, unsigned int reg, unsigned int value);
int pci_find_class(unsigned int cls, unsigned int sub, unsigned int prog, struct pci_loc *loc);   // 1 if found

// disk.c - partition discovery
struct volume {
    int drive;                 // index into the disk registry (blk.h)
    unsigned int start;        // first sector of the partition
    unsigned int sectors;
    unsigned char ptype;       // MBR partition type (0 for a bare, unpartitioned FAT disk)
    int part;                  // partition number: 1-4 primary, 5+ logical, 0 = whole disk
    int fat;                   // 1 if this is something the FAT driver can mount
};
int disk_scan(void);                                   // returns number of partitions found
int volume_count(void);
const struct volume *volume_get(int index);
const char *ptype_name(unsigned char type);

// install.c
void installer_init(unsigned int magic, unsigned int mbi);
int installer_command(const char *arg);

// desktop.c
void desktop_run(void);

// editor.c
int editor_run(const char *path);    // full-screen text editor; returns 0, or a negative FAT error

// cli.c
void Cli();
void print_prompt(void);
void print_disk_summary(void);

// Automatic selection of supported Ethernet controllers.
void nic_init(void);
int nic_present(void);
int nic_link(void);
const char *nic_name(void);
const unsigned char *nic_mac(void);
int nic_send(const void *, unsigned int);
int nic_receive(unsigned char *, unsigned int);
void nic_list(void);
int e1000_init(void);
int e1000_present(void);
int e1000_link(void);
const unsigned char *e1000_mac(void);
int e1000_send(const void *, unsigned int);
int e1000_receive(unsigned char *, unsigned int);
struct net_info { unsigned int ip, mask, gateway, dns, rx, tx, drop; int configured, manual; };
void net_get_info(struct net_info *info);
void net_format_ip(unsigned int ip, char *buf);

void mouse_set_bounds(int width,int height);
void graphics_begin_desktop(void);
int graphics_width(void);
int graphics_height(void);
int graphics_font_height(void);
int calc_eval(const char *expression,int *result);
void signed_string(int value,char *buf);
#endif
