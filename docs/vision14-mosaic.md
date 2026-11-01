# Klonoa: Empire of Dreams → Dreamcast: technical report 6 — Vision 1-4's broken last room

*2026-11-01. All results come from scripted runs under flycast (HLE BIOS) on Linux. No real
Dreamcast was used. The bug was reported from Flycast 2.7 on Windows with a real BIOS.*

## Summary

**The symptom, as reported.** Playing Vision 1-4 normally, things go wrong from the level's last
room onwards:
- the backgrounds turn to streaks of garbage, and the picture seems to shift against Klonoa;
- the sound stutters;
- it stays broken for the rest of the session;
- once, Flycast stopped with "Fatal: SH4 exception when blocked".

**It is a bug in this port's PowerVR renderer, not in the game.** At the same frames, the port's
software renderer draws the correct picture from the same game state. The original game is not
affected.

**The cause:**
- During Vision 1-4's room transitions the game writes `MOSAIC = 0x1110`: blocks one pixel wide
  and two tall.
- The renderer's mosaic buffer was 128 × 128 blocks, sized for blocks of at least 2 × 2. This
  mosaic needs 240 × 80.
- The buffer overflowed into the BG texture records stored after it, overwriting their texture
  pointers and sizes.
- Every background drawn afterwards was drawn from wherever those garbage pointers led, and
  uploads went there too.

**The fix** (`hal_video_pvr.c`): a 256 × 256 mosaic buffer and texture, which holds any block
size the GBA allows. Only the part that is in use is cleared and uploaded.

**The test.** `tools/vision14_regress.sh` plays Vision 1-4 to its end and on into Vision 1-5, and
compares the PowerVR picture with the software renderer at 19 frames.
- Before the fix: the picture breaks at the last room's transition and stays broken.
- After: every frame is within one colour level, and the game plays on into Vision 1-5.

## 1. Reproduction

A repeatable route, with no editing of the running game's memory:

1. **A saved game.** `tests/save.txt` from an empty VMU: title, file select, new game, on to the
   world map, which the game saves.
2. **A start at the vision select.** `tools/vmuimg.py vision IMG 1 4` edits that save *file*.
   - The game file now continues at World 1's vision select, with Visions 1-1 to 1-3 marked
     cleared.
   - It writes the game's own records with their own checksums, then the port's CRCs.
   - Nothing else changes.
3. **The level.** `tests/vision14.txt`:
   - continue, step right four times to Vision 1-4, and confirm;
   - then generated input (seed 1): mostly run right, jump, flutter and wind bullet, with some
     left, up, down and pause.
   
   On the hover board that clears the level: room 1 at frame 2593, room 2 at 3761, room 3 (the
   last) at 5926, "Vision clear", the vision select at 6064, then Vision 1-5 from 6589.

It is deterministic: every run with the same script and save reached the same rooms at the same
frames.

## 2. Root cause

**Where it went wrong first.** `build_mosaic` builds one texel per mosaic block into `mosbuf`, at
the twiddled (Morton-order) index `spread(by) | spread(bx) << 1`. It is sized for `MOS = 128`, so
128 × 128 blocks.
- That is enough while both block sizes are at least 2: 120 × 80.
- With blocks one pixel wide, bx runs to 239. Once bx reaches 128, the index is at least 32,768,
  past the end of the 16 KiB buffer.

An instrumented build logged the first such write:

```
DBG OOB mosaic frame=3575 bg0 bw 1 bh 2 bx 128 by 0 index 32768
```

That is the transition from room 1 to room 2. A guard on the BG texture records, checked after
each step of the frame's texture sync, showed where the writes landed:

```
DBG guard mosaic frame=3575 bg1 alloc 65536->0 tex 0xa42fc1e0->0x0 cfg 0000
DBG guard mosaic frame=4766 bg1 alloc 65536->0 tex 0xa4328280->0x0 cfg 0000
DBG guard mosaic frame=4767 bg1 alloc 65536->319623949 tex 0xa43382a0->0x160016 cfg 001e
```

- At 3575 the overflow zeroed BG1's record. The next frame simply allocated a new texture, so
  nothing showed, but 64 KiB of texture memory leaked.
- At 4767, entering the last room, it wrote mosaic texel values into the record:
  - texture pointer 0x00160016;
  - "allocated" size 319,623,949;
  - a layout that never matches the BG again.

From then on every frame rebuilt that BG through the bogus pointer and drew it from that address.
Nothing ever repaired the record, so it stayed broken across later rooms, the vision select and
Vision 1-5.

**Why 0x1110.** The transition ramps `gMosaicSize` from 1 up to 16 and writes
`REG_MOSAIC = MOSAIC_SET(gMosaicSize, ...)` (kleod `src/wait_for_next_frame.c`). At 16 each field
carries into the next one: 0x10 + 0x100 + 0x1000 + 0x10000, which is 0x1110 in 16 bits. On the GBA
that is a legal mosaic: BG blocks 1 × 2, OBJ blocks 2 × 2. It lasts two frames and nobody sees
it. This port's software renderer handles any block size and was always right.

**Why Vision 1-4.** The instrumented build checked every mosaic from boot onwards. On this route
(boot, title, file select, vision select, then Vision 1-4), the first BG mosaic with a one-pixel
side is Vision 1-4's first room transition, at frame 3575. The second is the transition into its
last room, at 4766–4767. Where the overflow lands depends on the texel values: the first one
happened to be harmless, the second was not. Other levels' transitions were not instrumented.

**Why the picture shifts, and the sound and the crash.** Neither the shift nor the sound was
measured to have a cause of its own.
- **The shift:** layers are drawn from the wrong texture memory, with sizes taken from a garbage
  record, so they no longer line up with the sprites.
- **What the uploads hit:** after the record is overwritten, every frame uploads that BG's tiles
  through its pointer, which is whatever texel values the overflow left. In the instrumented build
  it was 0x00160016.
  With the MMU on (the port maps the GBA address space through it), a low address like that has no
  TLB entry, so on hardware the write would raise a TLB-miss exception. The flycast build used
  here raised none, and nothing else visibly changed. Another run leaves different values.
- **The reported sound stutter and "exception when blocked":** stray writes like these are the
  most likely explanation. A write that lands in sound hardware would disturb the sound, and a
  TLB miss that the exception handler cannot service ends in a fault inside the handler, which is
  what "exception when blocked" means. Neither was reproduced here. On this route the music kept
  time before and after the fix: no underruns, and the same number of lag frames.

## 3. The fix

`hal_video_pvr.c`, `build_mosaic`:
- `MOS` is 256. The mosaic buffer is 64 KiB, and each mosaicked BG's texture 64 KiB of PowerVR RAM.
  That holds any block size: at most 240 × 160 blocks.
- Only the twiddled square that holds the blocks in use is cleared and uploaded: `span × span`,
  the smallest power of two covering both block counts. That is 16 KiB for blocks of at least
  2 × 2, as before, and 64 KiB only for one-pixel blocks.
- Nothing else changes: the quad's texture coordinates already scale with `MOS`.

Also in this change: `SCENE` log lines carry `world=`, `level=` and `room=`, which the test uses.
Their only other reader, `tools/vmulcd.py`, matches the line's start.

## 4. Tests and results

**`tools/vision14_regress.sh`** (§1), with 19 dumped frames compared against the software renderer:

| | Before (`0708135`) | After |
|---|---|---|
| Reaches Vision 1-4's last room, then Vision 1-5 | yes | yes |
| Frames more than one colour level off | 11 of 19, every one from frame 4,800 on (14,741–26,225 of 38,400 pixels) | 0 of 19 |
| Result | 1 check failed | all checks passed |

**Densely dumped run** (an earlier save of the same kind):
- frames 3,540–3,615 and 4,730–4,800 every 5 frames, which covers both transitions;
- every 100 frames from 3,700 to 4,800;
- then frames 4,800 to 9,500, through the last room, "Vision clear", the vision select and
  Vision 1-5.

Before the fix the picture was 0 pixels off up to frame 4,700, and 42–69% of pixels off from
4,800 on, in every scene. After it, every compared frame is 0 pixels off.

**Performance** (the same route, 12,000 frames, no dumps, sound on; before = `0708135`):

| | Before | After |
|---|---:|---:|
| Mean frame rate / slowest 120-frame window | 57.8 / 46.8 fps | 57.8 / 46.8 fps |
| Worst single frame | 59.5 ms | 54.2 ms |
| Lag frames (sound kept in time) / underruns | 442 / 0 | 441 / 0 |

The slowest windows are this hover-board level's own heavy scenes, the same before and after. The
mosaic upload costs the same as before, except during the two frames with one-pixel blocks.

**Regressions**, on the fixed code:
- **Visual** (`tools/regress.sh`, boot to Vision 1-1): 19 checkpoints, 0 pixels more than one level
  apart, letterbox bars black.
- **Sound** (`tools/audio_regress.sh`): all checks pass.
- **`tests/perf.txt`** (10,000 frames):
  - gameplay 59.8 fps, slowest window 58.1;
  - boot to level 57.7 fps, slowest window 49.0;
  - 0 underruns;
  - the same as at `5dd05ff` (report 5).
- **Memory:** PowerVR texture RAM free at the end is 4,751 KiB, against 4,943. That is the three
  mosaic textures at 64 KiB each instead of 16 KiB.

## 5. Limitations

- **No GBA emulator comparison.** The defect is in Dreamcast-only code. The game's state at the
  failing frames was checked against the port's own software renderer, an independent GBA
  picture model fed the same state. mGBA was installed, but this build has no scripted input to
  reach the room, so the original ROM was not run there.
- **No real hardware**, and not the reporter's setup (Flycast 2.7 on Windows with a real BIOS).
- **The sound stutter and the "exception when blocked" crash were not reproduced.** They are
  explained (§2), but not demonstrated.
- **The route uses generated input.** It clears the level reliably from this save, but it is not
  a human play-through, and Vision 1-4 was not played to its end from a new game.
