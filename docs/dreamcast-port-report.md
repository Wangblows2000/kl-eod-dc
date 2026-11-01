# Klonoa: Empire of Dreams → Dreamcast: technical report 1

*2026-10-31. Every figure comes from a build or a script in this repository. Run
`port/dc/tools/census.py` after the builds to reproduce them.*

> **Later:** the "no sound" and stubbed-mixer statements below describe the port at this report.
> The MP2K mixer and sequencer have since been ported and the port plays sound through the AICA;
> see [report 3](audio.md).

## Summary

**Can kleod be the foundation for a native Dreamcast port? Yes.** Its game logic compiles for the
SH-4 essentially unmodified. It links into a KallistiOS executable, boots in an emulator, and runs
the game's own scene flow, from the Namco logo through the title screen and file select to the
world map.

**What this is not yet:**
- **Not tested on real hardware.** It has only run in an emulator.
- **No sound.** The PCM mixer is still the original ARM assembly and is stubbed out.
- **Slow.** It runs at about 5–12 fps against the GBA's 60, because the graphics are drawn by a
  correctness-first software renderer.

## 1. Is kleod complete?

Checked independently at kleod commit `ac62cf6`, built with the compiler kleod specifies
(pret/agbcc) from a ROM with SHA-1 `a0a298d9…569b`.

| Check | Result |
|---|---|
| `make compare` | `kleod.gba: OK` (byte-identical ROM) |
| Game code written in C | 465 functions, 329,640 bytes of Thumb code, 27 files |
| `NONMATCH` / `INCLUDE_ASM` left in the C | 0 |
| Code still in assembly | 77 routines, 4,336 bytes, 7 files (1.3% of code bytes) |
| Inline `asm` in the C | 10 statements: 6 ARM register pins (`register … asm("r1")`, valid on SH-4 too), 3 `nop`, 1 `swi 0x2A` |
| License | MIT, © 2026 testyourmine |

The 77 assembly routines:

| Group | Routines | How the port handles them |
|---|---:|---|
| m4a / MP2K sound engine (`m4a0.s`) | 44 | 4 rewritten in C; the PCM mixer and the other sequencer events are stubbed (silent) |
| Thunks into IWRAM copies | 12 | C wrappers |
| Heap (ARM) | 6 | C, same block layout |
| BIOS calls | 6 | C: `CpuSet`, LZ77, Huffman, `Sqrt`, `VBlankIntrWait`, `SoftResetRom` |
| Startup, interrupt dispatch | 3 | Replaced by the Dreamcast entry point and frame loop |
| RNG (ARM) | 3 | C, bit-exact (the attract-mode demos depend on it) |
| `Abs`, `StringCompare`, `StringCopy` | 3 | C |

**Data.** Game data is mostly opaque `.incbin` ranges from your own `baserom.gba`, with
2,817 code pointers expressed symbolically. kleod's `make extract` turns graphics into files, but
levels and most tables are still raw ROM bytes. A port therefore still needs your ROM (or assets
extracted from it), which is the normal arrangement for decompilation ports.

## 2. Which parts are portable?

| | Result |
|---|---|
| Game source files compiled for SH-4 (GCC 15.2, KallistiOS flags) | **26 / 26**. `eeprom.c` is replaced, because it is a hardware driver |
| Functions emitted | **453 of the 454** non-EEPROM functions. The missing one is `UnusedFunc`, which nothing calls, so GCC drops it |
| Source changes needed | **4 one-line build-time patches** (`port/dc/patches/game.sed`), applied to a copy of the source. kleod's own files are never edited |
| Functions touched by those patches | 3 of 454 (**99.3% compile and run with no change at all**) |

The four patches:
- One removes a GBA BIOS call (`swi 0x2A`) from `m4a.c`.
- Three replace busy-waits on the GBA scanline counter (`REG_VCOUNT`) with calls that advance the
  emulated display (two in `m4a.c`, one in `rotation.c`).

The game logic itself (entities, physics, levels, menus, the scene state machine) needed no
changes.

## 3. GBA hardware dependencies found in the game C

| Type | Count |
|---|---|
| Memory-mapped I/O register accesses | **2,774 sites, 80 registers**. DMA accounts for 2,064 (through one macro); display 142, BG control 98, interrupts 96, blending 96, windows 83, scroll 72, affine 57, sound 52, other 14 |
| Fixed-address literals | VRAM 109, palette 66, EWRAM 46, IWRAM 22, OAM 14, ROM 12 |
| RAM globals at fixed addresses (kleod's `linker.ld`) | 182 IWRAM symbols resolved, plus 2 ROM-data and 2 linker constants |
| BIOS calls | 6 routines, 22 call sites (after macro expansion) |
| Busy-wait loops on hardware registers | 4: 3 on the scanline counter, 1 on the HBlank flag (the last needs no patch) |
| Raw GBA code addresses in data | 1 table of 12 entries (`gUnk_081179B4`, hard-coded in C), fixed up at boot |
| Cartridge EEPROM protocol | `eeprom.c` (10 functions): replaced |
| Sound hardware (DirectSound FIFO, timers) | `SoundInit` / `m4a` register writes; the backend is stubbed |

**Language and ABI.** Neither caused problems: both CPUs are 32-bit and little-endian, the code
has no unaligned-access faults so far, and modern GCC compiles the C89 game code without errors.
The build uses `-fwrapv -fno-strict-aliasing -fno-aggressive-loop-optimizations` so it doesn't
exploit undefined behaviour that agbcc never did.

## 4. Hardware abstraction layer (`port/dc/src/`)

**Memory (`hal_mem.c`).** Instead of rewriting every one of those 2,774 accesses, the SH-4's MMU
maps the GBA address map onto Dreamcast RAM:

| GBA range | Mapping |
|---|---|
| EWRAM | 4 × 64 KB pages |
| IWRAM | 64 KB page |
| I/O registers | 64 KB page |
| Palette | 64 KB page |
| VRAM | 2 × 64 KB pages |
| OAM | 64 KB page |
| ROM | 4 × 1 MB pages |

That is 14 static TLB entries. Game code, fixed-address globals and pointers embedded in data all
work as ordinary memory accesses. KallistiOS itself runs from untranslated P1 memory, so the
mapping does not disturb it.

The remaining layers:

| Layer | What it does |
|---|---|
| DMA (`hal_dma.c`) | An overlay of kleod's `include/gba/macro.h` routes `DmaSet` to software DMA: immediate, HBlank or VBlank timing, repeat and reload. State is mirrored into the I/O page, so the game's direct pause and restore writes still work |
| Frame and interrupts (`hal_frame.c`) | A scanline-stepped display beam. Each line is rendered, then HBlank DMA runs, then the game's own VCount and HBlank handlers. Line 160 runs VBlank DMA and the game's VBlank handler. `VBlankIntrWait()` runs the beam to VBlank and presents the frame |
| Video (`hal_video.c`) | Software GBA graphics renderer: modes 0–2, text and affine backgrounds, normal and affine sprites, priorities, WIN0, WIN1 and the OBJ window, alpha and brightness. Draws 240×160 at 2× into 640×480 RGB565. No mosaic or bitmap modes yet |
| Input (`hal_input.c`) | Dreamcast controller to the GBA key register |
| Save (`hal_eeprom.c`) | Same API as `eeprom.c`, backed by RAM. VMU persistence is still to do |
| BIOS (`hal_bios.c`) | C versions of the BIOS calls. The Huffman decoder was checked byte-identical against a decoder written from the GBATEK spec, on real ROM blobs |
| Sound (`hal_sound.c`) | C versions of the m4a routines the C sequencer calls through its jump table |
| Link (`tools/gen_symbols.py`) | Resolves every unresolved data symbol to its GBA address (taken from `kleod.elf`), and generates logging stubs for any assembly routine not yet ported |

## 5. Does the game logic link into a Dreamcast executable?

**Yes.** `make -C port/dc` produces `build/klonoa-dc.elf`:
- SH-4 ELF, entry point `0x8C010000`, about 9.7 MB.
- Your ROM is embedded at build time; the executable is never distributed.
- The game's own `AgbMain` links as native SH-4 code. Globals keep their GBA addresses, for
  example `gIntrTable` = `0x030047C0`.
- The link leaves 272 symbols for the generator to resolve: 186 to fixed GBA addresses, 26 to
  assembly stubs (sound only), and 60 provided by KallistiOS or the hardware layer.

## 6. First Dreamcast build result

**Toolchain:**
- kos-chain "stable" profile: GCC 15.2.0, binutils 2.45.1, newlib 4.6.0.
- KallistiOS v2.3.0 (`d81aa3f`), using its prebuilt AICA sound driver.

**Test setup:**
- flycast `7adbf3c`, running headless with its built-in HLE BIOS. No Dreamcast BIOS file is used.
- Serial output was captured, and screenshots were taken of the emulator window.

**Boot sequence:**
1. KallistiOS initialises and finds a controller and two VMUs.
2. `hal_mem` maps the GBA address space; both of its self-checks pass.
3. `AgbMain` runs: it clears RAM, sets up the heap, identifies the EEPROM and initialises sound.
4. The game's callback loop starts.

**Autoplay run.** A test build that taps START every 150 frames ran **1,500 frames with no
exceptions**. The game's own scene handlers advanced in order:

| Frames | Scene handler |
|---|---|
| 0–360 | `NamcoScreenHandler` |
| ~420 | `TitleScreenHandler` |
| ~780 | `FileSelectScreenHandler` |
| — | `sub_0804EB64` (the graphics-stream and cutscene player) |
| ~1,260 | `WorldMapScreenHandler` |
| — | `TransitionFromWorldMapToVisionSelect_FadeOut` |

Screenshots of the title screen and the world map show correct backgrounds, sprites, palettes and
priorities. The screenshots are not in the repository, since they are images of the game.

**Frame rate.** 5–12 fps, median about 6.5, measured in emulated Dreamcast time by the watchdog
thread. The bottleneck is the per-pixel software renderer.

**Bugs found and fixed on the way:**
- The game busy-waits on the scanline counter, which never advances in this design.
- `Clear64byte` calls through a jump table that the GBA BIOS normally fills.
- The Huffman decoder had a pointer-width bug (host-only) and needed input validation.

## 7. Not done yet, in priority order

1. **Speed.** Replace the software renderer with a PowerVR renderer: GBA tiles become textures,
   backgrounds become tilemap quads, sprites become quads, blending is done by the hardware.
   60 fps is not reachable without this.
2. **Sound.** Port the MP2K mixer (`SoundMain` / `SoundMainRAM`) and the remaining sequencer
   events to C, and feed the result to the AICA through KallistiOS `snd_stream`.
3. **Saves.** VMU-backed EEPROM image.
4. **Rendering gaps.** Mosaic, bitmap modes; check affine sprites and windows against the GBA
   game scene by scene.
5. **Hardware testing.** Run on a real Dreamcast. The static MMU mapping and cache aliasing are
   the main thing to watch.
6. **Packaging.** Load the ROM from CD or SD at runtime instead of embedding it; build a bootable
   `.cdi`.

**Known risks:**
- The port is undefined-behaviour-sensitive, because modern GCC is not agbcc. There are 17
  maybe-uninitialised warnings and 3 sequence-point warnings in the game C to review.
- Timing-sensitive raster effects may differ, because the beam only advances when the game waits
  on it.
- One early run crashed in `HuffUnComp` with a runaway pointer, and that was never reproduced. The
  decoder now validates its input and logs its first calls. Treat it as open until a long soak
  test passes.
