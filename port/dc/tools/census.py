#!/usr/bin/env python3
"""Portability census for the Dreamcast port. Every figure in docs/dreamcast-port-report.md comes
from this script, run after `make compare` (GBA) and `make -C port/dc` (Dreamcast).

    python3 port/dc/tools/census.py --root .
"""
import argparse
import collections
import glob
import os
import re
import subprocess

IO_CATS = [
    (0x000, 0x008, "display control/status"), (0x008, 0x010, "BG control"), (0x010, 0x020, "BG scroll"),
    (0x020, 0x040, "BG affine"), (0x040, 0x04C, "windows"), (0x04C, 0x050, "mosaic"), (0x050, 0x056, "blending"),
    (0x060, 0x0B0, "sound"), (0x0B0, 0x0E0, "DMA"), (0x100, 0x110, "timers"), (0x120, 0x200, "keypad/serial"),
    (0x200, 0x204, "interrupt enable/flags"), (0x204, 0x208, "waitstate"), (0x208, 0x20C, "IME"),
]
REGIONS = {0x02: "EWRAM", 0x03: "IWRAM", 0x05: "palette RAM", 0x06: "VRAM", 0x07: "OAM", 0x08: "ROM", 0x0D: "EEPROM"}
BIOS = ["CpuSet", "HuffUnComp", "LZ77UnCompWram", "Sqrt", "VBlankIntrWait", "SoftResetRom"]


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def io_cat(off):
    for lo, hi, name in IO_CATS:
        if lo <= off < hi:
            return name
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    a = ap.parse_args()
    r = a.root
    dcb = f"{r}/port/dc/build"

    print("== kleod (GBA) ==")
    print("ROM verification:", run(["sha1sum", "-c", f"{r}/kleod.sha1"]).strip() if os.path.exists(f"{r}/kleod.gba") else "not built")
    cfun_names, eeprom_names, cbytes = set(), set(), 0
    for o in glob.glob(f"{r}/build/kleod/src/*.o"):
        names = {l.split()[2] for l in run(["arm-none-eabi-nm", "--defined-only", o]).splitlines() if l.split()[1:2] in (["T"], ["t"])}
        cfun_names |= names
        if o.endswith("/eeprom.o"):
            eeprom_names = names
        for l in run(["arm-none-eabi-size", "-A", o]).splitlines():
            if l.startswith(".text"):
                cbytes += int(l.split()[1])
    afun = abytes = 0
    for s in glob.glob(f"{r}/asm/*.s"):
        afun += len(re.findall(r"^\s*(?:thumb|arm|non_word_aligned_thumb)_func_start\s", open(s).read(), re.M))
        o = s.replace(f"{r}/asm/", f"{r}/build/kleod/asm/").replace(".s", ".o")
        for l in run(["arm-none-eabi-size", "-A", o]).splitlines():
            if l.startswith(".text"):
                abytes += int(l.split()[1])
    src = "".join(open(f, errors="ignore").read() for f in glob.glob(f"{r}/src/*.c"))
    inline = len(re.findall(r"\basm\s*\(", src))
    nonmatch = len(re.findall(r"NONMATCH|INCLUDE_ASM", src))
    cfun = len(cfun_names)
    print(f"C game code: {cfun} functions, {cbytes} bytes of Thumb code in {len(glob.glob(f'{r}/src/*.c'))} files")
    print(f"assembly: {afun} routines, {abytes} bytes in {len(glob.glob(f'{r}/asm/*.s'))} files")
    print(f"C share of code bytes: {100 * cbytes / (cbytes + abytes):.2f}%")
    print(f"NONMATCH/INCLUDE_ASM in C: {nonmatch}; inline asm statements in C: {inline}")

    print("\n== Dreamcast build ==")
    games = sorted(glob.glob(f"{dcb}/game/*.pp.c"))
    objs = [g.replace(".pp.c", ".o") for g in games]
    built = [o for o in objs if os.path.exists(o)]
    print(f"game files compiled for SH-4: {len(built)}/{len(games)} (eeprom.c replaced by hal_eeprom.c)")
    sed_rules = [l for l in open(f"{r}/port/dc/patches/game.sed") if l.strip() and not l.startswith("#")]
    print(f"build-time source patches: {len(sed_rules)}")
    sh_names = set()
    for o in built:
        sh_names |= {l.split()[2].lstrip("_") for l in run(["sh-elf-nm", "--defined-only", o]).splitlines() if l.split()[1:2] in (["T"], ["t"])}
    expected = cfun_names - eeprom_names
    print(f"functions in SH-4 game objects: {len(sh_names & expected)} of the {len(expected)} kleod functions outside eeprom.c"
          f" ({100 * len(sh_names & expected) / len(expected):.1f}%); missing: {sorted(expected - sh_names)[:10]}")
    if os.path.exists(f"{dcb}/link_report.txt"):
        print(open(f"{dcb}/link_report.txt").read().rstrip())
    elf = f"{dcb}/klonoa-dc.elf"
    print("executable:", f"{elf} ({os.path.getsize(elf)} bytes)" if os.path.exists(elf) else "not linked")

    print("\n== GBA hardware dependencies in the game C (preprocessed with kleod's own headers) ==")
    # Count against kleod's own headers (the DMA overlay turns DMA register writes into calls).
    import tempfile
    tmp = tempfile.mkdtemp()
    orig = []
    for g in games:
        name = os.path.basename(g)[:-5]
        if name == "eeprom":
            continue
        out = f"{tmp}/{name}.i"
        subprocess.run(["sh-elf-cpp" if subprocess.run(["which", "sh-elf-cpp"], capture_output=True).returncode == 0 else "cpp",
                        "-nostdinc", "-I", f"{r}/tools/agbcc/include", "-iquote", f"{r}/include", "-DREVISION=0", "-DENGLISH",
                        f"{r}/src/{name}.c", "-o", out], capture_output=True)
        orig.append(out)
    io = collections.Counter()
    io_regs = collections.defaultdict(set)
    reg = collections.Counter()
    reg_addrs = collections.defaultdict(set)
    bios = collections.Counter()
    for g in orig:
        text = "\n".join(l for l in open(g, errors="ignore").read().split("\n") if not l.startswith("#"))
        for m in re.finditer(r"0x4000000\s*\+\s*(0x[0-9a-fA-F]+|\d+)", text):
            off = int(m.group(1), 0)
            io[io_cat(off)] += 1
            io_regs[io_cat(off)].add(off)
        for m in re.finditer(r"\b0x([0-9A-Fa-f]{7,8})\b", text):
            v = int(m.group(1), 16)
            if v >> 24 in REGIONS and v >= 0x02000000:
                reg[REGIONS[v >> 24]] += 1
                reg_addrs[REGIONS[v >> 24]].add(v)
        for b in BIOS:
            calls = len(re.findall(rf"\b{b}\s*\(", text))
            decls = len(re.findall(rf"\b(?:void|u16|u32|s32)\s+{b}\s*\(", text))
            bios[b] += calls - decls
    print(f"memory-mapped I/O register accesses: {sum(io.values())} sites, {sum(len(s) for s in io_regs.values())} distinct registers")
    for k, v in io.most_common():
        print(f"  {k:24s} {v:5d} sites  {len(io_regs[k]):3d} registers")
    print("fixed-address literals by region:")
    for k, v in reg.most_common():
        print(f"  {k:24s} {v:5d} sites  {len(reg_addrs[k]):3d} addresses")
    print("BIOS call sites:", dict(bios), "total", sum(bios.values()))
    polls = 0
    for g in orig:
        polls += len(re.findall(r"while\s*\([^;{]*0x4000000[^;{]*\)\s*;", open(g, errors="ignore").read()))
    print("busy-wait loops on hardware registers:", polls)


if __name__ == "__main__":
    main()
