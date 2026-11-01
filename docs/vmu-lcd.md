# Klonoa: Empire of Dreams → Dreamcast: technical report 4 — Klonoa on the VMU screen

*2026-11-01. All results come from the host test and the scripted flycast runs in §6. Flycast
emulates the VMU's LCD function (it decodes and shows each 48×32 bitmap it is sent); nothing here
has been seen on a real VMU.*

## Summary

Every VMU screen plugged into a controller now shows a little character:

- **in gameplay, Klonoa** — the very animation frame the game is showing on the TV, from the
  game's own sprite data and animation timing, facing the way he faces on screen;
- **on menus** (title, file select, world map, story cutscenes, vision select, pause menu, game
  over), **a Moo** walking back and forth with the game's own Moo walk and look-around animations,
  at the speed Vision 1-1's Moos walk.

| | |
|---|---|
| Picture | 1 bit per pixel, 1:1 (no scaling): the game's 32×32 sprite box, centred (columns 8–39) for Klonoa, patrolling columns 0–47 for the Moo |
| What is reused | the frame lists, timers and tile data in the ROM: `gUnk_0805553C`/`080555A8`/`08055614` (Klonoa), the shared enemy table at `0x081898E0` (Moo), palettes `0x08077E28`, `0x08078328` |
| Updates | only when the picture changes; at most one write per VMU every 2 game frames (30 Hz); never blocks the game |
| Cost | 39 µs per game frame in gameplay; frame rate unchanged at 59.8 fps (§5) |
| Storage | none: only the maple LCD function is used, never the VMU's flash |
| Checks | host test, 82 checks, all pass; flycast route: every write arrived intact, every picture recomputed from the ROM matches, every frame lasted its ROM timer (§6) |

## 1. Where the game keeps Klonoa's animation

kleod's `src/anim.c` runs every animated sprite through one function,
`UpdateEntityAnimationInfoEntries`. Klonoa is animation slot 0:

- `gEntityAnimationInfo[0]` holds the current **state**, the current **frame** index and a **timer**.
- `gUnk_03005418` points to an `EntityAnimationData` table; entry 0 gives Klonoa's
  `pFrames[state]` frame lists. Each frame is `{src, timer, x/y nudge}`; `src` is 0x200 bytes of
  4bpp tiles (one 32×32 OBJ, 16 tiles in 1D order), DMA'd to OBJ VRAM `0x06010000` when the frame
  starts. Special `src` values: −1 loop, −2 hide, −3 hold, < 10000 jump to that state.
- `SetEntityAnimationInfoState()` starts a state (timer 1, frame 0xFF); the next update shows its
  frame 0. There are three tables: normal levels, hover-board levels, and the hover-board goal.
- Klonoa's OAM entry (`TRANSFER_OAM` in `code_08003D58.c`) is one 32×32 sprite at (−16, −32)
  from his position, palette 0, flipped by `gEntityInfo[0].hvFlip` bit 0 (or bit 3 of the affine
  matrix number when the sprite is affine, as in boss levels).

So the frame on the LCD is `pFrames[state][frame].src` — the same tile data the game copied to VRAM
this frame — with the same hFlip. Nothing is approximated or redrawn, and any state the game
selects is shown, including those the route below never reaches.

Klonoa's states, named from where the game sets them (`code_0800D188.c`, `code_08014184.c`; names
for 13–20 and 34–37 are approximate):

| State | | State | |
|---|---|---|---|
| 0 | idle | 12 | damage (`ReceiveDamage`) |
| 1 | run | 13 | hurt |
| 2 | jump | 14 | holding a Goomi |
| 3 → 4 | jump apex, fall | 16–20 | hanging, climbing |
| 5 | land | 21 | game over |
| 6 | spring bounce | 22–27 | the same as 0–5 while carrying an enemy |
| 7 | flutter (float) | 28–33 | idle animations (after 20 s idle) |
| 8 | wind bullet (grabs an enemy) | 34–37 | doors, ladders |
| 9, 10 | throw (on the ground, in the air) | | |
| 11 | double jump (throw downwards) | | |

## 2. The Moo

All Moos share the enemy animation table at `0x081898E0` (Vision 1-1 points its slots 10–13 at
it). A probe of Vision 1-1's four Moos in flycast showed what they do:

- **walk** = state 1: 4 frames × 8 game frames, looping; 0.5 px per game frame;
- at a **ledge**: state 0, a 9-frame look-around (10, 7, 7, 7, 7, 7, 7, 7, 10 frames) that ends in
  "goto 1"; then the Moo turns and walks on;
- at a **wall**: it turns on the spot;
- frames face right; the game sets hFlip to walk left.

The menu Moo does the same on the LCD: it walks left from the right edge, looks around at the left
edge (the "ledge"), turns, walks right and turns on the spot at the right edge (the "wall"). Its
sequencing is the same code as the game's slot update (`vmulcd_anim_tick`, checked against the ROM
in §6), its palette is Vision 1-1's Moo palette, and it restarts from its first walk frame each time
a menu begins.

## 3. Menu or gameplay

Decided each frame from the game's state, not from the picture:

- **Menu** when the scene handler (`gCallbackQueue.current[1]`) is one of 30 named menu, transition
  or cutscene handlers (`hal_vmu_lcd.c`: boot, Namco, title, file select, button configuration,
  delete-save, world map, vision select, the story cutscene `sub_0804EB64`, pause, game over, the
  all-visions-cleared screen, and their fade transitions), or when there is no live Klonoa: no level
  (`gUnk_03004C20.level` = 0), entity 0 not `ENTITY_ID_KLONOA`, slot 0 not using one of Klonoa's
  three tables, or slot 0 unused.
- **Text boxes** (`TextBoxHandler`, `TextBoxRestoreGameplay`) keep whatever was showing. The vision
  select opens one with the next level already chosen, before the level loads; signs open one in
  a level.
- **Gameplay** otherwise. On the route that is the level loop (`sub_0800A804`); by the same rules
  it also covers room rotation, boss scenes, the death fade and the title screen's attract demo,
  which plays a real level — none of them exercised here.

On the test route this gives one clean switch each way (§6): menu until the level loop starts
at frame 2193, the Moo during the pause menu (3303–3422), Klonoa again from 3423. The picture
changes in the frame the mode changes.

## 4. Picture and update policy

**Conversion.** A palette colour is *ink* (a dark LCD pixel) when its BT.601 luma, on a 0–255
scale, is below **100**; transparent is never ink. No dithering. With that threshold Klonoa's
outline, dark grey, brown, both blues and the three reds are ink (`.##...#...##.###` for colours
0–15) and his white ears, face, yellow and light blue are not; the Moo keeps its outline, dark reds
and blue hands (`.###....##.#....`). Thresholds 64 and 140 were compared on the idle, run and jump
frames: 64 loses Klonoa's body to a bare outline, 140 fills the Moo in solid.

**Placement.** 1:1, no scaling or cropping: the sprite's 32×32 box fits the LCD's 32 rows exactly.
Klonoa's box sits at columns 8–39; the Moo's moves between columns 0 and 16 (its box). hFlip mirrors
the box in place, as OAM does. The bitmap is built top row first, MSB = left (`vmufb_t` layout),
and turned 180° for a VMU in a controller as `vmufb_present()` does.

**Updates.**
- A frame is converted once (64-entry cache keyed by source address and palette; frames in RAM are
  also keyed by a checksum, since the hover-board goal table points some frames at decompressed
  data).
- The picture is rebuilt only when the frame, the facing or the Moo's position changes, and sent
  only if its bytes differ.
- Each VMU gets at most one write every 2 game frames (30 Hz). KallistiOS states no LCD rate limit;
  this is a conservative choice, not a measured one. Only a picture that is replaced after a single
  frame can be skipped (the game sometimes changes state in two consecutive frames, e.g. flutter →
  fall; the Moo's walk frame can change one frame after it steps); the newest picture always goes
  out within 2 frames, so the screen is never left stale and pictures never appear out of order.
- `vmu_draw_lcd()` only *try*-locks the VMU's maple frame. If it is busy — the previous write still
  in flight, or a save writing a block to that VMU (block writes hold the frame for all four phases
  and the sync) — the write is retried next frame. The game thread never waits on the maple bus.
- A VMU that disappears and comes back is sent the current picture at once. Missing VMUs, devices
  without an LCD and non-VMU LCD devices (which KOS refuses) are skipped silently.

**What the LCD does not show.** Colour and shading (one bit per pixel); where Klonoa is in the level
or on screen; the camera, backgrounds and every other sprite — including an enemy he carries, which
is its own sprite; the per-frame position nudges in the frame lists (in the normal table only state
6, the spring bounce, uses them: −4, −8, +4 px vertically, which would push him off the 32-row
screen); the invulnerability blink (the last frame stays up instead of flickering); affine rotation and scaling (boss levels,
room rotation); fades, mosaic and palette effects. On menus the Moo is a mascot: it does not
reflect anything on the menu screen.

## 5. Cost

Measured with the hook's own timer in scripted runs (`VMULCD stats` lines, every 1,200 frames) and
with `tools/perfstats.py` on `tests/perf.txt` (10,000 frames), against the build before this feature:

| `tests/perf.txt` (10,000 frames, sound on) | Before | With the VMU screens |
|---|---|---|
| Gameplay (frames 2162–9961) | 59.8 fps; slowest window 58.1; worst frame 58.3 ms | **59.8 fps**; slowest window 58.1; worst frame 56.6 ms |
| Boot → level entry | 58.7 fps; slowest window 54.1 | 58.7 fps; slowest window 54.1 |
| Game / render / texture / submit per gameplay frame | 0.21 / 0.45 / 3.31 / 3.04 ms | 0.21 / 0.45 / 3.31 / 3.04 ms |
| Idle time left per gameplay frame | 8.15 ms | 8.09 ms |
| The hook itself (`VMULCD stats`) | — | **39 µs** per frame in gameplay, 34–48 µs on menus; worst single frame 0.64 ms (a frame that converts new sprites) |
| Scene changes (frame numbers of all 20) | | identical |

Over the run it converted 40 distinct frames in all, sent 4,886 pictures to the two VMUs and was
never refused or kept waiting (`busy=0 failed=0`). The hook runs inside the beam emulation's
frame end, so its time comes out of the idle time, not out of the game's.

## 6. Tests and results

**Host test** (`make -C port/dc test-vmu-lcd`, reads `baserom.gba`): 82 checks, all pass.

- palette thresholds against a floating-point luma reference;
- a synthetic sprite through the converter (tile order, nibble order, ink only), placement at
  columns 8–39, x = 0 and x = 16, and hFlip;
- all 150 Klonoa frames of the normal table and the 8 Moo frames against a per-pixel reference
  decode (161,792 pixels, 0 differ);
- frame sequencing: all 38 Klonoa states and both Moo states, 2,000 ticks each, against a timeline
  expanded straight from the ROM's frame lists (loops, gotos, holds, hides), plus the Moo timings
  measured in flycast;
- the Moo's patrol: stays on screen, 0.5 px per frame, still while looking around, looks around at
  the left edge, turns at both, walk frames advance;
- the mode rules (menu handler, text box over gameplay and over a menu, no level, wrong entity,
  wrong table, unused slot, the hover-board table);
- a menu → gameplay → pause → gameplay drive: the LCD always shows the game's current frame and
  facing, redraws only on change, turns at once, converts each frame once, switches to the Moo
  in the same frame, shows nothing new while slot 0 has no frame yet;
- RAM-sourced frames re-converted when their bytes change; unmapped sources rejected;
- the schedule: ≤ 1 write per 2 frames per VMU, the last picture always delivered, no resend of an
  unchanged picture, immediate resend after a replug.

**Flycast route** (`tests/vmu_lcd.txt`: boot → title → file select → cutscene → world map →
vision select → Vision 1-1; run, jump, flutter, wind bullet; pause and resume; run left into the
Moo, grab it with the wind bullet, carry it, jump and throw it in mid-air; jump, flutter, land,
idle). Built with `VMULCD_LOG=1`, run under flycast with the `FC_VMULCD` hook
(`tools/flycast-test-hooks.patch`), which writes every LCD bitmap flycast's VMU receives. Checked by
`tools/vmulcd.py`:

| Check | Result (3,800 frames) |
|---|---|
| Mode switches | menu from frame 1 (boot) through title, file select, cutscenes, world map, vision select and its text box; **gameplay** at 2193 (level loop starts); **menu** at 3303 (pause menu); **gameplay** at 3423 (resumed). No other switches |
| Transport | 1,936 writes (968 to each of the two VMUs flycast plugs into controller A); all arrived, in order, and decode to exactly the logged picture; none unexpected; none refused or busy |
| Conversion | all 1,035 pictures recomputed from the ROM by the checker's own decoder: 0 differ |
| Timing | 487 animation-frame durations compared with the ROM timers (Klonoa, plus the menu Moo): 0 differ. Gameplay frames whose game loop skipped its animation update are logged and allowed for; on this route there are two, each at the start of a fall after a flutter |
| Delivery | no picture late or lost; in gameplay 0 of 254 pictures skipped; on menus 67 of 781 skipped per VMU (a new Moo walk frame one frame after a step, replaced by the next step a frame later) |
| Klonoa states shown | 0 idle, 1 run, 2 jump, 3 jump apex, 4 fall, 5 land, 7 flutter, 8 wind bullet, 10 throw in the air, 12 damage (twice; the second time he runs into the Moo), 22 carry idle, 23 carry run, 24 carry jump — facing both ways |
| Moo | walk and look-around, facing both ways, during every menu stretch including the pause |

`--out` draws one picture per state as decoded from what flycast's VMUs received, and the whole
sequence. Those images are made from the game's sprites, so they are not in this repository.

**Regression.**
- `tools/regress.sh` passes with the VMU screens on: 19 checkpoints, 0 pixels more than one colour
  level apart between the PowerVR and software renderers (17 bit-identical, as before).
- Game logic unchanged: the same route built with `VMULCD=0` (the hook returns at once) gives 38
  frame dumps (both renderers) bit-identical to the run with the VMU screens on; on
  `tests/perf.txt` all 20 scene changes fall on the same frames as in the build before this
  feature.

**Not verifiable here.**
- No real VMU or Dreamcast was available. Flycast accepts any LCD write rate and shows every
  bitmap at once; a real VMU's LCD responds slowly and its maple timing is not flycast's. The
  30 Hz cap is a guess at a safe rate, not a measurement.
- Which way up the picture appears on real hardware rests on KallistiOS's `vmufb_present()`
  convention (rotate 180° unless the controller and VMU face opposite ways), which the port copies;
  flycast reports both VMUs as rotated and does not show which way up is right.
- Interleaving with VMU saves, before they were merged: the LCD path never touches storage and only
  try-locks, so it cannot stall a save or the game. A save's block write may wait up to one maple
  cycle (~16 ms) if an LCD write to that VMU is in flight. Saves were not exercised together with
  the LCD at this point (the save feature was being added separately).

**Later, with saves merged** ([report 5](vmu-saves-display.md)). On this route the game saves three
times to VMU A1, at frames 1225, 1320 and 1475, each taking about 1–1.5 s.
- While a save writes, the LCD's sends to that VMU find its maple frame locked and get
  `MAPLE_EAGAIN`. Pictures the game replaces meanwhile are never sent; the newest one goes as soon
  as the save releases the VMU. VMU A2 is unaffected.
- `tools/vmulcd.py` had counted busy as −1 (`MAPLE_EFAIL`) rather than −2 (`MAPLE_EAGAIN`), so it
  reported those pictures as lost. It now counts them separately ("replaced while a VMU was busy").
- Every save on the route was written and verified.

## 7. Files and commands

- `port/dc/src/hal_vmu_lcd_core.[ch]` — conversion, cache, the game's frame sequencing, the Moo,
  mode choice, send schedule (no Dreamcast code; also built by the host test).
- `port/dc/src/hal_vmu_lcd.[ch]` — reads the game's state, finds the VMUs, sends; one call in
  `hal_frame.c` (`finish_frame`), one in `main_dc.c`.
- `port/dc/tools/vmu_lcd_test.c` — the host test; `--sheet out.pgm` draws every converted frame.
- `port/dc/tools/vmulcd.py` — checks a logged run and draws what the VMUs showed.
- `port/dc/tests/vmu_lcd.txt` — the route.

```sh
make -C port/dc test-vmu-lcd
make -C port/dc RENDERER=pvr SCRIPT=tests/vmu_lcd.txt VMULCD_LOG=1
FC_VMULCD=lcd.txt FLYCAST=<flycast with the hooks> run-elf.sh port/dc/build/klonoa-dc.elf 400 out
python3 port/dc/tools/vmulcd.py out/serial.txt --capture lcd.txt --rom baserom.gba \
    --map port/dc/build/klonoa-dc.map --out out/lcd --expect-states 0,1,2,3,4,5,7,8,10,22,23,24 --expect-moo
```

Build options (separate block at the end of `port/dc/Makefile`): `VMULCD=0` leaves the VMU screens
alone; `VMULCD_LOG=1` logs every picture and write (scripted runs only: the serial output costs about
1.4 ms per frame while the menu Moo walks).
