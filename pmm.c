#include "kernel.h"

#define PAGE_SIZE   4096
#define MAX_PAGES   0x100000            // 4 GB worth of 4 KB pages (32-bit address space)
#define MAX_REGIONS 32
#define ADDR_LIMIT  0x100000000ULL

// Multiboot info structure (only the fields we need)
struct multiboot_info {
    unsigned int flags;
    unsigned int mem_lower;             // KB below 1 MB
    unsigned int mem_upper;             // KB above 1 MB
    unsigned int boot_device;
    unsigned int cmdline;
    unsigned int mods_count;
    unsigned int mods_addr;
    unsigned int syms[4];
    unsigned int mmap_length;
    unsigned int mmap_addr;
} __attribute__((packed));

struct mmap_entry {
    unsigned int size;                  // size of the rest of this entry (not counting this field)
    unsigned long long addr;
    unsigned long long len;
    unsigned int type;
} __attribute__((packed));

static unsigned int bitmap[MAX_PAGES / 32];   // bit set = page in use / not usable
static struct mem_region regions[MAX_REGIONS];
static int region_count = 0;
static unsigned int total_pages = 0;          // usable pages reported by the bootloader
static unsigned int free_pages = 0;           // usable pages not yet handed out

static inline int page_used(unsigned int p) { return bitmap[p >> 5] & (1u << (p & 31)); }
static inline void page_set(unsigned int p) { bitmap[p >> 5] |= (1u << (p & 31)); }
static inline void page_clear(unsigned int p) { bitmap[p >> 5] &= ~(1u << (p & 31)); }

// Mark [addr, addr+len) as in use (rounding outward to whole pages).
static void reserve_range(unsigned int addr, unsigned int len) {
    unsigned int first = addr >> 12;
    unsigned int last = (addr + len + PAGE_SIZE - 1) >> 12;
    if (last > MAX_PAGES) last = MAX_PAGES;
    for (unsigned int p = first; p < last; p++) {
        if (!page_used(p)) { page_set(p); free_pages--; }
    }
}

unsigned int pmm_init(unsigned int mbi) {
    struct multiboot_info *mb = (struct multiboot_info *)mbi;

    for (unsigned int i = 0; i < MAX_PAGES / 32; i++) bitmap[i] = 0xFFFFFFFF;
    region_count = 0;
    total_pages = 0;
    free_pages = 0;

    // 1. Copy the bootloader's memory map into our own table.
    if (mb->flags & (1 << 6)) {
        unsigned int p = mb->mmap_addr;
        unsigned int end = p + mb->mmap_length;
        while (p < end && region_count < MAX_REGIONS) {
            struct mmap_entry *e = (struct mmap_entry *)p;
            regions[region_count].base = e->addr;
            regions[region_count].len  = e->len;
            regions[region_count].type = e->type;
            region_count++;
            p += e->size + 4;
        }
    } else if (mb->flags & 1) {
        // Old-style fallback: only lower/upper memory sizes
        regions[0].base = 0;        regions[0].len = (unsigned long long)mb->mem_lower * 1024; regions[0].type = 1;
        regions[1].base = 0x100000; regions[1].len = (unsigned long long)mb->mem_upper * 1024; regions[1].type = 1;
        region_count = 2;
    }

    // 2. Free every whole page inside usable regions (clamped to 4 GB).
    for (int i = 0; i < region_count; i++) {
        if (regions[i].type != 1) continue;
        unsigned long long start = regions[i].base;
        unsigned long long end = start + regions[i].len;
        if (start >= ADDR_LIMIT) continue;
        if (end > ADDR_LIMIT) end = ADDR_LIMIT;
        start = (start + PAGE_SIZE - 1) & ~(unsigned long long)(PAGE_SIZE - 1);
        end &= ~(unsigned long long)(PAGE_SIZE - 1);
        if (end <= start) continue;

        unsigned int sp = (unsigned int)(start >> 12);
        unsigned int ep = (unsigned int)(end >> 12);
        for (unsigned int p = sp; p < ep; p++) {
            page_clear(p);
            free_pages++;
        }
        total_pages += ep - sp;
    }

    // 3. Reserve what is already in use: everything below the end of the kernel
    //    (page 0, BIOS/VGA area, the kernel image), plus the multiboot info itself.
    reserve_range(0, (unsigned int)kernel_end);
    reserve_range(mbi, sizeof(struct multiboot_info));
    if (mb->flags & (1 << 6)) reserve_range(mb->mmap_addr, mb->mmap_length);

    return total_pages;
}

// Find `count` contiguous free pages (first fit). Returns the address, or 0 on failure.
void *pmm_alloc_pages(unsigned int count) {
    if (count == 0) return 0;
    unsigned int run = 0, start = 0;

    for (unsigned int p = 0; p < MAX_PAGES; p++) {
        if ((p & 31) == 0 && bitmap[p >> 5] == 0xFFFFFFFF) {   // whole word in use: skip it
            p += 31;
            run = 0;
            continue;
        }
        if (page_used(p)) { run = 0; continue; }

        if (run == 0) start = p;
        run++;
        if (run == count) {
            for (unsigned int q = start; q < start + count; q++) page_set(q);
            free_pages -= count;
            return (void *)(start << 12);
        }
    }
    return 0;
}

void pmm_free_pages(void *addr, unsigned int count) {
    unsigned int first = (unsigned int)addr >> 12;
    for (unsigned int p = first; p < first + count && p < MAX_PAGES; p++) {
        if (page_used(p)) { page_clear(p); free_pages++; }
    }
}

unsigned int pmm_total_pages(void) { return total_pages; }
unsigned int pmm_free_page_count(void) { return free_pages; }
int mem_region_count(void) { return region_count; }
const struct mem_region *mem_region_get(int index) {
    if (index < 0 || index >= region_count) return 0;
    return &regions[index];
}
