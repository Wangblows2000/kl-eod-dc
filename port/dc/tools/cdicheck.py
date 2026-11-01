#!/usr/bin/env python3
"""Inspect a DiscJuggler (.cdi) Dreamcast image independently of the tool that wrote it.

    cdicheck.py image.cdi [--elf klonoa-dc.elf --scramble /path/to/kos/utils/scramble/scramble
                           --objcopy sh-elf-objcopy]

Parses the CDI descriptor the way Flycast's reader does (core/deps/chdpsr/cdipsr.cpp) and checks:
  * sessions and tracks: an audio session then a data session, as a self-booting MIL-CD has;
  * the data track starts with an IP.BIN ("SEGA SEGAKATANA" hardware id) whose boot file name is
    the one the ISO holds;
  * the ISO9660 primary volume descriptor, and the boot file in the root directory;
  * with --elf: the boot file, descrambled with KallistiOS's scramble -d, is byte for byte the
    ELF's binary image (objcopy -O binary) -- the disc carries exactly the executable built.
Exit status 0 when every check passes.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

CDI_V2, CDI_V3, CDI_V35 = 0x80000004, 0x80000005, 0x80000006
SIZES = {0: 2048, 1: 2336, 2: 2352, 4: 2448}
fails = []


def check(ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        fails.append(what)
    return ok


class Reader:
    def __init__(self, f):
        self.f = f

    def u8(self):
        return self.f.read(1)[0]

    def u16(self):
        return struct.unpack("<H", self.f.read(2))[0]

    def u32(self):
        return struct.unpack("<I", self.f.read(4))[0]

    def skip(self, n):
        self.f.seek(n, 1)


def parse(path):
    size = os.path.getsize(path)
    f = open(path, "rb")
    r = Reader(f)
    f.seek(size - 8)
    version, header = r.u32(), r.u32()
    if version not in (CDI_V2, CDI_V3, CDI_V35) or not header:
        raise SystemExit(f"{path}: not a CDI image (trailer {version:08x})")
    f.seek(size - header if version == CDI_V35 else header)
    sessions = []
    position = 0
    for _ in range(r.u16()):
        tracks = []
        for _ in range(r.u16()):
            if r.u32():
                r.skip(8)
            start_mark = bytes([0, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF])
            if f.read(10) != start_mark or f.read(10) != start_mark:
                raise SystemExit("track start mark missing")
            r.skip(4)
            r.skip(r.u8())
            r.skip(19)
            if r.u32() == 0x80000000:
                r.skip(8)
            r.skip(2)
            pregap, length = r.u32(), r.u32()
            r.skip(6)
            mode = r.u32()
            r.skip(12)
            start_lba, total = r.u32(), r.u32()
            r.skip(16)
            sector_size = SIZES[r.u32()]
            r.skip(29)
            if version != CDI_V2:
                r.skip(5)
                if r.u32() == 0xFFFFFFFF:
                    r.skip(78)
            tracks.append(dict(mode=mode, pregap=pregap, length=length, start_lba=start_lba, total=total,
                               sector_size=sector_size, data=position + pregap * sector_size))
            position += total * sector_size
        r.skip(12)
        if version != CDI_V2:
            r.skip(1)
        sessions.append(tracks)
    return f, version, sessions, position, size


def user_data_offset(t):
    # 2048: cooked; 2336: mode 2 (8-byte subheader); 2352: raw mode 1 (16) or mode 2 (24)
    return {2048: 0, 2336: 8, 2352: 24 if t["mode"] == 2 else 16, 2448: 24 if t["mode"] == 2 else 16}[t["sector_size"]]


def read_sectors(f, t, lba, count):
    """Read `count` 2048-byte user-data sectors at disc LBA `lba` from data track t. The track's
    first sector after its pregap is LBA start_lba (FAD start_lba + 150), which is also what its
    ISO9660 extents count from."""
    out = bytearray()
    off = user_data_offset(t)
    for i in range(count):
        f.seek(t["data"] + (lba - t["start_lba"] + i) * t["sector_size"] + off)
        out += f.read(2048)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cdi")
    ap.add_argument("--elf")
    ap.add_argument("--scramble")
    ap.add_argument("--objcopy", default="sh-elf-objcopy")
    a = ap.parse_args()

    f, version, sessions, tracks_end, size = parse(a.cdi)
    names = {CDI_V2: "2.0", CDI_V3: "3.0", CDI_V35: "3.5"}
    print(f"{a.cdi}: {size} bytes, DiscJuggler CDI v{names[version]}")
    for i, s in enumerate(sessions, 1):
        for t in s:
            kind = {0: "audio", 1: "mode 1", 2: "mode 2"}.get(t["mode"], "?")
            print(f"  session {i}: {kind} track, {t['sector_size']}-byte sectors, start LBA {t['start_lba']}, "
                  f"pregap {t['pregap']}, {t['length']} sectors")
    check(tracks_end <= size, "track data lies inside the file")
    check(len(sessions) == 2 and sessions[0] and sessions[0][0]["mode"] == 0, "session 1 is an audio track (MIL-CD layout)")
    data = sessions[-1][-1] if sessions and sessions[-1] else None
    if not check(data is not None and data["mode"] in (1, 2), "the last session holds a data track"):
        return 1
    first = data["start_lba"]

    ip = read_sectors(f, data, first, 16)
    check(ip[:16] == b"SEGA SEGAKATANA ", "IP.BIN hardware id 'SEGA SEGAKATANA'")
    boot_name = ip[0x60:0x70].decode("ascii", "replace").strip()
    title = ip[0x80:0x100].decode("ascii", "replace").strip()
    print(f"  IP.BIN: product '{ip[0x40:0x4A].decode('ascii', 'replace').strip()}', boot file '{boot_name}', "
          f"title '{title}'")

    pvd = read_sectors(f, data, first + 16, 1)
    if not check(pvd[0] == 1 and pvd[1:6] == b"CD001", "ISO9660 primary volume descriptor"):
        return 1
    root = pvd[156:156 + 34]
    root_lba, root_len = struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0]
    rootdir = read_sectors(f, data, root_lba, (root_len + 2047) // 2048)
    files = {}
    i = 0
    while i < len(rootdir) and rootdir[i]:
        rec = rootdir[i:i + rootdir[i]]
        name = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0]
        if name not in ("\x00", "\x01"):
            files[name] = (struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0])
        i += rootdir[i]
    print(f"  root directory: {', '.join(f'{n} ({files[n][1]} bytes)' for n in sorted(files))}")
    if not check(boot_name in files, f"boot file {boot_name} is in the root directory"):
        return 1
    lba, length = files[boot_name]
    boot = read_sectors(f, data, lba, (length + 2047) // 2048)[:length]

    if a.elf:
        with tempfile.TemporaryDirectory() as d:
            scr, plain, ref = (os.path.join(d, n) for n in ("1st_read.bin", "descrambled.bin", "elf.bin"))
            open(scr, "wb").write(boot)
            subprocess.run([a.scramble, "-d", scr, plain], check=True)
            subprocess.run([a.objcopy, "-O", "binary", a.elf, ref], check=True)
            same = open(plain, "rb").read() == open(ref, "rb").read()
            check(same, f"descrambled {boot_name} == objcopy -O binary of {os.path.basename(a.elf)} "
                        f"({os.path.getsize(ref)} bytes)")
    print("all checks passed" if not fails else f"{len(fails)} check(s) failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
