# Klonoa: Empire of Dreams → Dreamcast: technical report 2 — the PowerVR renderer

*2026-11-01. All figures come from the scripted runs in §7, under flycast (headless, HLE BIOS,
framebuffer emulation on). Nothing here has been run on real Dreamcast hardware.*

> **Later:** the "no sound" statements below describe the port at this report. Sound has since
> landed; see [report 3](audio.md). The visual regression of §7 still passes with sound on, with
> dumps bit-identical to the silent build's.
>
> **Later still:** the ×2 output described in §1 ("Output") has been replaced. The game is now
> drawn 1:1 into a 240×160 render-to-texture target, and that texture is shown at scale 8/3
> (640×426, centred, black bars of 27 rows): the largest uniform 3:2 fit in 4:3 640×480. That
> costs one frame of display latency and some frame rate in heavy boot scenes; see
> [report 5](vmu-saves-display.md) §2.

## Summary

The PowerVR renderer is now the default. The software renderer from report 1 is still built in:
hold **Y** at boot to switch renderer, or build with `RENDERER=sw` to make it the default.

| | Software renderer | PowerVR renderer |
|---|---|---|
| Gameplay, Vision 1-1 (27,480 frames) | 6.0 fps (480 frames measured) | **59.8 fps**; no 120-frame window below 59.7; worst frame 20.7 ms |
| Boot → level entry (2,160 frames) | 6.4 fps; slowest window 4.6 | 58.8 fps; slowest window 55.1; 13 frames over 33 ms |
| CPU time spent drawing, per gameplay frame | 159 ms (152 ms drawing lines, 6.6 ms scaling to the screen) | 6.8 ms (of which 3.3 ms texture conversion, 3.0 ms vertex submission) |
| Picture | reference | 19/19 checkpoints within one colour level of the reference; 17 bit-identical |
| 30,000-frame soak | — | completed; memory flat; no exceptions |

**What these numbers do and do not show:**
- **Gameplay holds 60 fps in flycast** over 27,480 consecutive frames (§5). That is an emulator
  measurement. Flycast's SH-4 timing is approximate, and with the MMU on, the game's own
  memory-bound code ran about 1.7× slower than with it off in a microbenchmark (§6). Real hardware
  may differ in either direction.
- **Scene transitions still drop frames.** 13 frames in the first 2,300 take over 33 ms, the worst
  88 ms. They are the game's own decompression and the bulk texture rebuild that follows it
  (§5.3). None occur once the level is running.
- **There is still no sound.** The game's PCM mixer is ARM assembly and is still stubbed (§6). It
  is the one large unknown in the 60 fps budget: gameplay currently leaves 9.3 ms of each
  16.7 ms frame idle.
- **`-ffast-math` and sh4zam were not needed.** Rendering costs 6.8 ms per frame, and almost all of
  it is integer and memory work: converting tiles into textures and writing vertices. The floating
  point is a few hundred vertex positions and UVs per frame, inside the 3.0 ms vertex submission.
  They remain options for the sound mixer.

## 1. How the renderer works

Source: `port/dc/src/hal_video_pvr.c`. The game is unchanged. It still writes GBA registers, VRAM,
OAM and palettes through the MMU-mapped GBA address space. The renderer reads those and draws the
picture with the PowerVR rather than computing pixels on the CPU.

- **Per-line state.** As the emulated beam passes each visible line, `hal_pvr_line()` records the
  display registers: mode, BG control, scroll, affine matrices and reference points, windows,
  blending, brightness and mosaic. That keeps the game's HBlank effects, such as per-line scroll
  waves and affine floor planes. Each layer is then split into runs of lines over which *its own*
  state is constant, so an effect on one BG does not multiply the geometry of the others.
- **Backgrounds.**
  - Every text or affine BG is mirrored into an 8bpp twiddled texture the size of the whole BG.
    For a text BG each texel is (palette bank << 4 | colour), so per-tile 4bpp palettes survive;
    8bpp BGs store the colour index directly.
  - Scrolling is a UV offset with texture wrap. An affine BG is the same quad with affine UVs.
  - Only map entries whose value or character data changed are rebuilt, tracked per 64-byte VRAM
    chunk with generation counters.
- **Sprites.** OBJ character data is mirrored into a 4bpp and an 8bpp 256×256 tile atlas, and
  only changed tiles are reconverted. Each sprite tile is a quad. Affine sprites are transformed
  quads clipped to the display.
- **Palettes.** All four palette sets live in PVR palette RAM, in ARGB1555:

  | PVR palette entries | Contents |
  |---|---|
  | 0–255 | BG, as 16-colour banks |
  | 256–511 | OBJ, as 16-colour banks |
  | 512–767 | BG, 256-colour |
  | 768–1023 | OBJ, 256-colour |

  Colour 0 of each bank is transparent.
- **Priority and blending.** GBA priority becomes depth, so colour-keyed layers go to the
  punch-through list in any order. Alpha-blended layers go to the translucent list.
- **Windows.** WIN0 and WIN1 split each run into horizontal segments with their own layer and
  effect masks.
- **Brightness fades** are applied in one of two ways:
  - **Exact path:** when every visible region gets the effect, the fade is folded into the
    palette, which matches the GBA's integer arithmetic exactly.
  - **Fallback:** otherwise it is applied per layer with vertex colour and offset colour.
- **BG mosaic** is drawn from a small block texture holding one texel per mosaic block, cached by
  its parameters.
- **Output.** The 240×160 picture is scaled ×2 and centred in 640×480. Dithering is off so that
  colours match exactly. Vertices go through KallistiOS direct rendering.
- **Texture uploads use plain 32-bit stores.** Store-queue uploads (`sq_cpy`) froze PowerVR
  rendering under the MMU (§4).

## 2. GBA effects Klonoa actually uses

Counted over the 30,000-frame soak (`FEATURE` lines, printed at the end of every scripted run).
The soak covers boot, title, file select, the world map, vision select, the story cutscene, the
Vision 1-1 text box and about 7½ minutes of Vision 1-1 gameplay. Later visions and bosses are not
covered yet.

| Effect | Frames | Status |
|---|---:|---|
| Windows (WIN0/WIN1) | 29,159 | implemented |
| Affine BG (rotation/scaling) | 28,374 | implemented |
| 8bpp (256-colour) BG | 28,495 | implemented |
| Affine sprites | 759 | implemented |
| Brightness fade, exact palette path | 582 | implemented, exact |
| Brightness fade, per-layer vertex colour | 379 | implemented, ±1 level |
| Alpha blending | 351 | implemented; exact when EVA + EVB = 16, approximated otherwise |
| BG registers changed mid-frame (HBlank effects) | 299 | implemented |
| BG mosaic | 212 | implemented, both renderers |
| **OBJ mosaic** | **152** | **not implemented** — sprites drawn without mosaic (§6) |
| Semi-transparent sprites | 0 | implemented, unused so far |
| OBJ window | 0 | not implemented, unused so far |
| Bitmap modes 3–5 | 0 | not implemented, unused so far |

A feature the renderer does not implement is logged once when the game first uses it
(`pvr: OBJ mosaic used (not implemented)`), so a longer route will show whether anything else is
needed.

## 3. Visual verification

`tools/regress.sh` builds the route script (`tests/route.txt`) and runs it in flycast. On each of
19 checkpoint frames it renders the same game state with **both** renderers and dumps both
pictures over serial: the software picture, and the PowerVR picture read back from video RAM.
`tools/framedump.py` then compares them pixel by pixel. Input is frame-indexed, so a frame number
is the same game state in every build.

| Frame | Scene | Pixels off by more than 1 level | Not bit-identical |
|---:|---|---:|---:|
| 200 | Namco logo | 0 | 0 |
| 560 | title screen | 0 | 0 |
| 900 | file select (alpha blend) | 0 | 358 (0.93%) |
| 1085 | mosaic transition | 0 | 0 |
| 1180 | vision select | 0 | 0 |
| 1245 | mosaic into world map (large blocks) | 0 | 0 |
| 1258 | mosaic into world map | 0 | 0 |
| 1300 | world map | 0 | 0 |
| 1360 | mosaic, world map → vision select | 0 | 0 |
| 1700 | story cutscene | 0 | 0 |
| 1900 | vision select | 0 | 0 |
| 2000 | Vision 1-1 text box (window-limited darken) | 0 | 1,968 (5.12%) |
| 2175 | mosaic into Vision 1-1 | 0 | 0 |
| 2200 | Vision 1-1 fade-in | 0 | 0 |
| 2500 | gameplay | 0 | 0 |
| 2800 | gameplay | 0 | 0 |
| 2950 | gameplay, running right | 0 | 0 |
| 3100 | gameplay, after a jump | 0 | 0 |
| 3200 | gameplay | 0 | 0 |

The two non-identical frames differ only by single colour levels:
- **Frame 900:** the GBA computes A·EVA + B·EVB in integers; the PowerVR blends in its own
  precision.
- **Frame 2000:** the darkening applies inside a window only, so it takes the vertex-colour
  fallback path instead of the exact palette path.

The software renderer is itself a reimplementation, not the GBA. The comparison shows that the
two renderers agree, and the dumped PNGs were also inspected by eye. It is not a comparison
against real GBA output.

## 4. Bugs found on the way

- **Store-queue texture uploads froze the PowerVR** from the cutscene on: render statistics
  stopped and the frame wait grew to 106 ms. I bisected it with `PVRDBG=` masks to `sq_cpy`
  uploads with the MMU on. KallistiOS translates the store-queue area through a TLB entry that it
  remaps on every `sq_lock`. Plain stores to uncached texture RAM fixed it.
- **A game DMA to NULL corrupted Dreamcast memory.**
  - Every VBlank, `VBlankIntr_Common` copies the BG1 and BG2 tilemaps to `gBgInfo[n].pTilemap`.
    During the boot screen those pointers are still NULL.
  - On the GBA that copy targets the read-only BIOS and does nothing. Here the 2 KiB write went
    to an address with no TLB entry. Under flycast it landed in unrelated Dreamcast RAM; on
    hardware it would have been a TLB-miss exception.
  - In one build it hit the frame counter, which hung the game at frame 1. Every other build was
    probably corrupting something else silently.
  - I found it with a write watchpoint in a locally patched flycast interpreter.
  - **Fix:** DMA and `CpuSet` ranges are now checked against the mapped GBA regions plus
    Dreamcast RAM, and anything else is dropped, as on the GBA. It is logged
    (`dma3: 03001100 -> 00000000 … dropped as on hardware`): four transfers per run, all during
    the boot screen.
- **Edge texels of affine layers** differed until UVs were shifted a quarter pixel and biased by
  1/512 texel.
- **±1-level noise everywhere** came from the PowerVR's dithering, which is now off.

## 5. Performance

### 5.1 Frame rate and timing (PowerVR renderer, 30,000-frame soak)

Frame time is measured between consecutive frame ends with the SH-4 timer. Each window is 120
frames.

| Stretch | Frames | Mean fps | Slowest window | Worst frame | Frames > 33 ms |
|---|---:|---:|---:|---:|---:|
| Boot → level entry | 2–2161 | 58.8 | 55.1 fps | 88.1 ms | 12 |
| Level entry window (fade-in) | 2162–2281 | — | 58.0 fps | 57.9 ms | 1 |
| Gameplay | 2402–29881 | **59.8** | **59.7 fps** | **20.7 ms** | **0** |
| Whole run | 2–29881 | 59.7 | 55.1 fps | 88.1 ms | 13 |

The 10,000-frame performance run (`tests/perf.txt`) gives the same figures to the first decimal.
It is the same route, shorter.

### 5.2 Where a gameplay frame's time goes

| Phase | ms/frame | What it is |
|---|---:|---|
| Game logic | 0.08 | the game's own C code between VBlanks (excluding rendering) |
| Line snapshots | 0.44 | recording display registers for 160 lines |
| Texture conversion | 3.30 | rebuilding changed map entries and sprite tiles; palette upload |
| Vertex submission | 3.02 | building and sending the punch-through and translucent lists |
| Waiting | 9.31 | waiting for the PowerVR and VBlank: idle time |
| **Total** | **16.72** | **59.8 fps** |

The game's own logic is a small fraction of the frame. The software renderer's cost (§5.5) came
from computing every pixel on the CPU, and that is what the PowerVR now does.

### 5.3 Transition spikes

All 13 frames over 33 ms are scene changes:

| Frame | Time | Dominant cost |
|---:|---:|---|
| 371 | 52 ms | game 36 ms (title screen load) |
| 467 | 34 ms | texture 19 ms |
| 1073 | 36 ms | texture 22 ms |
| 1237 | 50 ms | texture 43 ms (world map) |
| 1265 | 38 ms | texture 29 ms |
| 1384 | 88 ms | game 74 ms (cutscene load) |
| 1385 | 48 ms | texture 42 ms |
| 1834 | 34 ms | game 19 ms |
| 1835 | 43 ms | texture 38 ms |
| 1863 | 34 ms | texture 21 ms |
| 2193 | 58 ms | game 43 ms (Vision 1-1 load) |
| 2194 | 34 ms | texture 29 ms |
| 2222 | 36 ms | texture 22 ms |

There are two causes:
- **"Game" spikes** are the game's own scene loading, most likely decompressing graphics
  (LZ77, Huffman) and copying them into VRAM. That is memory-bound code, the kind the flycast
  MMU overhead hits hardest (§6).
- **"Texture" spikes** follow on the next frame. A whole BG's character data has changed, so the
  renderer rebuilds most of a 512×512 or 1024×1024 map texture at once.

Possible next steps, none tried yet:
- spread the rebuild over two or three frames while the screen is faded out;
- rebuild only the visible part of the map;
- unroll the decompressor's inner loops.

### 5.4 Memory (end of the soak)

| | |
|---|---|
| Main RAM, executable | 10.8 MB ELF: text+rodata 5.76 MB (of which the 4 MiB ROM image, mapped in place), data 1.30 MB, bss 0.50 MB |
| Main RAM, heap | 730 KiB in use (798 KiB arena), the same at boot and after 30,000 frames |
| Video RAM, textures | 5,087 KiB of 8 MB free (5,407 KiB at boot; BG map textures are allocated when first needed and reallocated when a BG changes size) |

### 5.5 Software renderer, for comparison

Same route, `RENDERER=sw`, stopped at frame 2,700 (it reaches Vision 1-1 at frame 2,193). It
was re-run after the DMA fix to confirm the fallback still works.

| Stretch | Frames | Mean fps | Slowest window | Drawing per frame |
|---|---:|---:|---:|---:|
| Boot → level entry | 2–2161 | 6.4 | 4.6 fps | 139 ms + 6.6 ms scaling |
| Level entry and gameplay | 2162–2641 | 6.0 | 4.9 fps | 152 ms + 6.6 ms scaling |

The PowerVR renderer is about 10× faster in frame rate. Its CPU drawing cost is about 23× lower
in gameplay (6.8 ms against 159 ms).

## 6. Limitations

- **No real-hardware test.** Everything above is flycast.
  - The port runs with the SH-4 MMU on, so that the game's literal GBA addresses work.
  - Flycast emulates the MMU in its full-MMU mode. A microbenchmark of plain memory loops ran in
    68 ms with the MMU off and 116 ms with it on, but real hardware has no per-access cost for a
    TLB hit.
  - Flycast also does not emulate the SH-4's UBC watchpoints, and its timing is approximate.
- **No sound.** The m4a sequencer runs as compiled C. The PCM mixer (`SoundMain`/`SoundMainRAM`)
  is ARM assembly in kleod and is stubbed, so the game is silent. The next milestone is porting
  that mixer, or replacing it with an AICA backend.
- **OBJ mosaic** is used on 152 frames of the soak and not drawn: those sprites appear without
  the mosaic effect. It needs each affected sprite's pixels sampled per mosaic block. The BG
  mosaic approach (a small block texture) should carry over, but it has not been tried.
- **Alpha blending** is exact only when EVA + EVB = 16. The PowerVR blends as A·a + B·(1−a).
- **Not implemented, not yet seen in use:**
  - OBJ window;
  - bitmap modes 3–5;
  - palette writes in the middle of a frame (all palette changes are taken at the start of the
    frame);
  - a BG switching between text and affine within one frame.

  Each is logged once if the game uses it.
- **Coverage.** The route reaches Vision 1-1 only. Later visions, bosses and the ending may use
  effects that this one does not.
- **Brightness under a window** uses the vertex-colour path, which is within one level, not
  exact (frame 2000).

## 7. How to build and test

Prerequisites, as in report 1:
- KallistiOS v2.3.0 (`d81aa3f`) with its GCC 15.2.0 `sh-elf` toolchain;
- kleod at `ac62cf6`, built once with your own `baserom.gba`;
- this repository's `port/` directory in the kleod checkout;
- flycast for testing. The runs here used `7adbf3c` with
  `-config config:rend.EmulateFramebuffer=yes`, so that the PowerVR picture can be read back.

```sh
source /path/to/kos/environ.sh
cd kleod/port/dc

make                       # PowerVR renderer (default)    -> build/klonoa-dc.elf
make RENDERER=sw           # software renderer as the default (hold Y at boot for the other)

# Visual regression: 19 checkpoints, PowerVR vs software. Exit 0 if no pixel is off by > 1 level.
tools/regress.sh /path/to/kleod /path/to/run-elf.sh build/regress

# Performance (10,000 frames) and soak (30,000 frames); read the results with perfstats.py.
make RENDERER=pvr SCRIPT=tests/perf.txt FREEZE=1      # or SCRIPT=tests/soak.txt
<run build/klonoa-dc.elf in flycast, saving serial output to serial.txt>
python3 tools/perfstats.py serial.txt
```

Build options are written into `build/options.h`, and the HAL rebuilds when any of them changes:

| Option | Effect |
|---|---|
| `RENDERER=sw\|pvr` | the default renderer |
| `SCRIPT=tests/<name>.txt` | frame-indexed input, dump and end commands; enables fast serial |
| `MAX_FRAMES=N` | stop after N frames |
| `AUTOPLAY=K` | tap START every K frames |
| `FREEZE=1` | hold the last frame instead of exiting |
| `PVRDBG=mask` | disable parts of the PowerVR pipeline, for bisecting |

The flycast wrapper used for these runs is a short script. It runs flycast headless under Xvfb
with the HLE BIOS and the serial console on, waits N seconds, and saves the serial output and a
screenshot.

```sh
flycast -config config:UseReios=yes -config config:Debug.SerialConsoleEnabled=yes \
        -config config:rend.EmulateFramebuffer=yes build/klonoa-dc.elf > serial.txt
```
