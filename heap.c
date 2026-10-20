#include "kernel.h"

#define HEAP_MAGIC 0xC0DEB10C
#define MIN_SPLIT  16

// Every allocation is preceded by one of these. Blocks form a linked list in address order.
struct block {
    unsigned int size;        // usable bytes after this header
    unsigned int free;
    unsigned int magic;
    struct block *next;
};

static struct block *heap_start = 0;
static unsigned int heap_bytes = 0;

// Grab one contiguous chunk of physical memory (physical == virtual for now).
// Tries 16 MB and halves until it fits. The heap does not grow after this.
void heap_init(void) {
    unsigned int pages = 4096;
    void *mem = 0;
    while (pages >= 64 && !(mem = pmm_alloc_pages(pages)))
        pages /= 2;
    if (!mem) return;

    heap_start = (struct block *)mem;
    heap_bytes = pages * 4096;
    heap_start->size = heap_bytes - sizeof(struct block);
    heap_start->free = 1;
    heap_start->magic = HEAP_MAGIC;
    heap_start->next = 0;
}

// Not interrupt-safe: only call from normal kernel code, never from an IRQ handler.
void *kmalloc(unsigned int size) {
    if (!heap_start || size == 0) return 0;
    size = (size + 15) & ~15u;

    for (struct block *b = heap_start; b; b = b->next) {
        if (!b->free || b->size < size) continue;

        // Split if enough is left over for another block
        if (b->size >= size + sizeof(struct block) + MIN_SPLIT) {
            struct block *n = (struct block *)((char *)(b + 1) + size);
            n->size = b->size - size - sizeof(struct block);
            n->free = 1;
            n->magic = HEAP_MAGIC;
            n->next = b->next;
            b->size = size;
            b->next = n;
        }
        b->free = 0;
        return b + 1;
    }
    return 0;
}

void kfree(void *ptr) {
    if (!ptr || !heap_start) return;
    if ((char *)ptr < (char *)heap_start + sizeof(struct block) ||
        (char *)ptr >= (char *)heap_start + heap_bytes) return;

    struct block *b = (struct block *)ptr - 1;
    if (b->magic != HEAP_MAGIC || b->free) return;   // bad pointer or double free
    b->free = 1;

    // Merge neighbouring free blocks
    for (struct block *c = heap_start; c; c = c->next) {
        while (c->free && c->next && c->next->free) {
            c->size += sizeof(struct block) + c->next->size;
            c->next = c->next->next;
        }
    }
}

void heap_stats(unsigned int *total, unsigned int *used, unsigned int *free_bytes) {
    unsigned int u = 0, f = 0;
    for (struct block *b = heap_start; b; b = b->next) {
        if (b->free) f += b->size; else u += b->size;
    }
    *total = heap_bytes;
    *used = u;
    *free_bytes = f;
}
