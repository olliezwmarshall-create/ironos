#include "kernel.h"

int top_rendered_line = 1;
int bottom_rendered_line = 25;
char terminal_history[MAX_HISTORY_LINES][VGA_WIDTH];
int total_lines_written = 0;
int cursor_row = 1;
int cursor_col = 0;
unsigned char current_color = 0x0F;
char cmd_buffer[80];
int buf_index = 0;
volatile unsigned long uptime_ticks = 0;

void kernel_main(unsigned int magic, unsigned int mbi) {
    gdt_init();
    interrupts_init();
    timer_init(TIMER_HZ);
    keyboard_init();
    mouse_init();

    // 0x2BADB002 means a Multiboot loader (GRUB) gave us a valid info struct
    unsigned int usable_pages = 0;
    if (magic == 0x2BADB002)
        usable_pages = pmm_init(mbi);
    installer_init(magic, mbi);
    heap_init();

    nic_init();

    ata_init();      // find disks: IDE, then SATA (AHCI), then NVMe; then their partitions.
    ahci_init();     // (the first FAT volume gets mounted read-only by the CLI)
    nvme_init();
    disk_scan();

    clear_screen();
    print_banner();
    if (usable_pages == 0) {
        print_string("WARNING: no memory map from bootloader");
        newline();
    }
    print_disk_summary();
    print_string("Type desktop for the desktop, or help for commands.");
    newline();
    print_prompt();

    sti();   // interrupts on: timer + keyboard start firing
    Cli();
}
