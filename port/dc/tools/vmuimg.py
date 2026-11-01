#!/usr/bin/env python3
"""Inspect and prepare flycast VMU images (vmu_save_A1.bin: 256 blocks of 512 bytes) for save tests.

    vmuimg.py list IMG                     files, sizes, free blocks
    vmuimg.py fill IMG LEAVE               add a filler file so only LEAVE user blocks stay free
    vmuimg.py corrupt IMG NAME [OFFSET]    flip one byte of file NAME (default: inside its payload)
    vmuimg.py record IMG NAME              print the save record header inside NAME (hal_save_core.h)

VMU file system: block 255 root (FAT at the block in word 0x46, directory at 0x4A for 0x4C blocks,
user blocks 0..0x50), FAT = one little-endian u16 per block (0xFFFC free, 0xFFFA last in chain),
32-byte directory entries (type 0x33 data / 0xCC game, first block, 12-byte name, size in blocks).
"""
import struct
import sys

BLK = 512


def load(path):
    return bytearray(open(path, "rb").read())


def root(img):
    r = img[255 * BLK:256 * BLK]
    fat_blk, fat_n, dir_blk, dir_n = struct.unpack_from("<HHHH", r, 0x46)
    user = struct.unpack_from("<H", r, 0x50)[0]
    return fat_blk, dir_blk, dir_n, user


def fat(img):
    fb = root(img)[0]
    return list(struct.unpack_from("<256H", img, fb * BLK))


def entries(img):
    _, db, dn, _ = root(img)
    out = []
    for b in range(db, db - dn, -1):
        for k in range(16):
            off = b * BLK + k * 32
            e = img[off:off + 32]
            if e[0] in (0x33, 0xCC):
                out.append((off, e[0], struct.unpack_from("<H", e, 2)[0], e[4:16].rstrip(b"\0").decode("ascii", "replace"),
                            struct.unpack_from("<H", e, 0x18)[0]))
    return out


def chain(img, first):
    f, out = fat(img), []
    b = first
    while b != 0xFFFA and len(out) < 256:
        out.append(b)
        b = f[b]
    return out


def free_blocks(img):
    f, user = fat(img), root(img)[3]
    return sum(1 for b in range(user) if f[b] == 0xFFFC)


def cmd_list(img):
    for off, t, first, name, size in entries(img):
        print(f"  {name:12s} {'game' if t == 0xCC else 'data'} {size} blocks, first block {first}")
    print(f"  free user blocks: {free_blocks(img)}")


def cmd_fill(path, leave):
    img = load(path)
    f = fat(img)
    user = root(img)[3]
    free = [b for b in range(user - 1, -1, -1) if f[b] == 0xFFFC]
    use = free[:max(0, len(free) - leave)]
    if not use:
        raise SystemExit("nothing to fill")
    for a, b in zip(use, use[1:]):
        f[a] = b
    f[use[-1]] = 0xFFFA
    struct.pack_into("<256H", img, root(img)[0] * BLK, *f)
    _, db, dn, _ = root(img)
    for b in range(db, db - dn, -1):
        for k in range(16):
            off = b * BLK + k * 32
            if img[off] not in (0x33, 0xCC):
                e = bytearray(32)
                e[0], e[1] = 0x33, 0
                struct.pack_into("<H", e, 2, use[0])
                e[4:16] = b"FILLER______"
                e[16:24] = bytes([0x20, 0x26, 0x11, 0x01, 0x12, 0x00, 0x00, 0x06])
                struct.pack_into("<HH", e, 0x18, len(use), 0)
                img[off:off + 32] = e
                for blk in use:
                    img[blk * BLK:(blk + 1) * BLK] = bytes([0xA5]) * BLK
                open(path, "wb").write(img)
                print(f"{path}: filler file of {len(use)} blocks; {free_blocks(img)} user blocks free")
                return
    raise SystemExit("directory full")


def file_bytes(img, name):
    for off, t, first, n, size in entries(img):
        if n == name:
            return chain(img, first)
    raise SystemExit(f"no file {name}")


def cmd_corrupt(path, name, offset):
    img = load(path)
    blocks = file_bytes(img, name)
    # payload: VMS header 128 + icon 512 = 640; record header 32; data byte 100 by default
    offset = 640 + 32 + 100 if offset is None else offset
    b = blocks[offset // BLK]
    img[b * BLK + offset % BLK] ^= 0xFF
    open(path, "wb").write(img)
    print(f"{path}: flipped byte {offset} of {name} (block {b})")


def cmd_record(path, name):
    img = load(path)
    data = b"".join(bytes(img[b * BLK:(b + 1) * BLK]) for b in file_bytes(img, name))
    rec = data[640:640 + 32]
    magic, ver, hb, db, seq, dcrc, hcrc = struct.unpack_from("<8sHHIIII", rec)
    print(f"{name}: magic {magic!r} version {ver} header {hb} data {db} seq {seq} data_crc {dcrc:08x}")


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "list":
        cmd_list(load(path))
    elif cmd == "fill":
        cmd_fill(path, int(sys.argv[3]))
    elif cmd == "corrupt":
        cmd_corrupt(path, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else None)
    elif cmd == "record":
        cmd_record(path, sys.argv[3])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
