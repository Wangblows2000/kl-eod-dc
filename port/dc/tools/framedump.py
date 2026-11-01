#!/usr/bin/env python3
"""Extract FRAMEDUMP blocks from a serial log into PNGs, and compare renderers.

    framedump.py serial.txt outdir            # writes <tag>_<frame>.png for every dump
    framedump.py serial.txt outdir --compare sw pvr
        # for every frame dumped under both tags: per-pixel diff count, and a diff image
        # (outdir/diff_<frame>.png: left = a, middle = b, right = differing pixels in red)
"""
import base64
import os
import struct
import sys
import zlib


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def to_rgb(pix):
    out = bytearray()
    for v in pix:
        r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
        out += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))
    return out


def parse(path):
    dumps, cur = {}, None
    for line in open(path, errors="ignore"):
        line = line.strip()
        if line.startswith("FRAMEDUMP END"):
            if cur:
                tag, frame, data = cur
                try:
                    raw = base64.b64decode("".join(data))
                except Exception:
                    raw = b""
                if len(raw) >= 240 * 160 * 2:
                    dumps[(tag, frame)] = list(struct.unpack("<38400H", raw[:240 * 160 * 2]))
                else:
                    print(f"warning: dump {tag} {frame} is incomplete ({len(raw)} bytes), skipped")
            cur = None
        elif line.startswith("FRAMEDUMP "):
            _, tag, frame, _w, _h = line.split()
            cur = (tag, int(frame), [])
        elif cur is not None and line and all(ch.isalnum() or ch in "+/=" for ch in line):
            cur[2].append(line)
    return dumps


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    dumps = parse(src)
    for (tag, frame), pix in sorted(dumps.items()):
        png(f"{out}/{tag}_{frame}.png", 240, 160, to_rgb(pix))
    if "--compare" in sys.argv:
        a, b = sys.argv[sys.argv.index("--compare") + 1:][:2]
        frames = sorted({f for (t, f) in dumps if t == a} & {f for (t, f) in dumps if t == b})
        total_bad = 0
        for fr in frames:
            pa, pb = dumps[(a, fr)], dumps[(b, fr)]
            def far(x, y):
                return max(abs((x & 31) - (y & 31)), abs(((x >> 5) & 31) - ((y >> 5) & 31)),
                           abs(((x >> 10) & 31) - ((y >> 10) & 31))) > 1
            exact = sum(x != y for x, y in zip(pa, pb))
            diff = [far(x, y) for x, y in zip(pa, pb)]
            n = sum(diff)
            total_bad += n
            print(f"frame {fr}: {n} pixels differ by more than 1 level ({100 * n / 38400:.2f}%), "
                  f"{exact} not bit-identical ({100 * exact / 38400:.2f}%)")
            ra, rb = to_rgb(pa), to_rgb(pb)
            rgb = bytearray()
            for y in range(160):
                rgb += ra[y * 720:(y + 1) * 720] + rb[y * 720:(y + 1) * 720]
                for x in range(240):
                    rgb += b"\xff\x00\x00" if diff[y * 240 + x] else bytes(c // 4 for c in ra[(y * 240 + x) * 3:(y * 240 + x) * 3 + 3])
            png(f"{out}/diff_{fr}.png", 720, 160, rgb)
        print(f"compared {len(frames)} frames, {total_bad} pixels differ by more than 1 level in total")


if __name__ == "__main__":
    main()
