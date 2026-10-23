#!/usr/bin/env python3
# fatread.py: unpack a FAT12/16/32 image (an SD card such as Project+'s Wii/sd.raw) into a folder,
# with the Python standard library alone, so make-bundle.py needs no 7-Zip on any computer
# (ORCA.md "Mods (phase 1)"). Read-only: the image is never written. Long file names are used when
# their checksum matches the short entry, as 7-Zip and every FAT driver do; otherwise the 8.3 name,
# lower-cased where the entry's NT case bits say so. Deleted entries, volume labels, "." and ".."
# are skipped. Paths that would leave the output folder are refused.
#
#   fatread.py <image> <out folder>
import os
import struct
import sys


class FatImage:
    def __init__(self, path):
        self.f = open(path, "rb")
        bs = self.f.read(512)
        if len(bs) < 512 or bs[510:512] != b"\x55\xaa":
            raise ValueError(f"{path}: no FAT boot sector")
        self.bps = struct.unpack_from("<H", bs, 11)[0]
        self.spc = bs[13]
        reserved = struct.unpack_from("<H", bs, 14)[0]
        fats = bs[16]
        root_entries = struct.unpack_from("<H", bs, 17)[0]
        total = struct.unpack_from("<H", bs, 19)[0] or struct.unpack_from("<I", bs, 32)[0]
        fat_sectors = struct.unpack_from("<H", bs, 22)[0] or struct.unpack_from("<I", bs, 36)[0]
        if self.bps not in (512, 1024, 2048, 4096) or self.spc == 0 or fats == 0 or fat_sectors == 0:
            raise ValueError(f"{path}: not a FAT volume this reader knows")
        root_sectors = (root_entries * 32 + self.bps - 1) // self.bps
        self.root_start = (reserved + fats * fat_sectors) * self.bps
        self.root_bytes = root_sectors * self.bps
        self.data_start = self.root_start + self.root_bytes
        clusters = (total - reserved - fats * fat_sectors - root_sectors) // self.spc
        self.type = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
        self.root_cluster = struct.unpack_from("<I", bs, 44)[0] if self.type == 32 else None
        self.cluster_bytes = self.bps * self.spc
        self.clusters = clusters
        self.f.seek(reserved * self.bps)
        self.fat = self.f.read(fat_sectors * self.bps)

    def next_cluster(self, c):
        if self.type == 32:
            n = struct.unpack_from("<I", self.fat, c * 4)[0] & 0x0FFFFFFF
            return None if n >= 0x0FFFFFF8 else n
        if self.type == 16:
            n = struct.unpack_from("<H", self.fat, c * 2)[0]
            return None if n >= 0xFFF8 else n
        v = struct.unpack_from("<H", self.fat, c + c // 2)[0]
        n = v >> 4 if c & 1 else v & 0xFFF
        return None if n >= 0xFF8 else n

    def chain(self, first):
        seen = set()
        c = first
        while c is not None:
            if c < 2 or c >= self.clusters + 2 or c in seen:
                raise ValueError(f"bad cluster chain at {c}")
            seen.add(c)
            yield c
            c = self.next_cluster(c)

    def read_chain(self, first, size=None):
        if first == 0:
            return b""
        out = bytearray()
        for c in self.chain(first):
            self.f.seek(self.data_start + (c - 2) * self.cluster_bytes)
            out += self.f.read(self.cluster_bytes)
            if size is not None and len(out) >= size:
                break
        if size is not None:
            if len(out) < size:
                raise ValueError(f"chain from {first} is shorter than {size} bytes")
            del out[size:]
        return bytes(out)

    def root(self):
        if self.type == 32:
            return self.read_chain(self.root_cluster)
        self.f.seek(self.root_start)
        return self.f.read(self.root_bytes)

    @staticmethod
    def entries(data):
        lfn = {}
        lfn_sum = None
        for off in range(0, len(data) - 31, 32):
            e = data[off : off + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn, lfn_sum = {}, None
                continue
            attr = e[11]
            if attr == 0x0F:
                seq = e[0]
                if seq & 0x40:
                    lfn, lfn_sum = {}, e[13]
                part = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le", "surrogatepass")
                lfn[seq & 0x1F] = part
                continue
            if attr & 0x08:
                lfn, lfn_sum = {}, None
                continue
            short = bytes([0xE5 if e[0] == 0x05 else e[0]]) + e[1:11]
            checksum = 0
            for b in short:
                checksum = (((checksum & 1) << 7) + (checksum >> 1) + b) & 0xFF
            name = None
            if lfn and lfn_sum == checksum and sorted(lfn) == list(range(1, len(lfn) + 1)):
                full = "".join(lfn[i] for i in sorted(lfn))
                name = full.split("\x00", 1)[0]
            if not name:
                base = short[:8].decode("cp437").rstrip(" ")
                ext = short[8:11].decode("cp437").rstrip(" ")
                if e[12] & 0x08:
                    base = base.lower()
                if e[12] & 0x10:
                    ext = ext.lower()
                name = base + ("." + ext if ext else "")
            lfn, lfn_sum = {}, None
            if name in (".", ".."):
                continue
            first = (struct.unpack_from("<H", e, 20)[0] << 16) | struct.unpack_from("<H", e, 26)[0]
            if attr & 0x10:
                yield name, True, first, 0
            else:
                yield name, False, first, struct.unpack_from("<I", e, 28)[0]


def extract(image, out):
    fat = FatImage(image)
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    count = 0
    stack = [(fat.root(), out)]
    while stack:
        data, folder = stack.pop()
        for name, is_dir, first, size in FatImage.entries(data):
            if not name or "/" in name or "\\" in name or name in (".", ".."):
                raise ValueError(f"unsafe name {name!r} in {folder}")
            dest = os.path.join(folder, name)
            if os.path.commonpath([out, os.path.abspath(dest)]) != out:
                raise ValueError(f"{dest} leaves {out}")
            if is_dir:
                os.makedirs(dest, exist_ok=True)
                stack.append((fat.read_chain(first), dest))
            else:
                with open(dest, "wb") as w:
                    w.write(fat.read_chain(first, size))
                count += 1
    return count


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: fatread.py <image> <out folder>")
    n = extract(sys.argv[1], sys.argv[2])
    print(f"{n} files")
