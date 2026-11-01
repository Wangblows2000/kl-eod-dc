#!/usr/bin/env python3
"""Check the presented picture's geometry from a GEOMPROBE build's 640x480 frame.

    geomcheck.py frame.bin [--png out.png]

frame.bin is the raw RGB565 frame a GEOMPROBE=N build hands to flycast's FC_RAMDUMP hook. In that
build both renderers show a 240x160 test card in place of the game -- red where the game x is odd,
green where the game y is odd, blue everywhere -- through their normal presentation path.

Checks: the picture is 3:2 (640 x 426 rows of the 426.7), centred (27 black rows above and below),
fills the width, every screen pixel shows the game pixel under its centre for a uniform 8/3 scale
(so game columns and rows are 3,3,2 screen pixels, never stretched in one direction only), and
the bars are black apart from the status-line band. Exit status 0 when all pass.
"""
import struct
import sys
import zlib

W, H = 640, 480
GW, GH = 240, 160
SCALE = 8 / 3
Y0 = 80 / 3
TEXT_Y, TEXT_ROWS = 458, 16
fails = []


def check(ok, what):
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        fails.append(what)


def png(path, pix):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            p = pix[y * W + x]
            r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
            raw += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def runs(bits):
    out, n = [], 1
    for a, b in zip(bits, bits[1:]):
        if a == b:
            n += 1
        else:
            out.append(n)
            n = 1
    out.append(n)
    return out


def main():
    data = open(sys.argv[1], "rb").read()
    if len(data) != W * H * 2:
        raise SystemExit(f"{sys.argv[1]}: {len(data)} bytes, expected {W * H * 2}")
    pix = struct.unpack(f"<{W * H}H", data)
    if "--png" in sys.argv:
        png(sys.argv[sys.argv.index("--png") + 1], pix)

    rows = [y for y in range(H) if not TEXT_Y <= y < TEXT_Y + TEXT_ROWS
            and any(pix[y * W + x] & 31 for x in range(W))]  # blue: the card (status line excluded)
    top, bottom = rows[0], rows[-1]
    height = bottom - top + 1
    cols = [x for x in range(W) if any(pix[y * W + x] & 31 for y in range(top, bottom + 1))]
    left, right = cols[0], cols[-1]
    width = right - left + 1
    print(f"{sys.argv[1]}: picture rows {top}-{bottom} ({height}), columns {left}-{right} ({width}); "
          f"bars {top} rows above, {H - 1 - bottom} below; aspect {width / height:.4f}")
    check(width == W and left == 0, "fills the width (640 columns)")
    check(height == 426 and top == 27 and bottom == 452, "426 rows, 27..452 (3:2 at 640 wide is 426.7)")
    check(top == H - 1 - bottom, "centred vertically")
    check(abs(width / (GW * SCALE) - height / (GH * SCALE)) < 0.003, "same scale both ways (3:2 kept)")
    check(all(pix[y * W + x] & 31 for y in range(top, bottom + 1) for x in range(W)),
          "every pixel inside the picture shows the card")

    xr = runs([1 if (pix[240 * W + x] >> 11) & 31 else 0 for x in range(W)])
    yr = runs([1 if (pix[y * W + 320] >> 5) & 63 else 0 for y in range(top, bottom + 1)])
    print(f"  game columns: {len(xr)}, screen widths {sorted(set(xr))}; game rows: {len(yr)}, heights "
          f"{sorted(set(yr))}")
    check(len(xr) == GW and set(xr) <= {2, 3}, "240 game columns, each 2 or 3 screen pixels wide")
    check(len(yr) == GH and set(yr) <= {2, 3}, "160 game rows, each 2 or 3 screen pixels tall")
    check(all(sum(xr[i:i + 3]) == 8 for i in range(0, GW - 2, 3)), "columns repeat 3,3,2 (8 pixels per 3)")
    bad = 0
    for y in range(top, bottom + 1):
        gy = int((y + 0.5 - Y0) / SCALE)
        for x in range(W):
            gx = int((x + 0.5) / SCALE)
            p = pix[y * W + x]
            if (1 if (p >> 11) & 31 else 0) != (gx & 1) or (1 if (p >> 5) & 63 else 0) != (gy & 1):
                bad += 1
    check(bad == 0, f"every pixel is the game pixel under its centre at scale 8/3 ({bad} differ)")
    nonblack = sum(1 for y in range(H) if not top <= y <= bottom and not TEXT_Y <= y < TEXT_Y + TEXT_ROWS
                   for x in range(W) if pix[y * W + x])
    check(nonblack == 0, f"bars black outside the status line ({nonblack} non-black pixels)")
    print("all checks passed" if not fails else f"{len(fails)} check(s) failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
