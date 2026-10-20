#!/usr/bin/env python3
"""Independent FAT12/16/32 consistency checker + manifest printer (written from the spec, not from the kernel code)."""
import sys, struct, zlib

ILLEGAL = set(b'"*+,/:;<=>?[\\]|')

def fail(msg): errors.append(msg)
errors = []

def main(path):
    data = open(path, "rb").read()
    base = 0
    def looks_bpb(o):
        s = data[o:o+512]
        return len(s) == 512 and s[510:512] == b"\x55\xAA" and struct.unpack_from("<H", s, 11)[0] == 512 and s[13] and (s[13] & (s[13]-1)) == 0 and s[16] in (1, 2)
    if not looks_bpb(0):
        for i in range(4):
            e = data[446+i*16:462+i*16]
            if e[4] in (1, 4, 6, 0xB, 0xC, 0xE):
                base = struct.unpack_from("<I", e, 8)[0] * 512
                break
    if not looks_bpb(base):
        print("NOT FAT"); return 2
    b = data[base:base+512]
    bps, spc, rsv, nfats, rootent, tot16, media, fatsz16 = struct.unpack_from("<HBHBHHBH", b, 11)
    tot32, = struct.unpack_from("<I", b, 32)
    fatsz32, _, _, rootclus, fsinfo_sec = struct.unpack_from("<IHHIH", b, 36)
    total = tot16 or tot32
    fatsz = fatsz16 or fatsz32
    rootsecs = (rootent * 32 + 511) // 512
    dataoff = rsv + nfats * fatsz + rootsecs
    nclust = (total - dataoff) // spc
    bits = 12 if nclust < 4085 else 16 if nclust < 65525 else 32
    cb = spc * 512
    def sec(n): return base + n * 512
    fats = [data[sec(rsv + i*fatsz): sec(rsv + (i+1)*fatsz)] for i in range(nfats)]
    for i in range(1, nfats):
        if fats[i] != fats[0]:
            # report the first differing entry
            n = next(k for k in range(len(fats[0])) if fats[0][k] != fats[i][k])
            fail("FAT copy %d differs from copy 0 at byte %d" % (i, n))
    fat = fats[0]
    def entry(c):
        if bits == 12:
            o = c + c // 2
            v = fat[o] | (fat[o+1] << 8)
            return v >> 4 if c & 1 else v & 0xFFF
        if bits == 16: return struct.unpack_from("<H", fat, c*2)[0]
        return struct.unpack_from("<I", fat, c*4)[0] & 0x0FFFFFFF
    eoc_min = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[bits]
    def cl_off(c): return sec(dataoff) + (c-2) * cb
    visited = {}
    def chain(start, what):
        out, c, seen = [], start, set()
        while True:
            if c < 2 or c >= nclust + 2:
                fail("%s: chain hits invalid cluster %d" % (what, c)); break
            if c in seen: fail("%s: chain loops at %d" % (what, c)); break
            if c in visited: fail("%s: cluster %d cross-linked with %s" % (what, c, visited[c])); break
            seen.add(c); out.append(c); visited[c] = what
            n = entry(c)
            if n >= eoc_min: break
            if n == 0: fail("%s: chain runs into a free cluster after %d" % (what, c)); break
            if n == 0x0FF7 and bits == 12 or n == 0xFFF7 and bits == 16 or n == 0x0FFFFFF7 and bits == 32: fail("%s: chain hits a bad cluster" % what); break
            c = n
        return out
    def dir_bytes(start, what):
        if start == 0 and bits != 32:
            return data[sec(rsv + nfats*fatsz): sec(rsv + nfats*fatsz + rootsecs)]
        cl = chain(start, what)
        return b"".join(data[cl_off(c): cl_off(c)+cb] for c in cl)
    manifest = []
    def csum(n11):
        s = 0
        for x in n11: s = (((s & 1) << 7) + (s >> 1) + x) & 0xFF
        return s
    def walk(start, path, parent_ref, is_root):
        raw = dir_bytes(start, "dir " + (path or "/"))
        names_l, names_s = set(), set()
        lfn = None
        seen = 0
        for o in range(0, len(raw), 32):
            e = raw[o:o+32]
            if e[0] == 0x00:
                if any(raw[o:]): fail("%s: non-zero data after end-of-directory marker at entry %d" % (path or "/", o//32))
                break
            if e[0] == 0xE5:
                if lfn: fail("%s: orphan LFN entries before a deleted entry" % (path or "/"))
                lfn = None; continue
            attr = e[11]
            if attr == 0x0F:
                order = e[0]
                if order & 0x40:
                    if lfn: fail("%s: LFN sequence interrupted" % (path or "/"))
                    lfn = {"n": order & 0x1F, "next": (order & 0x1F), "sum": e[13], "chars": {}}
                elif not lfn or (order & 0x1F) != lfn["next"]:
                    fail("%s: LFN order broken at entry %d" % (path or "/", o//32)); lfn = None; continue
                if lfn["next"] != (order & 0x1F): fail("%s: LFN order broken" % (path or "/"))
                if e[13] != lfn["sum"]: fail("%s: LFN checksum differs between fragments" % (path or "/"))
                k = (order & 0x1F) - 1
                units = [struct.unpack_from("<H", e, x)[0] for x in (1,3,5,7,9,14,16,18,20,22,24,28,30)]
                for i, u in enumerate(units): lfn["chars"][k*13+i] = u
                lfn["next"] -= 1
                continue
            if attr & 0x08: fail("unexpected volume label entry"); lfn = None; continue
            n11 = e[0:11]
            longname = None
            if lfn:
                if lfn["next"] != 0: fail("%s: LFN sequence incomplete for %r" % (path or "/", n11))
                if lfn["sum"] != csum(n11): fail("%s: LFN checksum mismatch for %r" % (path or "/", n11))
                us = []
                for i in range(lfn["n"]*13):
                    u = lfn["chars"].get(i)
                    if u in (0, 0xFFFF): break
                    us.append(u)
                longname = "".join(chr(u) for u in us)
            lfn = None
            base_s = n11[:8].rstrip(b" ").decode("latin1"); ext_s = n11[8:].rstrip(b" ").decode("latin1")
            nt = e[12]
            if base_s in (".", ".."):
                sname = base_s
            else:
                for ch in n11:
                    if ch in ILLEGAL or ch < 0x20 or (0x61 <= ch <= 0x7A): fail("%s: illegal char %r in short name %r" % (path or "/", chr(ch), n11)); break
                bs = base_s.lower() if nt & 0x08 else base_s
                es = ext_s.lower() if nt & 0x10 else ext_s
                sname = bs + ("." + es if ext_s else "")
            name = longname if longname else sname
            hi = struct.unpack_from("<H", e, 20)[0] if bits == 32 else 0
            lo = struct.unpack_from("<H", e, 26)[0]
            clus = (hi << 16) | lo
            size = struct.unpack_from("<I", e, 28)[0]
            full = path + "/" + name
            isdir = bool(attr & 0x10)
            if not is_root and seen == 0 and name != ".": fail("%s: first entry is not '.'" % path)
            if not is_root and seen == 1 and name != "..": fail("%s: second entry is not '..'" % path)
            if is_root and name in (".", ".."): fail("root directory contains %s" % name)
            seen += 1
            if name in (".", ".."):
                if not isdir: fail("%s: %s is not a directory" % (path, name))
                if name == "." and clus != start: fail("%s: '.' points to %d, expected %d" % (path, clus, start))
                if name == ".." and clus != parent_ref: fail("%s: '..' points to %d, expected %d" % (path, clus, parent_ref))
                continue
            for coll, key in ((names_l, name.lower()), (names_s, n11)):
                if key in coll: fail("%s: duplicate name %r" % (path or "/", name))
                coll.add(key)
            if isdir:
                manifest.append("D " + full)
                if size != 0: fail("%s: directory with size %d" % (full, size))
                if clus == 0: fail("%s: directory with cluster 0" % full)
                else: walk(clus, full, 0 if is_root else start, False)
            else:
                cl = []
                if size == 0:
                    if clus != 0: fail("%s: empty file owns cluster %d" % (full, clus))
                    content = b""
                else:
                    if clus == 0: fail("%s: non-empty file with cluster 0" % full)
                    cl = chain(clus, "file " + full) if clus else []
                    need = -(-size // cb)
                    if len(cl) != need: fail("%s: size %d needs %d clusters, chain has %d" % (full, size, need, len(cl)))
                    content = b"".join(data[cl_off(c): cl_off(c)+cb] for c in cl)[:size]
                manifest.append("F %s %d %08x" % (full, size, zlib.crc32(content) & 0xFFFFFFFF))
        if lfn: fail("%s: dangling LFN entries at end of directory" % (path or "/"))
    if bits == 32:
        walk(rootclus, "", 0, True)
    else:
        walk(0, "", 0, True)
    # every non-free cluster must be reachable; no reachable cluster may be free
    lost = [c for c in range(2, nclust + 2) if entry(c) != 0 and c not in visited]
    if lost: fail("%d lost (allocated but unreferenced) clusters, e.g. %s" % (len(lost), lost[:8]))
    free = sum(1 for c in range(2, nclust + 2) if entry(c) == 0)
    if bits == 32 and fsinfo_sec not in (0, 0xFFFF):
        fi = data[sec(fsinfo_sec):sec(fsinfo_sec)+512]
        fc, nf = struct.unpack_from("<II", fi, 488)
        if fc not in (0xFFFFFFFF, free): fail("FSInfo free count %d is wrong (actual %d)" % (fc, free))
    if "--manifest" in sys.argv:
        print("\n".join(sorted(manifest)))
    if "--free" in sys.argv:
        print("FREE", free)
    for e in errors: print("FSCK ERROR:", e)
    if not errors and "--quiet" not in sys.argv: print("FSCK OK (FAT%d, %d files/dirs, %d free clusters)" % (bits, len(manifest), free), file=sys.stderr)
    return 1 if errors else 0

sys.exit(main(sys.argv[1]))
