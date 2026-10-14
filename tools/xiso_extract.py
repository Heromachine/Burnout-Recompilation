#!/usr/bin/env python3
"""Extract an Xbox (XDVDFS) disc image to a directory.

The launcher's first step: the player points it at their own Burnout ISO
and this writes default.xbe and the game data next to the recompilation
project. Nothing from the disc is shipped; this reads the user's image.

XDVDFS in brief (offsets are relative to the start of the game partition):
  sector 32 (0x10000)  volume descriptor: "MICROSOFT*XBOX*MEDIA", then the
                       root directory's sector (u32) and size (u32)
  directory            a binary tree of entries packed into sectors:
                         u16 left, u16 right  (subtree offsets, in dwords;
                                              0 = none, 0xFFFF = sector pad)
                         u32 sector, u32 size, u8 attributes, u8 name_len,
                         name, padded to a 4-byte boundary
                       attribute 0x10 marks a directory
The game partition starts at offset 0 in a trimmed "xiso" image, or after a
video partition in a full redump image (XGD1 0x18300000, XGD2 0x0FD90000,
XGD3 0x02080000).

Usage: xiso_extract.py <image.iso> <output dir> [--list]
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise ValueError("not an Xbox disc image (no XDVDFS volume descriptor found)")


def read_dir(f, base, sector, size):
    """Yield (name, sector, size, is_dir) for every entry in one directory."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, esec, esize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue                    # sector padding, not an entry
        name = data[off + 14:off + 14 + nlen].decode("latin-1")
        yield name, esec, esize, bool(attr & ATTR_DIRECTORY)
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def walk(f, base, sector, size, prefix=""):
    for name, esec, esize, is_dir in read_dir(f, base, sector, size):
        path = prefix + name
        if is_dir:
            yield path, None, None, True
            yield from walk(f, base, esec, esize, path + "/")
        else:
            yield path, esec, esize, False


def extract(image, out_dir, progress=None, list_only=False, only=None):
    """only: a set of image paths ("default.xbe"); everything else is skipped."""
    with open(image, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = list(walk(f, base, root_sector, root_size))
        if only is not None:
            entries = [e for e in entries if not e[3] and e[0] in only]
        total = sum(e[2] for e in entries if not e[3]) or 1
        done = 0
        for path, esec, esize, is_dir in entries:
            if ".." in path.split("/"):
                raise ValueError(f"unsafe path in image: {path!r}")
            if list_only:
                print(("D " if is_dir else "F ") + path + ("" if is_dir else f"  {esize}"))
                continue
            dest = os.path.join(out_dir, *path.split("/"))
            if is_dir:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            f.seek(base + esec * SECTOR)
            remaining = esize
            with open(dest, "wb") as out:
                while remaining:
                    chunk = f.read(min(remaining, 8 << 20))
                    if not chunk:
                        raise IOError(f"image ends inside {path}")
                    out.write(chunk)
                    remaining -= len(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, total, path)
        return base, len(entries), total


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    list_only = "--list" in argv
    last = [0]

    def progress(done, total, path):
        pct = done * 100 // total
        if pct != last[0]:
            last[0] = pct
            print(f"PROGRESS {pct} {path}", flush=True)

    base, count, total = extract(argv[1], argv[2], progress, list_only)
    print(f"DONE partition_offset=0x{base:X} entries={count} bytes={total}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
