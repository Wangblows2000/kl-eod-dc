#!/usr/bin/env python3
"""Inspect and prepare flycast VMU images (vmu_save_A1.bin: 256 blocks of 512 bytes) for save tests.

    vmuimg.py list IMG                     files, sizes, free blocks
    vmuimg.py fill IMG LEAVE               add a filler file so only LEAVE user blocks stay free
    vmuimg.py corrupt IMG NAME [OFFSET]    flip one byte of file NAME (default: inside its payload)
    vmuimg.py record IMG NAME              print the save record header inside NAME (hal_save_core.h)
    vmuimg.py vision IMG WORLD LEVEL       test saves: make the newest save's last-used game file
                                           continue to WORLD's vision select with visions 1..LEVEL-1
                                           of that world cleared, so LEVEL can be chosen there

VMU file system: block 255 root (FAT at the block in word 0x46, directory at 0x4A for 0x4C blocks,
user blocks 0..0x50), FAT = one little-endian u16 per block (0xFFFC free, 0xFFFA last in chain),
32-byte directory entries (type 0x33 data / 0xCC game, first block, 12-byte name, size in blocks).
"""
import struct
import sys
import zlib

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


def crc16_ccitt(data):
    """KallistiOS net_crc16ccitt (start 0): the VMS header CRC that vmu_pkg_parse checks."""
    rv = 0
    for x in data:
        rv ^= x << 8
        for _ in range(8):
            rv = ((rv << 1) ^ 0x1021) & 0xFFFF if rv & 0x8000 else (rv << 1) & 0xFFFF
    return rv


def game_checksums(rec, n):
    """The game's own add and xor checksums over rec[:n] (kleod src/save.c)."""
    a = x = 0
    for b in rec[:n]:
        a, x = (a + b) & 0xFF, x ^ b
    return a, x


def cmd_vision(path, world, level):
    """Rewrite the EEPROM image inside the newest of KLONOAEOD_A/B. The scene record (SceneSaveData,
    EEPROM dwords base+1..5) becomes world WORLD, level LEVEL, scene type vision (1), so "continue"
    on the file select goes to WORLD's vision select; levelInfo[WORLD-1][0..LEVEL-2] in the file
    record (FileSaveData, dwords base+6..14) get the "beaten" flag (0x80), which is what lets the
    vision select reach LEVEL. Both records get the game's checksums again, the save record its
    CRC-32s (hal_save_core.c) and the VMS file its CRC-16. Everything else is the game's own data."""
    img = load(path)
    best = None
    for name in ("KLONOAEOD_A", "KLONOAEOD_B"):
        if not any(n == name for _, _, _, n, _ in entries(img)):
            continue
        blocks = file_bytes(img, name)
        data = bytearray(b"".join(bytes(img[b * BLK:(b + 1) * BLK]) for b in blocks))
        seq = struct.unpack_from("<I", data, 640 + 16)[0]
        if best is None or seq > best[2]:
            best = (name, blocks, seq, data)
    if best is None:
        raise SystemExit(f"{path}: no Klonoa save")
    name, blocks, seq, data = best
    dlen = struct.unpack_from("<I", data, 0x48)[0]
    chk = bytearray(data)
    chk[0x46:0x48] = b"\0\0"
    if crc16_ccitt(chk[:128 + 512 + dlen]) != struct.unpack_from("<H", data, 0x46)[0]:
        raise SystemExit(f"{name}: VMS CRC does not match; not a file this port wrote")
    rec = 640
    ee = bytearray(data[rec + 32:rec + 32 + 512])
    if zlib.crc32(ee) != struct.unpack_from("<I", data, rec + 20)[0]:
        raise SystemExit(f"{name}: record data CRC does not match")
    base = ee[0x30 * 8] * 0x10  # last-used game file
    scene = bytearray(ee[(base + 1) * 8:(base + 6) * 8])
    scene[1], scene[2], scene[3] = world, level, 1
    scene[0x20], scene[0x21] = game_checksums(scene, 0x20)
    ee[(base + 1) * 8:(base + 6) * 8] = scene
    frec = bytearray(ee[(base + 6) * 8:(base + 15) * 8])
    for lv in range(level - 1):
        frec[8 + (world - 1) * 8 + lv] |= 0x80
    frec[0x3C], frec[0x3D] = game_checksums(frec, 0x3C)
    ee[(base + 6) * 8:(base + 15) * 8] = frec
    data[rec + 32:rec + 32 + 512] = ee
    struct.pack_into("<I", data, rec + 20, zlib.crc32(ee))
    struct.pack_into("<I", data, rec + 24, zlib.crc32(bytes(data[rec:rec + 24])))
    data[0x46:0x48] = b"\0\0"
    struct.pack_into("<H", data, 0x46, crc16_ccitt(data[:128 + 512 + dlen]))
    for i, b in enumerate(blocks):
        img[b * BLK:(b + 1) * BLK] = data[i * BLK:(i + 1) * BLK]
    open(path, "wb").write(img)
    print(f"{path}: {name} (seq {seq}), last-used game file (slot {base // 0x10}): continues at world {world} vision select, "
          f"visions {world}-1..{world}-{level - 1} cleared")


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
    elif cmd == "vision":
        cmd_vision(path, int(sys.argv[3]), int(sys.argv[4]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
