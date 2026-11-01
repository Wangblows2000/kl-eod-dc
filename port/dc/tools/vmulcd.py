#!/usr/bin/env python3
"""Check a VMU LCD test run (docs/vmu-lcd.md) and draw what the VMU showed.

    vmulcd.py serial.txt [--capture lcd.txt] [--rom baserom.gba] [--map klonoa-dc.map]
              [--out DIR] [--expect-states 0,1,2,...] [--expect-moo]

serial.txt is the run's serial log from a VMULCD_LOG=1 build: "VMULCD f=..." for every new
image, "VMULCDSEND ..." for every write attempt, "VMULCD mode=..." at every gameplay/menu switch.
--capture is flycast's FC_VMULCD file: the raw bytes each VMU actually received.

Checks (exit status 1 if any fails):
  transport   every successful write arrived, in order, on its port, and decodes (with the
              180-degree turn when the log says rot=1) to exactly the logged image
  conversion  every logged image is recomputed from the ROM (an independent per-pixel decode of
              the 32x32 4bpp frame, the palette threshold, placement and mirroring)
  timing      every animation frame lasted the timer the ROM's frame list gives it, unless the
              game itself changed state; no image reached a VMU later than the send interval
              allows unless the VMU was busy
  coverage    the Klonoa states named by --expect-states and the Moo's walk and look-around
              were all shown, and both menu->gameplay and gameplay->menu switches happened
--out writes a PNG strip of every distinct image in order and a sheet of one image per state.
"""
import argparse
import re
import struct
import sys

IMG = re.compile(r"^VMULCD f=(\d+) gen=(\d+) mode=(\w+) src=([0-9a-f]+) pal=([0-9a-f]+) x=(\d+) flip=(\d) "
                 r"state=(\d+) frame=(\d+) lag=(\d+) img=([0-9a-f]{384})$")
SEND = re.compile(r"^VMULCDSEND f=(\d+) gen=(\d+) slot=(\d+) port=(\w+) rot=(\d) rv=(-?\d+)$")
MODE = re.compile(r"^VMULCD mode=(\w+) frame=(\d+) scene=([0-9a-f]+) level=(\d+)$")
STATS = re.compile(r"^VMULCD stats .*$")
MIN_INTERVAL = 2
MAPLE_EAGAIN = -2  # vmu_draw_lcd: the VMU's maple frame is locked (a save writing to it); try again
INK_THRESHOLD = 100
KLONOA_ANIMS = 0x0805553C
MOO_FRAMES = 0x081898E0

# Klonoa's animation states in the normal-level table, named from where the game sets them
# (kleod src/code_0800D188.c, code_08014184.c); approximate where the code is not yet understood.
KLONOA_STATES = {
    0: "idle", 1: "run", 2: "jump", 3: "jump apex", 4: "fall", 5: "land", 6: "spring bounce",
    7: "flutter (float)", 8: "wind bullet (grab)", 9: "throw (ground)", 10: "throw (air)", 11: "double jump",
    12: "damage", 13: "hurt", 14: "hold Goomi", 15: "goal", 16: "hang", 17: "hang (up/down)", 18: "climb",
    19: "climb (up)", 20: "climb (down)", 21: "game over", 22: "carry: idle", 23: "carry: run",
    24: "carry: jump", 25: "carry: jump apex", 26: "carry: fall", 27: "carry: land",
    28: "idle animation", 29: "idle animation", 30: "idle animation", 31: "idle animation",
    32: "idle animation", 33: "idle animation", 34: "door / ladder", 35: "door / ladder", 36: "door / ladder",
    37: "enter door",
}
MOO_STATES = {0: "Moo look-around", 1: "Moo walk"}


class Rom:
    def __init__(self, path):
        self.b = open(path, "rb").read()

    def u8(self, a):
        return self.b[a - 0x08000000]

    def u16(self, a):
        return struct.unpack_from("<H", self.b, a - 0x08000000)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.b, a - 0x08000000)[0]

    def in_rom(self, a, n=1):
        return 0x08000000 <= a and a + n <= 0x08000000 + len(self.b)

    def ink(self, pal):
        out = []
        for c in range(16):
            v = self.u16(pal + 2 * c)
            luma = (0.299 * (v & 31) + 0.587 * ((v >> 5) & 31) + 0.114 * ((v >> 10) & 31)) / 31 * 255
            out.append(c != 0 and luma < INK_THRESHOLD - 1e-9)
        return out

    def image(self, src, pal, x, flip):
        """The 48x32 picture (list of rows of 0/1) of a frame, decoded pixel by pixel."""
        ink = self.ink(pal)
        rows = [[0] * 48 for _ in range(32)]
        for y in range(32):
            for sx in range(32):
                tile = (y // 8) * 4 + sx // 8
                b = self.u8(src + tile * 32 + (y % 8) * 4 + (sx % 8) // 2)
                c = b >> 4 if sx & 1 else b & 15
                if ink[c]:
                    rows[y][x + (31 - sx if flip else sx)] = 1
        return rows

    def timer(self, frames, state, frame):
        return self.u8(self.u32(frames + 4 * state) + 8 * frame + 4)

    def next_in_list(self, frames, state, frame):
        """(state, frame) the game moves to after this one, following -1 and goto entries."""
        lst = self.u32(frames + 4 * state)
        nxt = self.u32(lst + 8 * (frame + 1))
        if nxt == 0xFFFFFFFF:
            return state, 0
        if nxt < 10000:
            return nxt, 0
        if nxt >= 0xFFFFFFF0:
            return None
        return state, frame + 1


def rows_of(hexstr):
    b = bytes.fromhex(hexstr)
    return [[(b[y * 6 + x // 8] >> (7 - x % 8)) & 1 for x in range(48)] for y in range(32)]


def unrotate(raw):
    """Inverse of KOS vmu_draw_lcd_rotated(): the bitmap's bit stream reversed."""
    bits = int.from_bytes(raw, "big")
    rev = int(bin(bits)[2:].zfill(len(raw) * 8)[::-1], 2)
    return rev.to_bytes(len(raw), "big")


def png(path, images, labels=None, scale=4, cols=8):
    from PIL import Image, ImageDraw
    w, h, gut, lab = 48, 32, 3, 10 if labels else 0
    rows = (len(images) + cols - 1) // cols
    im = Image.new("RGB", (cols * (w + gut) * scale, rows * (h + gut + lab) * scale), (90, 90, 90))
    d = ImageDraw.Draw(im)
    for i, img in enumerate(images):
        ox, oy = (i % cols) * (w + gut) * scale, (i // cols) * (h + gut + lab) * scale
        d.rectangle([ox, oy, ox + w * scale - 1, oy + h * scale - 1], fill=(170, 190, 160))
        for y in range(h):
            for x in range(w):
                if img[y][x]:
                    d.rectangle([ox + x * scale, oy + y * scale, ox + (x + 1) * scale - 1, oy + (y + 1) * scale - 1],
                                fill=(30, 40, 60))
        if labels:
            d.text((ox + 2, oy + h * scale + 2), labels[i], fill=(255, 255, 255))
    im.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("serial")
    ap.add_argument("--capture")
    ap.add_argument("--rom")
    ap.add_argument("--map")
    ap.add_argument("--out")
    ap.add_argument("--expect-states", default="")
    ap.add_argument("--expect-moo", action="store_true")
    a = ap.parse_args()

    images, sends, modes, scenes = [], [], [], []
    for line in open(a.serial, errors="replace"):
        line = line.rstrip("\n")
        if m := IMG.match(line):
            images.append(dict(f=int(m[1]), gen=int(m[2]), mode=m[3], src=int(m[4], 16), pal=int(m[5], 16),
                               x=int(m[6]), flip=int(m[7]), state=int(m[8]), frame=int(m[9]), lag=int(m[10]),
                               img=m[11]))
        elif m := SEND.match(line):
            sends.append(dict(f=int(m[1]), gen=int(m[2]), slot=int(m[3]), port=m[4], rot=int(m[5]), rv=int(m[6])))
        elif m := MODE.match(line):
            modes.append(dict(mode=m[1], f=int(m[2]), scene=int(m[3], 16), level=int(m[4])))
        elif m := re.match(r"^SCENE frame=(\d+) handler=([0-9a-f]+)", line):
            scenes.append((int(m[1]), int(m[2], 16)))
        elif STATS.match(line):
            print(line)
    fails = []
    names = {}
    if a.map:
        for m in re.finditer(r"^\s+0x([0-9a-f]{8,16})\s+([A-Za-z_]\w*)\s*$", open(a.map).read(), re.M):
            names[int(m[1], 16)] = m[2]
    by_gen = {i["gen"]: i for i in images}
    print(f"{len(images)} images, {len(sends)} write attempts "
          f"({sum(s['rv'] == 0 for s in sends)} ok, {sum(s['rv'] == MAPLE_EAGAIN for s in sends)} busy), "
          f"{len(modes)} mode switches")
    for m in modes:
        print(f"  frame {m['f']:5d}: {m['mode']:8s} (scene {names.get(m['scene'], hex(m['scene']))}, level {m['level']})")

    # ---- transport ----
    if a.capture:
        cap = {}
        for line in open(a.capture):
            seq, port, hexdata = line.split()
            cap.setdefault(port, []).append(bytes.fromhex(hexdata))
        used, bad = {}, 0
        for s in sends:
            if s["rv"] != 0:
                continue
            k = used.get(s["port"], 0)
            used[s["port"]] = k + 1
            got = cap.get(s["port"], [])
            if k >= len(got):
                bad += 1
                continue
            raw = got[k]
            fb = unrotate(raw) if s["rot"] else raw
            if fb.hex() != by_gen[s["gen"]]["img"]:
                bad += 1
        extra = sum(len(v) for v in cap.values()) - sum(used.values())
        print(f"transport: {sum(used.values())} writes compared with what the VMUs received "
              f"({', '.join(f'{p}: {len(v)}' for p, v in sorted(cap.items()))}); {bad} differ, {extra} unexpected")
        if bad or extra:
            fails.append("transport")

    # ---- conversion ----
    if a.rom:
        rom = Rom(a.rom)
        bad = 0
        for i in images:
            if not rom.in_rom(i["src"], 0x200):
                continue
            if rom.image(i["src"], i["pal"], i["x"], i["flip"]) != rows_of(i["img"]):
                bad += 1
        print(f"conversion: {len(images)} images recomputed from the ROM, {bad} differ")
        if bad:
            fails.append("conversion")

        # ---- timing: each (state, frame) lasted its ROM timer ----
        # Walk each unbroken stretch of one mode; within it, collapse image changes that are not
        # animation changes (the Moo moving), and compare each animation frame's life with its
        # ROM timer plus the frames in which the game skipped its animation update (lag=).
        checked = bad = 0
        runs, cur = [], []
        for i in images:
            if cur and cur[-1]["mode"] != i["mode"]:
                runs.append(cur)
                cur = []
            cur.append(i)
        runs.append(cur)
        for run in runs:
            frames = rom.u32(KLONOA_ANIMS) if run[0]["mode"] == "gameplay" else MOO_FRAMES
            anim, lag = [], 0
            for i in run:
                lag += i["lag"]
                if not anim or (anim[-1]["state"], anim[-1]["frame"]) != (i["state"], i["frame"]):
                    anim.append(dict(i, lag=lag))
                    lag = 0
            for p, n in zip(anim, anim[1:]):
                if rom.next_in_list(frames, p["state"], p["frame"]) != (n["state"], n["frame"]):
                    continue  # the game (or the Moo's patrol) changed state
                checked += 1
                t = rom.timer(frames, p["state"], p["frame"])
                if p["mode"] == "menu":
                    n["lag"] = 0  # the Moo is the port's own player: it never skips a frame
                if n["f"] - p["f"] != t + n["lag"]:
                    bad += 1
                    if bad <= 5:
                        print(f"  {p['mode']} state {p['state']} frame {p['frame']} at {p['f']}: lasted "
                              f"{n['f'] - p['f']}, ROM says {t} (+{n['lag']} lag frames)")
        print(f"timing: {checked} frame durations compared with the ROM timers, {bad} differ")
        if bad:
            fails.append("timing")

    late = busy_late = 0
    first_ok = {}
    for s in sends:
        if s["rv"] == 0:
            first_ok.setdefault((s["port"], s["gen"]), s["f"])
    # A VMU busy with a save (the save writer holds its maple frame for each block) answers
    # MAPLE_EAGAIN: the picture waits, and one the game replaces meanwhile is never sent -- the save
    # comes first. Those count as held by a busy VMU, not lost; the first picture after the save
    # must still arrive on time (it is checked like any other).
    busy = {(s["port"], s["gen"]) for s in sends if s["rv"] == MAPLE_EAGAIN}
    held = 0
    ports = sorted({s["port"] for s in sends})
    for i, img in enumerate(images):
        for p in ports:
            nxt = images[i + 1]["f"] if i + 1 < len(images) else None
            if (p, img["gen"]) not in first_ok:
                if nxt is not None and nxt - img["f"] < MIN_INTERVAL:
                    continue  # replaced before it was due: skipped, never stale
                if nxt is None and img["f"] > images[-1]["f"] - MIN_INTERVAL:
                    continue  # the run stopped before it was due
                if (p, img["gen"]) in busy:
                    held += 1  # every attempt found the VMU busy, then a newer picture replaced it
                    continue
                late += 1
                if late <= 5:
                    print(f"  image {img['gen']} (frame {img['f']}) never reached {p}")
            elif first_ok[(p, img["gen"])] - img["f"] > MIN_INTERVAL:
                if (p, img["gen"]) in busy:
                    busy_late += 1
                else:
                    late += 1
    skipped = sum(1 for i, img in enumerate(images) for p in ports if (p, img["gen"]) not in first_ok)
    print(f"delivery: {len(images) * len(ports) - skipped} image deliveries, {skipped - held} skipped (replaced within "
          f"{MIN_INTERVAL} frames), {late} late or lost, {busy_late} delayed and {held} replaced while a VMU was busy")
    if late:
        fails.append("delivery")

    # ---- coverage ----
    kl = {}
    moo = {}
    for i in images:
        (kl if i["mode"] == "gameplay" else moo).setdefault(i["state"], i)
    print("Klonoa states shown: " + ", ".join(f"{s} {KLONOA_STATES.get(s, '?')}" for s in sorted(kl)))
    print("Moo states shown: " + ", ".join(f"{s} {MOO_STATES.get(s, '?')}" for s in sorted(moo)))
    flips = {(i["mode"], i["flip"]) for i in images}
    print(f"facing: {sorted(flips)}")
    want = [int(s, 0) for s in a.expect_states.split(",") if s]
    missing = [s for s in want if s not in kl]
    if missing:
        print(f"  missing Klonoa states: {missing}")
        fails.append("coverage")
    if a.expect_moo and not {0, 1} <= set(moo):
        fails.append("coverage (Moo)")
    seqm = [m["mode"] for m in modes]
    if a.expect_states and not any(x == "menu" and y == "gameplay" for x, y in zip(seqm, seqm[1:])):
        fails.append("coverage (menu -> gameplay)")
    if a.expect_states and not any(x == "gameplay" and y == "menu" for x, y in zip(seqm, seqm[1:])):
        fails.append("coverage (gameplay -> menu)")

    if a.out:
        import os
        os.makedirs(a.out, exist_ok=True)
        png(os.path.join(a.out, "sequence.png"), [rows_of(i["img"]) for i in images[:400]],
            [str(i["f"]) for i in images[:400]], scale=3, cols=12)
        reps = [kl[s] for s in sorted(kl)] + [moo[s] for s in sorted(moo)]
        labels = [f"{s} {KLONOA_STATES.get(s, '?')}" for s in sorted(kl)] + [MOO_STATES.get(s, "?") for s in sorted(moo)]
        png(os.path.join(a.out, "states.png"), [rows_of(i["img"]) for i in reps], labels, scale=4, cols=6)
        print(f"wrote {a.out}/sequence.png, {a.out}/states.png")

    print("FAIL: " + ", ".join(fails) if fails else "all checks passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
