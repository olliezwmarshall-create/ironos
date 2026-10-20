#include "kernel.h"

// NVMe SSD driver: polling, admin queue + one I/O queue, namespace 1, 512-byte sectors, DMA via a bounce buffer.

#define R_CAP    0x00
#define R_INTMS  0x0C
#define R_CC     0x14
#define R_CSTS   0x1C
#define R_AQA    0x24
#define R_ASQ    0x28
#define R_ACQ    0x30
#define DB_BASE  0x1000

#define QD       16                      // entries per queue
#define SPIN     20000000
#define CHUNK    16                      // sectors per command (8 KB = two PRP pages)

static volatile unsigned char *nv;
static unsigned int stride;              // doorbell stride in bytes
static unsigned char *asq, *acq, *isq, *icq, *bounce, *ident;
static unsigned int asq_tail, acq_head, acq_phase = 1;
static unsigned int isq_tail, icq_head, icq_phase = 1;
static unsigned int next_cid = 1;
static int ready = 0;
static struct disk ns;

static unsigned int rd(unsigned int off) { return mmio_read32(nv + off); }
static void wr(unsigned int off, unsigned int v) { mmio_write32(nv + off, v); }
static void doorbell(unsigned int qid, int is_cq, unsigned int value) { wr(DB_BASE + (2 * qid + (is_cq ? 1 : 0)) * stride, value); }

// Submit one command and poll for its completion. Returns the NVMe status field (0 = success), or -1 on timeout.
static int submit(int admin, unsigned int opcode, unsigned int nsid, unsigned int prp1, unsigned int prp2,
                  unsigned int cdw10, unsigned int cdw11, unsigned int cdw12) {
    unsigned char *sq = admin ? asq : isq;
    unsigned char *cq = admin ? acq : icq;
    unsigned int *tail = admin ? &asq_tail : &isq_tail;
    unsigned int *head = admin ? &acq_head : &icq_head;
    unsigned int *phase = admin ? &acq_phase : &icq_phase;
    unsigned int qid = admin ? 0 : 1;

    unsigned int *c = (unsigned int *)(sq + *tail * 64);
    memset(c, 0, 64);
    c[0] = opcode | ((next_cid++ & 0xFFFFu) << 16);
    c[1] = nsid;
    c[6] = prp1;  c[7] = 0;
    c[8] = prp2;  c[9] = 0;
    c[10] = cdw10;
    c[11] = cdw11;
    c[12] = cdw12;
    *tail = (*tail + 1) % QD;
    mem_barrier();
    doorbell(qid, 0, *tail);

    volatile unsigned int *e = (volatile unsigned int *)(cq + *head * 16);
    for (int i = 0; i < SPIN; i++) {
        if (((e[3] >> 16) & 1) == *phase) {
            int status = (int)((e[3] >> 17) & 0x7FFF);
            *head = (*head + 1) % QD;
            if (*head == 0) *phase ^= 1;
            doorbell(qid, 1, *head);
            return status;
        }
    }
    return -1;
}

static void trim(char *s, unsigned int n) {
    s[n] = '\0';
    int k = (int)n - 1;
    while (k >= 0 && (s[k] == ' ' || s[k] == '\0')) s[k--] = '\0';
    for (int i = 0; s[i]; i++) if (s[i] < 32 || s[i] > 126) s[i] = '?';
}

int nvme_init(void) {
    struct pci_loc loc;
    ready = 0;
    if (!pci_find_class(0x01, 0x08, 0x02, &loc)) return 0;

    unsigned int bar0 = pci_read32(loc.bus, loc.dev, loc.fn, 0x10);
    if (bar0 & 1) return 0;
    if (((bar0 >> 1) & 3) == 2 && pci_read32(loc.bus, loc.dev, loc.fn, 0x14) != 0) return 0;   // above 4 GB: unreachable
    nv = (volatile unsigned char *)(unsigned long)(bar0 & ~0xFu);
    if (!nv) return 0;

    unsigned int cmd = pci_read32(loc.bus, loc.dev, loc.fn, 0x04) & 0xFFFFu;
    pci_write32(loc.bus, loc.dev, loc.fn, 0x04, cmd | 0x06);           // memory space + bus master

    stride = 4u << (rd(R_CAP + 4) & 0xF);

    wr(R_INTMS, 0xFFFFFFFFu);                                          // 1. disable the controller
    if (rd(R_CC) & 1) {
        wr(R_CC, rd(R_CC) & ~1u);
        int i;
        for (i = 0; i < SPIN && (rd(R_CSTS) & 1); i++) { }
        if (i == SPIN) return 0;
    }

    asq = (unsigned char *)pmm_alloc_pages(1);                         // 2. queues
    acq = (unsigned char *)pmm_alloc_pages(1);
    isq = (unsigned char *)pmm_alloc_pages(1);
    icq = (unsigned char *)pmm_alloc_pages(1);
    bounce = (unsigned char *)pmm_alloc_pages(2);
    ident = (unsigned char *)pmm_alloc_pages(1);
    if (!asq || !acq || !isq || !icq || !bounce || !ident) return 0;
    memset(asq, 0, 4096); memset(acq, 0, 4096); memset(isq, 0, 4096); memset(icq, 0, 4096);
    asq_tail = acq_head = isq_tail = icq_head = 0;
    acq_phase = icq_phase = 1;

    wr(R_AQA, ((QD - 1) << 16) | (QD - 1));
    wr(R_ASQ, (unsigned int)(unsigned long)asq);  wr(R_ASQ + 4, 0);
    wr(R_ACQ, (unsigned int)(unsigned long)acq);  wr(R_ACQ + 4, 0);

    wr(R_CC, (6u << 16) | (4u << 20) | 1u);                            // 3. enable: 64-byte SQ entries, 16-byte CQ entries
    int i;
    for (i = 0; i < SPIN; i++) {
        unsigned int st = rd(R_CSTS);
        if (st & 2) return 0;                                          // controller fatal status
        if (st & 1) break;
    }
    if (i == SPIN) return 0;

    // 4. create the I/O completion queue, then the submission queue (queue id 1)
    if (submit(1, 0x05, 0, (unsigned int)(unsigned long)icq, 0, ((QD - 1) << 16) | 1, 1, 0) != 0) return 0;
    if (submit(1, 0x01, 0, (unsigned int)(unsigned long)isq, 0, ((QD - 1) << 16) | 1, (1u << 16) | 1, 0) != 0) return 0;

    // 5. identify the controller (model name) and namespace 1 (size, sector size)
    memset(&ns, 0, sizeof ns);
    if (submit(1, 0x06, 0, (unsigned int)(unsigned long)ident, 0, 1, 0, 0) != 0) return 0;
    memcpy(ns.model, ident + 24, 40);
    trim(ns.model, 40);

    if (submit(1, 0x06, 1, (unsigned int)(unsigned long)ident, 0, 0, 0, 0) != 0) return 0;
    unsigned long long nsze = (unsigned long long)(*(unsigned int *)ident) | ((unsigned long long)(*(unsigned int *)(ident + 4)) << 32);
    unsigned int flbas = ident[26] & 0xF;
    unsigned int lbaf = *(unsigned int *)(ident + 128 + 4 * flbas);
    unsigned int lbads = (lbaf >> 16) & 0xFF;
    if (nsze == 0 || lbads != 9) return 0;                              // only 512-byte sectors for now

    ns.kind = DISK_NVME;
    ns.unit = 0;
    ns.usable = 1;
    ns.name[0] = 'n'; ns.name[1] = 'v'; ns.name[2] = 'm'; ns.name[3] = 'e'; ns.name[4] = '0';
    const char *d = "namespace 1";
    for (int k = 0; d[k]; k++) ns.desc[k] = d[k];
    ns.sectors = nsze > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)nsze;
    ns.truncated = nsze > 0xFFFFFFFFull;
    ready = 1;
    disk_add(&ns);
    return 1;
}

static int xfer(int unit, int is_write, unsigned int lba, unsigned int count, void *buf) {
    if (unit != 0 || !ready) return -1;
    unsigned char *b = (unsigned char *)buf;
    unsigned int bounce_addr = (unsigned int)(unsigned long)bounce;

    while (count > 0) {
        unsigned int n = count > CHUNK ? CHUNK : count;
        unsigned int bytes = n * 512;
        if (is_write) memcpy(bounce, b, bytes);
        if (submit(0, is_write ? 0x01 : 0x02, 1, bounce_addr, bytes > 4096 ? bounce_addr + 4096 : 0, lba, 0, n - 1) != 0) return -1;
        if (!is_write) memcpy(b, bounce, bytes);
        b += bytes;
        lba += n;
        count -= n;
    }
    if (is_write && submit(0, 0x00, 1, 0, 0, 0, 0, 0) != 0) return -1;      // FLUSH
    return 0;
}

int nvme_read(int unit, unsigned int lba, unsigned int count, void *buf) { return xfer(unit, 0, lba, count, buf); }
int nvme_write(int unit, unsigned int lba, unsigned int count, const void *buf) { return xfer(unit, 1, lba, count, (void *)buf); }
