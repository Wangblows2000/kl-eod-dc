#!/usr/bin/env python3
"""Differential test of hal_m4a.c against the original MP2K assembly in the GBA ROM.

Both run on an emulated ARM CPU (Unicorn) over the same GBA memory image:

  reference  the cartridge's own code: m4aSoundVSync, SoundMain/SoundMainRAM, MP2KPlayerMain and
             the track commands, exactly as the GBA executes them;
  port       hal_m4a.c compiled for ARM with arm-none-eabi-gcc, called in their place (the jump
             table, plynote and the player chain are pointed at the compiled functions).

Everything else (the engine's C half in kleod's m4a.c, the song data, CgbSound) is the ROM's own
code in both runs. After every frame the script compares the mixed pcmBuffer, the channel and
track state, and the PSG registers; any difference is a translation error.

    difftest.py --rom baserom.gba --elf kleod.elf [--songs 0-164] [--frames 600] [--mix]

Needs: arm-none-eabi-gcc, and the Python module unicorn (pip install unicorn).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_SP, UC_ARM_REG_R0, UC_ARM_REG_R1, \
    UC_ARM_REG_R2, UC_ARM_REG_CPSR

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src")
RET = 0x0A000000
PORT_BASE = 0x09000000

REPLACED = ["SoundMain", "m4aSoundVSync", "MP2KPlayerMain", "TrackStop", "MP2K_event_nxx", "MP2K_event_endtie",
            "MP2K_event_goto", "MP2K_event_patt", "MP2K_event_pend", "MP2K_event_rept", "MP2K_event_prio",
            "MP2K_event_tempo", "MP2K_event_keysh", "MP2K_event_voice", "MP2K_event_vol", "MP2K_event_pan",
            "MP2K_event_bend", "MP2K_event_bendr", "MP2K_event_lfodl", "MP2K_event_modt", "MP2K_event_tune",
            "MP2K_event_port", "MP2K_event_lfos", "MP2K_event_mod"]


def nm(path, tool="arm-none-eabi-nm"):
    out = subprocess.run([tool, path], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = (int(p[0], 16), p[1])
    return syms


def build_port(gba):
    """Compile hal_m4a.c for ARM, resolving the engine's C half to its ROM addresses."""
    d = tempfile.mkdtemp(prefix="m4a_difftest_")
    cflags = ["-I", SRC, "-mcpu=arm7tdmi", "-mthumb", "-mthumb-interwork", "-mlong-calls", "-O2", "-ffreestanding",
              "-DHAL_AUDIO=1", "-fno-strict-aliasing", "-Wall"]
    objs = []
    for src in (os.environ.get("HAL_M4A_SRC", os.path.join(SRC, "hal_m4a.c")), os.path.join(HERE, "harness.c")):
        o = os.path.join(d, os.path.basename(src) + ".o")
        subprocess.run(["arm-none-eabi-gcc", *cflags, "-c", src, "-o", o], check=True)
        objs.append(o)
    undefined = set()
    for o in objs:
        out = subprocess.run(["arm-none-eabi-nm", "-u", o], capture_output=True, text=True, check=True).stdout
        undefined |= {line.split()[-1] for line in out.splitlines() if line.strip()}
    ld = os.path.join(d, "link.ld")
    with open(ld, "w") as f:
        f.write(open(os.path.join(HERE, "link.ld.in")).read())
        for name in sorted(undefined):
            if name in gba:
                addr, typ = gba[name]
                if typ in "Tt":
                    addr |= 1  # Thumb
                f.write(f"{name} = 0x{addr:08X};\n")
    elf = os.path.join(d, "port.elf")
    libgcc = subprocess.run(["arm-none-eabi-gcc", "-mcpu=arm7tdmi", "-mthumb", "-print-libgcc-file-name"],
                            capture_output=True, text=True, check=True).stdout.strip()
    subprocess.run(["arm-none-eabi-ld", "-T", ld, *objs, libgcc, "-o", elf], check=True)
    binf = os.path.join(d, "port.bin")
    subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", elf, binf], check=True)
    return open(binf, "rb").read(), nm(elf)


class Gba:
    def __init__(self, rom, port_bin):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        for base, size in ((0, 0x4000), (0x02000000, 0x40000), (0x03000000, 0x8000), (0x04000000, 0x1000),
                           (0x08000000, 0x400000), (PORT_BASE, 0x100000), (RET, 0x1000)):
            uc.mem_map(base, size)
        uc.mem_write(0x08000000, rom)
        uc.mem_write(PORT_BASE, port_bin)
        uc.mem_write(RET, b"\x00\xbf" * 8)
        self.vcount = 0

        def vcount_read(uc_, access, addr, size, value, user):
            self.vcount = (self.vcount + 1) % 228
            uc_.mem_write(0x04000006, bytes([self.vcount, 0]))

        uc.hook_add(UC_HOOK_MEM_READ, vcount_read, begin=0x04000006, end=0x04000007)

    def hook_cpuset(self, addr):
        def cpuset(uc, a, size, user):
            src, dst, ctl = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
            n = ctl & 0x1FFFFF
            unit = 4 if ctl & (1 << 26) else 2
            fixed = ctl & (1 << 24)
            src &= ~(unit - 1)
            dst &= ~(unit - 1)
            if fixed:
                v = bytes(uc.mem_read(src, unit))
                uc.mem_write(dst, v * n)
            else:
                uc.mem_write(dst, bytes(uc.mem_read(src, n * unit)))
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        self.uc.hook_add(UC_HOOK_CODE, cpuset, begin=addr, end=addr)

    def call(self, addr, *args):
        uc = self.uc
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_SP, 0x03007E00)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        uc.emu_start(addr | 1, RET, count=50_000_000)

    def r32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--songs", default="0-164")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--mix", action="store_true", help="also start the next songs (sound effects) during each song")
    ap.add_argument("--reverb", type=int, default=0, help="force m4aSoundMode reverb 1..127 after init")
    a = ap.parse_args()

    gba = nm(a.elf)
    port_bin, port = build_port(gba)
    rom = open(a.rom, "rb").read()
    S = lambda n: gba[n][0]
    si = S("gSoundInfo")
    regions = [("pcmBuffer", si + 0x350, 1584 * 2), ("channels", si + 0x50, 12 * 0x40),
               ("soundInfo header", si + 4, 0x1C), ("cgbChans", S("gCgbChans"), 4 * 0x40),
               ("PSG registers", 0x04000060, 0x40), ("DMA registers", 0x040000BC, 0x24)]
    for i in range(4):
        regions.append((f"tracks {i}", S(f"gMPlayTrack_{i}"), (8 if i == 0 else 1) * 0x50))  # gMPlayTable: 8, 1, 1, 1
        regions.append((f"player {i}", S(f"gMPlayInfo_{i}"), 0x38))

    lo, hi = (int(x) for x in a.songs.split("-")) if "-" in a.songs else (int(a.songs), int(a.songs))
    failures = 0
    for song in range(lo, hi + 1):
        runs = {}
        for side in ("rom", "port"):
            g = Gba(rom, port_bin)
            g.hook_cpuset(S("CpuSet"))
            g.call(S("m4aSoundInit"))
            if a.reverb:
                g.call(S("m4aSoundMode"), 0x80 | a.reverb)
            if side == "port":
                swap = {S(n) | 1: port[n][0] | 1 for n in REPLACED}
                jt = S("gMPlayJumpTable")
                for k in range(36):
                    v = g.r32(jt + 4 * k)
                    if v in swap:
                        g.w32(jt + 4 * k, swap[v])
                g.w32(si + 0x38, port["MP2K_event_nxx"][0] | 1)
                if g.r32(si + 0x20) in swap:
                    g.w32(si + 0x20, swap[g.r32(si + 0x20)])
                for i in range(4):
                    p = S(f"gMPlayInfo_{i}")
                    if g.r32(p + 0x38) in swap:
                        g.w32(p + 0x38, swap[g.r32(p + 0x38)])
            calls = {}
            if side == "port":  # make sure the compiled code is what actually runs
                for n in ("MP2K_event_nxx", "SoundMain", "MP2KPlayerMain"):
                    def count(uc, addr, size, user, n=n):
                        calls[n] = calls.get(n, 0) + 1
                    g.uc.hook_add(UC_HOOK_CODE, count, begin=port[n][0], end=port[n][0])
            vsync = (port["m4aSoundVSync"][0] if side == "port" else S("m4aSoundVSync"))
            main_ = (port["SoundMain"][0] if side == "port" else S("SoundMain"))
            g.call(S("m4aSongNumStart"), song)
            frames = []
            for f in range(a.frames):
                if a.mix and f in (90, 200, 330):
                    g.call(S("m4aSongNumStart"), (song + f // 90) % 165)
                g.call(vsync)
                g.call(main_)
                frames.append([bytes(g.uc.mem_read(addr, size)) for _, addr, size in regions])
            runs[side] = frames
            if side == "port":
                runs["calls"] = calls
        bad = None
        for f in range(a.frames):
            for (name, addr, size), x, y in zip(regions, runs["rom"][f], runs["port"][f]):
                if x != y:
                    off = next(i for i in range(size) if x[i] != y[i])
                    bad = (f, name, off, x[off], y[off])
                    break
            if bad:
                break
        active = sum(1 for fr in runs["rom"] if any(fr[0]))
        if bad:
            failures += 1
            print(f"song {song:3d}: MISMATCH frame {bad[0]} in {bad[1]} at +0x{bad[2]:X}: rom {bad[3]:02X} port {bad[4]:02X}")
        else:
            c = runs["calls"]
            print(f"song {song:3d}: identical over {a.frames} frames ({active} with sound; port calls: "
                  f"{c.get('SoundMain', 0)} SoundMain, {c.get('MP2KPlayerMain', 0)} MP2KPlayerMain, "
                  f"{c.get('MP2K_event_nxx', 0)} notes)")
        sys.stdout.flush()
    print(f"{hi - lo + 1 - failures}/{hi - lo + 1} songs identical")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
