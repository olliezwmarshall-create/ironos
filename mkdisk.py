#!/usr/bin/env python3
"""Build a FAT12/16/32 disk image from a directory tree. Needs only Python 3 (no mkfs/mtools).

usage: mkdisk.py OUTPUT.img SOURCE_DIR [--fat 12|16|32] [--size MB] [--spc N] [--mbr] [--scatter]
"""
import argparse, os, random, struct, sys

SECTOR = 512
VALID = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~!(){}^#&")


def checksum(name11):
    s = 0
    for b in name11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def is_83(name):
    if name in (".", ".."):
        return False
    base, ext = (name.rsplit(".", 1) + [""])[:2] if "." in name else (name, "")
    if not base or len(base) > 8 or len(ext) > 3 or name.count(".") > 1:
        return False
    if not all(c.upper() in VALID for c in base + ext):
        return False
    for part in (base, ext):
        letters = [c for c in part if c.isalpha()]
        if letters and not (all(c.isupper() for c in letters) or all(c.islower() for c in letters)):
            return False
    return True


def gen_short(name, used):
    """-> (short11 bytes, nt_flags, needs_lfn)"""
    if is_83(name):
        base, ext = name.rsplit(".", 1) if "." in name else (name, "")
        short = (base.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii")
        if short not in used:
            nt = 0
            if any(c.islower() for c in base): nt |= 0x08
            if any(c.islower() for c in ext): nt |= 0x10
            used.add(short)
            return short, nt, False
    base, ext = name.rsplit(".", 1) if "." in name else (name, "")
    clean = lambda s: "".join(c for c in s.upper() if c in VALID)
    base, ext = clean(base) or "FILE", clean(ext)[:3]
    i = 1
    while True:
        tail = "~%d" % i
        short = (base[: 8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
        short = short.encode("ascii")
        if short not in used:
            used.add(short)
            return short, 0, True
        i += 1


def short_entry(short11, attr, nt, cluster, size):
    return struct.pack("<11sBBBHHHHHHHI", short11, attr, nt, 0, 0, 0x21, 0x21,
                       (cluster >> 16) & 0xFFFF, 0, 0x21, cluster & 0xFFFF, size)


def lfn_entries(name, short11):
    units = list(name.encode("utf-16-le"))
    units = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
    if len(units) % 13:
        units.append(0)
        while len(units) % 13:
            units.append(0xFFFF)
    pieces = len(units) // 13
    offs = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    out = []
    for k in range(pieces, 0, -1):
        b = bytearray(32)
        b[0] = k | (0x40 if k == pieces else 0)
        b[11] = 0x0F
        b[13] = checksum(short11)
        for i, u in enumerate(units[(k - 1) * 13: k * 13]):
            struct.pack_into("<H", b, offs[i], u)
        out.append(bytes(b))
    return out


class Volume:
    def __init__(self, bits, size_bytes, spc, scatter, label, hidden):
        self.bits, self.spc, self.hidden = bits, spc, hidden
        self.total = size_bytes // SECTOR
        self.reserved = 32 if bits == 32 else 1
        self.nfats = 2
        self.root_entries = 0 if bits == 32 else 512
        self.root_secs = (self.root_entries * 32 + 511) // 512
        fatsz = 1
        while True:
            n = (self.total - self.reserved - self.nfats * fatsz - self.root_secs) // spc
            need = (((n + 2) * bits + 7) // 8 + 511) // 512
            if need <= fatsz:
                break
            fatsz = need
        self.fatsz, self.nclusters = fatsz, n
        ok = {12: n < 4085, 16: 4085 <= n < 65525, 32: n >= 65525}[bits]
        if not ok:
            sys.exit("FAT%d needs a different --size/--spc (this layout has %d clusters)" % (bits, n))
        self.cb = spc * SECTOR
        self.fat = [0] * (n + 2)
        self.eoc = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[bits]
        self.fat[0] = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[bits]
        self.fat[1] = self.eoc
        self.data = bytearray(n * self.cb)
        self.free = list(range(2, n + 2))
        if scatter:
            random.Random(1234).shuffle(self.free)     # fragment files so chain-following is really tested
        self.label = label
        self.root_fixed = bytearray(self.root_entries * 32)
        self.root_cluster = 0

    def alloc(self, count):
        if count > len(self.free):
            sys.exit("disk image is too small for the files given")
        chain = [self.free.pop(0) for _ in range(count)]
        for a, b in zip(chain, chain[1:]):
            self.fat[a] = b
        self.fat[chain[-1]] = self.eoc
        return chain

    def write_chain(self, chain, payload):
        for i, c in enumerate(chain):
            chunk = payload[i * self.cb:(i + 1) * self.cb]
            self.data[(c - 2) * self.cb:(c - 2) * self.cb + len(chunk)] = chunk

    @staticmethod
    def plan(hostdir):
        used, items = set(), []
        for name in sorted(os.listdir(hostdir)):
            short, nt, lfn = gen_short(name, used)
            items.append((name, short, nt, lfn))
        return items

    def count_entries(self, hostdir, is_root):
        n = 0 if is_root else 2
        for name, short, nt, lfn in self.plan(hostdir):
            n += 1 + (len(lfn_entries(name, short)) if lfn else 0)
        return n

    def write_dir(self, hostdir, chain, parent_ref, is_root):
        ents = []
        if not is_root:
            ents.append(short_entry(b".".ljust(11), 0x10, 0, chain[0], 0))
            ents.append(short_entry(b"..".ljust(11), 0x10, 0, parent_ref, 0))
        for name, short, nt, lfn in self.plan(hostdir):
            path = os.path.join(hostdir, name)
            if lfn:
                ents.extend(lfn_entries(name, short))
            if os.path.isdir(path):
                n = self.count_entries(path, False)
                sub = self.alloc(max(1, -(-n * 32 // self.cb)))
                own = 0 if is_root else chain[0]
                self.write_dir(path, sub, own, False)
                ents.append(short_entry(short, 0x10, nt, sub[0], 0))
            else:
                payload = open(path, "rb").read()
                first = 0
                if payload:
                    fc = self.alloc(-(-len(payload) // self.cb))
                    self.write_chain(fc, payload)
                    first = fc[0]
                ents.append(short_entry(short, 0x20, nt, first, len(payload)))
        blob = b"".join(ents)
        if chain is None:
            if len(blob) > len(self.root_fixed):
                sys.exit("too many entries in the root directory (max %d)" % self.root_entries)
            self.root_fixed[:len(blob)] = blob
        else:
            self.write_chain(chain, blob)

    def build(self, srcdir):
        if self.bits == 32:
            n = self.count_entries(srcdir, True)
            chain = self.alloc(max(1, -(-n * 32 // self.cb)))
            self.root_cluster = chain[0]
            self.write_dir(srcdir, chain, 0, True)
        else:
            self.write_dir(srcdir, None, 0, True)
        return self.serialize()

    def fat_bytes(self):
        b = bytearray(self.fatsz * SECTOR)
        for i, v in enumerate(self.fat):
            if self.bits == 12:
                o = i + i // 2
                if i % 2 == 0:
                    b[o] = v & 0xFF
                    b[o + 1] = (b[o + 1] & 0xF0) | (v >> 8)
                else:
                    b[o] = (b[o] & 0x0F) | ((v & 0xF) << 4)
                    b[o + 1] = v >> 4
            elif self.bits == 16:
                struct.pack_into("<H", b, i * 2, v)
            else:
                struct.pack_into("<I", b, i * 4, v)
        return bytes(b)

    def boot_sector(self):
        b = bytearray(SECTOR)
        b[0:3] = b"\xEB\x3C\x90"
        b[3:11] = b"IRONOS  "
        struct.pack_into("<HBHBH", b, 11, SECTOR, self.spc, self.reserved, self.nfats, self.root_entries)
        t16 = self.total if (self.total < 65536 and self.bits != 32) else 0
        struct.pack_into("<HBHHHI", b, 19, t16, 0xF8, self.fatsz if self.bits != 32 else 0, 63, 255, self.hidden)
        struct.pack_into("<I", b, 32, self.total if t16 == 0 else 0)
        label = self.label.ljust(11).encode("ascii")[:11]
        if self.bits == 32:
            struct.pack_into("<IHHIHH", b, 36, self.fatsz, 0, 0, self.root_cluster, 1, 6)
            b[64] = 0x80; b[66] = 0x29
            struct.pack_into("<I", b, 67, 0x1234ABCD)
            b[71:82] = label
            b[82:90] = b"FAT32   "
        else:
            b[36] = 0x80; b[38] = 0x29
            struct.pack_into("<I", b, 39, 0x1234ABCD)
            b[43:54] = label
            b[54:62] = b"FAT%d   " % self.bits
        b[510:512] = b"\x55\xAA"
        return bytes(b)

    def serialize(self):
        out = bytearray(self.total * SECTOR)
        boot = self.boot_sector()
        out[0:SECTOR] = boot
        if self.bits == 32:
            fsinfo = bytearray(SECTOR)
            struct.pack_into("<I", fsinfo, 0, 0x41615252)
            struct.pack_into("<III", fsinfo, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
            struct.pack_into("<I", fsinfo, 508, 0xAA550000)
            out[SECTOR:2 * SECTOR] = fsinfo
            out[6 * SECTOR:7 * SECTOR] = boot
        fb = self.fat_bytes()
        for i in range(self.nfats):
            o = (self.reserved + i * self.fatsz) * SECTOR
            out[o:o + len(fb)] = fb
        ro = (self.reserved + self.nfats * self.fatsz) * SECTOR
        out[ro:ro + len(self.root_fixed)] = self.root_fixed
        do = ro + self.root_secs * SECTOR
        out[do:do + len(self.data)] = self.data
        return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("output")
    ap.add_argument("source")
    ap.add_argument("--fat", type=int, choices=(12, 16, 32), default=16)
    ap.add_argument("--size", type=float, default=64, help="volume size in MB")
    ap.add_argument("--spc", type=int, default=4, help="sectors per cluster")
    ap.add_argument("--mbr", action="store_true", help="wrap the volume in an MBR partition table")
    ap.add_argument("--scatter", action="store_true", help="fragment files across the disk (for testing)")
    ap.add_argument("--label", default="IRONOS")
    a = ap.parse_args()

    hidden = 2048 if a.mbr else 0
    vol = Volume(a.fat, int(a.size * 1024 * 1024), a.spc, a.scatter, a.label, hidden)
    image = vol.build(a.source)

    if a.mbr:
        mbr = bytearray(SECTOR)
        ptype = {12: 0x01, 16: 0x0E, 32: 0x0C}[a.fat]
        mbr[446:462] = struct.pack("<B3sB3sII", 0x80, b"\xFE\xFF\xFF", ptype, b"\xFE\xFF\xFF", hidden, vol.total)
        mbr[510:512] = b"\x55\xAA"
        image = bytes(mbr) + bytes(hidden * SECTOR - SECTOR) + image

    with open(a.output, "wb") as f:
        f.write(image)
    print("%s: FAT%d, %d clusters of %d bytes%s" % (a.output, a.fat, vol.nclusters, vol.cb, ", MBR partitioned" if a.mbr else ""))


main()
