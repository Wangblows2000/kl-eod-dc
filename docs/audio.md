# Klonoa: Empire of Dreams → Dreamcast: technical report 3 — sound

*2026-11-01, updated the same day with §8 (the fix for the crunchy, bassy output; the figures in
the summary, §2, §5 and §6 are from the code after it). All figures come from the scripted runs
and host tests in §7, under flycast. Nothing here has been run or heard on real Dreamcast
hardware.*

## Summary

The port now plays the game's music, sound effects and Game Boy-style PSG voices through the
AICA. `AUDIO=0` builds the previous silent port.

| | Silent (`AUDIO=0`) | With sound |
|---|---|---|
| Gameplay, Vision 1-1 (frames 2162–9961) | 59.8 fps; slowest 120-frame window 58.0; worst frame 57.9 ms | **59.8 fps**; slowest window 58.1; worst frame 58.3 ms |
| Boot → level entry (frames 2–2161) | 58.8 fps; slowest window 55.1 | 58.7 fps; slowest window 54.1 |
| Sound CPU time per gameplay frame | — | 1.5 ms: engine 0.12 + hardware model 1.05 + stream 0.30 |
| 30,000-frame soak | completed | completed; memory flat; 1 gap (244 samples, ~6 ms, during silence at the title scene's load), none in 27,720 gameplay frames |
| Visual regression (19 checkpoints) | passes | passes; all dumps bit-identical to the silent run |
| Sound, stage by stage (`tools/audio_regress.sh`, §8) | — | passes: the AICA plays what the port produces (−50 dB or better), the Namco voice's engine output is byte-identical to the cartridge's |

**How sure this is:**
- **Bit-exact against the ROM's own code.** The engine's assembly half — the sequencer, every track
  command, note allocation, envelopes, the DirectSound mixer and reverb — was translated to C. It
  was run next to the cartridge's own code on an emulated ARM, and compared after every frame. All
  165 songs give identical `pcmBuffer` bytes and identical channel, track and register state for
  1,200 frames: played alone, with sound effects started mid-song, and with reverb forced to
  maximum (§3).
- **Spec-checked, not hardware-checked.** The GBA's sound hardware (PSG channels, DAC, routing) is
  modelled from GBATEK and mGBA's mixing levels. It passes 30 host checks against those figures
  (§4), but nobody has compared it with a real GBA.
- **Emulator only.** Latency, drift and underrun figures are flycast's.
- **Output path checked sample by sample (§8).** What the AICA plays now matches what the port
  produces, to −50 dB or better; until 2026-11-01 KallistiOS's stereo split swapped the channels
  and reversed every pair of samples, which made the sound muffled and crunchy.

## 1. What the game does

Klonoa uses the stock MP2K ("m4a", Sappy) engine. It has two halves:

- **C, already compiled into the port:** in kleod's `src/m4a.c`.
  - the public API (`m4aSongNumStart`/`Stop`, volume, tempo, pan and fade control,
    `m4aSoundVSyncOff`/`On`);
  - initialisation (`SoundInit`, `SampleFreqSet`, `m4aSoundMode`);
  - `CgbSound`, which drives the four PSG channels by writing the GBA sound registers.
- **ARM/Thumb assembly, stubbed until now:** in `asm/m4a0.s`.
  - the sequencer (`MP2KPlayerMain` and its 20 track commands, including note-on);
  - `TrackStop`;
  - `m4aSoundVSync`;
  - the mixer `SoundMain`/`SoundMainRAM`.

**Per frame:**

1. The main loop ends with `m4aSoundMain()`. On a lag frame the VBlank handler calls it instead.
2. `m4aSoundMain()` runs the sequencer, then `CgbSound`, then the mixer.
3. The mixer adds every active DirectSound channel into one 224-sample segment of `pcmBuffer`. The
   buffer is a ring of 7 segments: first half FIFO A (right), second half FIFO B (left). The mixer
   always works one segment ahead of playback.
4. On the GBA, DMA1/DMA2 play one segment per frame, restarted by `m4aSoundVSync` every 7 frames.
5. Loads bracket themselves with `m4aSoundVSyncOff`/`On`, which stop DMA and zero the buffer.

**Formats and behaviour actually used:**

- **DirectSound samples:** 8-bit signed PCM with a base rate, loop start, length and loop flag.
  The game has 66 of them.
- **Mixing:** 8 channels at 13,379 Hz (`DEFAULT_SOUND_MODE`), master volume 15.
  - Pitched voices use linear interpolation; fixed-rate voices (drums) step one sample per output
    sample.
  - Each note has an attack/decay/sustain/release envelope and an optional pseudo-echo.
  - Songs can set reverb, which mixes in the buffer's own earlier contents.
  - Sums wrap at 8 bits instead of clipping.
- **PSG:** square 1 (with sweep), square 2, programmable wave and noise voices appear in all 9
  voicegroups. `CgbSound` uses the hardware envelope and length counters and retriggers through
  NRx4 bit 7.
- **Game logic and sound state:** the game only sends commands and volume changes. It never reads
  the players' state back, so sound cannot change scene timing.

## 2. Design

```
game thread, once per frame                         poll thread, every ~5-10 ms
───────────────────────────                         ─────────────────────────────────────────────
m4aSoundMain ─ hal_m4a.c: sequencer, CgbSound,      snd_stream_poll ─► callback: split the ring's
     │          mixer (into the game's pcmBuffer)        L,R into two buffers, write them to sound
VBlank handler: m4aSoundVSync (DMA ring position)        RAM (or fade to silence on underrun)
     │                                              if the next sound frame is half a frame
hal_audio_frame (hal_audio.c)                       overdue ("lag frame", as the GBA's VBlank
  ├ segment DMA plays this frame                    interrupt would run during a long frame),
  │   (none while DMA1/2 are stopped)               interrupts off: m4aSoundVSync, one block,
  │                                                 SoundMain before or after (one mix per VSync)
  │                                                            │
  ├ hal_gbasnd.c: PSG from the registers +                     │
  │   DirectSound, GBA DAC → 672 stereo samples                │
  └ append ──────────────────► static ring (8,192 frames) ◄────┘
                                        │
                               AICA: 2 × 2,048-sample buffers, 16-bit stereo, 40,280 Hz
```

- **Software mixer, hardware output.**
  - MP2K's sound depends on things only its own mixer reproduces: 8-bit wraparound, reverb from
    the buffer's earlier contents, envelopes quantised to frames, and fixed-rate drums. Mapping
    notes onto AICA voices would lose all of these.
  - The mixer is cheap: at most 8 channels × 224 samples a frame.
  - So the AICA does what the GBA's FIFOs and DAC did. It plays one stereo stream, through
    KallistiOS `snd_stream` (PCM16).
- **Output rate: 3 × 224 samples per Dreamcast frame (40,280 Hz).**
  - Each DirectSound sample is held for three output samples, as the GBA holds each FIFO sample
    until the next timer tick.
  - The PSG is generated at the full rate, area-sampled, so square, wave and noise tones up to the
    GBA's limits do not alias badly.
- **Buffer ownership and lifetime.**
  - The ring and the callback's two channel buffers are static.
  - Sample data is ROM data the game never frees.
  - The stream uses KallistiOS's direct callback: the port splits L and R itself and writes each
    channel's buffer into sound RAM with plain G2 writes (`spu_memload`) before returning, so
    nothing reads the port's memory later. It does not hand KallistiOS interleaved stereo, whose
    16-bit split routine in v2.3.0 swaps the channels and every pair of samples (§8).
- **Keeping real time.**
  - The engine advances once per emulated frame, and an emulated frame lasts as long as the game
    takes over it. A Dreamcast frame longer than 1/60 s would leave the music behind and the ring
    dry.
  - The GBA avoids this with its VBlank interrupt, which keeps firing while the main loop is busy.
    The game's VBlank handlers then run the sound engine themselves on such lag frames:
    `VBlankIntr_Common` calls `if (!gFrameFinished) m4aSoundMain()`.
  - The port does the same. When a sound frame is half a frame overdue, the poll thread runs the
    lag frame: `m4aSoundVSync` and one block, with `SoundMain` before or after it, so that every
    VSync has exactly one mix behind it. The game mixes once per frame right after its VBlank wait
    (`CommonWaitForNextFrame`); a lag frame before that point mixes first, one after it mixes
    afterwards (§8).
  - After its own frame, the game thread catches up anything left.
  - A lag frame's block is queued only while the ring is below its target plus one frame;
    otherwise it is rendered and dropped. The engine still advances, so the music stays in time
    without adding latency.
  - More than 8 frames behind, as in a multi-second stall, the cadence resynchronises instead.
- **Synchronisation.**
  - Single core, two threads. The poll thread has the higher static priority, but KallistiOS v2.3
    ages the priority of waiting threads, so the game thread can still preempt it (§8). Each lag
    frame therefore runs with interrupts off, as the GBA's interrupt runs to completion while the
    main loop waits; one takes well under a millisecond.
  - Lag frames stay out while the game thread is inside the VBlank handler or its own audio frame
    (`hal_audio_busy`), or inside an MP2K call. For the last, the engine's own lock word suffices:
    on the GBA it already protects the engine against interrupts.
  - So the two producers never interleave. The ring's indices are each written by one side, after
    the data they cover, behind a compiler barrier.
  - Nothing blocks, and no audio work runs in an interrupt handler.
  - What the AICA plays is tested against what the model produced, sample by sample, in
    `tools/audio_regress.sh` (§7, §8).
  - Only the game thread writes to the serial log. An underrun found by the poll thread is
    reported by the game thread's next frame, so it can never land inside a frame dump.
  - The game never reads sound state, so none of this changes game logic. Frame-indexed tests stay
    exact, and the visual regression's dumps are bit-identical to the silent build's.
- **Underruns and drift.**
  - If the ring does run dry, the callback still returns the whole request, fading the last sample
    to zero, so the AICA never replays stale data.
  - The next block then refills the ring to its target with silence instead of speeding up.
  - Each gap is logged with its frame, and as audible or silent depending on whether sound was
    playing just before it.
  - A trim of at most ±4 samples per frame (0.6%) keeps the ring near 1,600 frames, absorbing the
    difference between the video and audio clocks.
- **Latency:** about 40 ms in the ring plus 25–51 ms in the AICA buffer, so 65–90 ms from mix to
  output. The GBA's is 17–33 ms. These are design values, not measurements, and both buffer sizes
  are constants at the top of `hal_audio.c`.
- **Disable switch:** `AUDIO=0` removes the engine port, the model, the stream and the thread. The
  build is then the previous silent one (§5).
- **No renderer changes.** The frame loop's integration point is the call after the VBlank
  handler in `hal_frame.c`, with the busy flag around it.

## 3. The engine port and its differential test

- **Translation.** `src/hal_m4a.c` follows `asm/m4a0.s` routine by routine. It keeps the same
  control flow, the same arithmetic (including the mixer's packed-byte wraparound and the reverb's
  rounding) and the same structure offsets; `_Static_assert`s check the offsets.
- **Test setup.** `tools/m4a_difftest/difftest.py` builds that file for ARM with
  `arm-none-eabi-gcc` and loads it next to the cartridge image in Unicorn. Both runs:
  1. execute the ROM's own `m4aSoundInit` and `m4aSongNumStart`;
  2. then, every frame, call `m4aSoundVSync` and `SoundMain`. One run uses the ROM's code, the
     other the port's: the jump table, `plynote` and the player chain are pointed at the compiled
     functions.
- **What is compared each frame:** `pcmBuffer`, all mixer channels, the PSG channels, every track
  and player, the PSG registers and the DMA registers.

| Run | Songs | Frames each | Result |
|---|---:|---:|---|
| Each song alone | 165 | 1,200 | 165/165 identical |
| Each song with three more started at frames 90, 200 and 330 (channel stealing) | 165 | 1,200 | 165/165 identical |
| As above, with reverb forced to 127 | 165 | 1,200 | 165/165 identical |

A test run is only meaningful if it can fail, so I injected single faults into the C and checked
that the comparison reported them:

| Fault | Caught |
|---|---|
| LFO speed initialised to 21 instead of 22 | yes, frame 0 |
| Mixer fraction mask `~0x3F000000` instead of `~0x3F800000` | yes, frame 0 |
| Reverb rounding removed | yes |
| DMA counter reload off by one | yes |
| Release-to-echo threshold `<` instead of `<=` | yes (143 songs) |
| Pseudo-echo length check | **no** |

The last one means no Klonoa song uses the pseudo-echo, so that branch is translated but not
exercised.

## 4. The GBA sound hardware model and its tests

`src/hal_gbasnd.c` covers:

- **Triggers and registers.** The PSG registers are read once per frame, after `CgbSound`. NRx4
  bit 7 is a trigger and is cleared again, as it reads back 0 on hardware. MP2K writes each
  channel at most once per frame.
- **Channels, per GBATEK:**
  - squares at 131072 / (2048 − x) Hz with 12.5/25/50/75% duty;
  - the 32-step wave at 65536 / (2048 − x) Hz, with NR32's 0/25/50/100% and forced 75% volume;
  - the 15- or 7-bit LFSR at 524288 / r / 2^(s+1) Hz;
  - the 512 Hz frame sequencer: length at 256 Hz, sweep at 128 Hz, envelope at 64 Hz;
  - channel 1's sweep, which writes the frequency back to NR13/14 and silences the channel on
    overflow;
  - DAC-off silencing.
- **Levels, as mGBA:**
  - PSG: per side Σ channel × 8 × (1 + NR50 volume) >> (4 − SOUNDCNT_H ratio);
  - DirectSound: samples << 2 at 100% or << 1 at 50%, routed by SOUNDCNT_H bits 8–13;
  - master enable SOUNDCNT_X;
  - DAC: SOUNDBIAS bias, clip to 10 bits, reduce to the SOUNDBIAS resolution, then a ~25 Hz DC
    blocker for the GBA's AC coupling.

`make test-audio` builds `tools/gbasnd_test.c` on the host and runs 30 checks. All 30 pass:

| Area | Checks |
|---|---|
| Square | frequency 439.84 Hz and 128 Hz from the register; 50% and 12.5% duty; level 15 at 100% = ±120 DAC steps |
| Mixing | NR51 routing; SOUNDCNT_H 50%; NR50 volume; master off |
| Envelope | decreasing 15 → 0 in 15/64 s; increasing 0 → 15 at 7/64 s per step |
| Length | 32 → 125 ms |
| Wave | 219.9 Hz; 50% and 75% codes; DAC off |
| Noise | 7-bit LFSR repeats every 127 steps; level |
| Sweep | write-back of 1536; overflow silence |
| DirectSound | routing, 100%/50% level, 3-sample hold, DMA stopped → silence |
| Registers | trigger bit cleared |

Writing these tests found one bug, since fixed: DirectSound samples were held 4-3-3 instead of
3-3-3, from a rounding error.

## 5. Performance and soak

Both columns come from the code after the output fix of §8 (the silent build is unchanged by it).
The silent column is the `AUDIO=0` build on `tests/perf.txt`; the sound column is the default
build on the same 10,000-frame route; the soak is 30,000 frames with sound. §8 compares the sound
column with the code before the fix.

| 10,000-frame route (`tests/perf.txt`) | Silent | With sound |
|---|---:|---:|
| Gameplay (frames 2162–9961): mean / slowest window | 59.8 / 58.0 fps | 59.8 / 58.1 fps |
| Gameplay: worst frame (level entry) | 57.9 ms | 58.3 ms |
| Gameplay: idle time left per frame | 9.36 ms | 8.15 ms |
| Boot → level (frames 2–2161): mean / slowest window | 58.8 / 55.1 fps | 58.7 / 54.1 fps |
| Frames over 33 ms (all scene loads) | 13 | 14 |
| Main RAM heap in use | 730 KiB | 768 KiB |

Where the sound time goes (means of 120-frame windows):

| | Boot → level | Gameplay |
|---|---:|---:|
| MP2K engine (`SoundMain`: sequencer, `CgbSound`, mixer) | 0.15 ms | 0.12 ms |
| Hardware model + ring (`hal_audio_frame`: PSG, DAC, block copy) | 0.84 ms | 1.05 ms |
| Stream polling, including lag frames run by the poll thread | 0.29 ms | 0.30 ms |
| of which: writing the two channels to sound RAM (`spu=` in the AUDIO lines) | 0.14 ms | 0.16 ms |

- The model costs more in gameplay because Vision 1-1's music keeps PSG channels sounding.
- The engine is cheap because Klonoa plays at most 8 DirectSound channels at 13,379 Hz.
- The slowest windows are the text-box scene (frames ~1950–2160). It already ran at 55 fps
  without sound; sound takes it to 54.1.
- Stream polling costs 0.13 ms a frame more than before §8's fix, which let KallistiOS split the
  channels and DMA them; the port now splits them and writes sound RAM itself.

**30,000-frame soak with sound** (`tests/soak.txt`, `AUDIOCHECK=1`):

| | |
|---|---|
| Completed | yes, 30,000 frames |
| Gameplay (frames 2162–29881) | 59.8 fps, slowest window 57.7, worst frame 60.6 ms |
| Boot → level | 58.6 fps, slowest window 54.3 |
| Memory | heap 768 KiB at boot and at the end; texture RAM unchanged |
| Underruns | 1 gap, 244 samples (6 ms), before frame 371 (the title scene's load), during silence; none in gameplay |
| Lag frames | 123 run by the poll thread and 0 by the game thread; 3 not queued; no resyncs |
| Ring overflows | 0 |
| Clipped samples in the capture | 0 |
| Clicks in the capture (`audiocheck.py`) | 1, at 404.8 s: a pulse four DirectSound samples long (≈0.3 ms) inside the music, the level the same on both sides of it — a sample's attack, not a break in the stream |
| Gaps inside sound | 311 counted in the produced stream up to frame 29,881, against 320 in the flycast capture over about the same time: within the frame-to-time alignment |

The gaps-inside-sound comparison checks the transport. It counts near-silent stretches with sound
on both sides, which are rests in the music, both in what the port produces and in what flycast's
AICA actually played.
- On the 10,000-frame route, before the lag-frame thread, the counts matched exactly (104 and 104).
- In the soak, the capture also contains about 215 s after frame 30,000. There the game is frozen
  for screen capture, while the poll thread keeps the music going and no counter is printed.

**Behaviour the game exercises** (`AUDIOCHECK=1`, 10,000-frame route): this is what the in-game
runs above actually tested, beyond the differential test's 165 songs.

| Engine behaviour | Count | Engine behaviour | Count |
|---|---:|---|---:|
| DirectSound notes | 1,453 | PSG notes | 1,638 |
| Sample loops | 7,749 | Samples played to the end | 636 |
| Notes released at gate time | 2,394 | End-tie (EOT) | 19 |
| `TrackStop` | 128 | Frames mixed with reverb | 2,167 |
| Tempo changes | 190 | Voice changes | 220 |
| Volume / pan events | 284 / 218 | Key shift / tune | 221 / 2 |
| Bend / bend range | 13 / 3 | Modulation depth / type / LFO speed | 67 / 4 / 2 |
| Pattern call / end / goto | 341 / 419 / 359 | Repeat, priority, LFO delay, PSG register writes | 0 |
| Notes that stole a playing channel | 0 | Pseudo-echo started | 0 |

Channel stealing never happens in these scenes; the differential test's `--mix` runs exercise it.

**Before the lag-frame change** (commit `237b536`), the same 10,000-frame route had 17 gaps, 11
of them while sound was playing, at every scene load and through the text-box scene.

## 6. Limitations and unverified behaviour

- **No real-hardware test.** The AICA path, KallistiOS stream timing, clock drift and the latency
  above are flycast's. The output fix (§8) avoids KallistiOS v2.3.0's 16-bit stereo split; that
  routine's fault is established from the SH-4 manual's XTRCT semantics and flycast, not on a
  console.
- **The PSG is modelled from documentation**, not compared with a GBA or with mGBA's audio:
  - its area-sampled output is smoother than the GBA's raw PWM output;
  - quirks MP2K does not rely on are not modelled: obscure NRx2 writes while a channel plays,
    wave-RAM access while channel 3 plays, and the DMG length quirks.
- **Lag frames run with interrupts off** (§8), up to about 1.5 ms each in gameplay. They only
  happen when the game is already behind, but a VBlank or controller interrupt can be delayed by
  that much while one runs.
- **A lag frame's block can be dropped** when the ring is above its target plus one frame (§2),
  which skips 16.7 ms of sound. It happened 3 times in the 30,000-frame soak; at boot it can fall
  inside the Namco voice, as in one of the runs in §8.
- **Gaps are possible if the poll thread is starved.** Lag frames need the poll thread to get
  the CPU every 10–20 ms, which it has in every run here. §5 lists the remaining gaps.
- **Latency of 65–90 ms** against the GBA's 17–33 ms, a design estimate. A smaller ring is
  possible but leaves less room for slow frames.
- **Pseudo-echo is unverified:** translated, but no song exercises it (§3).
- **The `maxScanlines` mixing time limit** is translated but inactive, as on the GBA build:
  Klonoa sets `gMaxLines` to 0.
- **Not ported:** the GBA's own `SoundMainRAM` code copy, which the port never executes; the
  ARM-side compressed-sample formats, which this engine version does not have.

## 7. How to build and test

```sh
source /path/to/kos/environ.sh
cd kleod/port/dc

make                                  # sound on (default)
make AUDIO=0                          # the silent build
make AUDIOTEST=1                      # adds a 440 Hz test tone on top of the game's output
make AUDIOPROBE=1                     # replaces the output with 440 Hz left / 1 kHz right
make AUDIODUMP=1-900                  # records each stage's audio for frames 1-900 (see below)

make test-audio                       # host: GBA sound model vs GBATEK (30 checks)
python3 tools/m4a_difftest/difftest.py --rom ../../baserom.gba --elf ../../kleod.elf \
        --songs 0-164 --frames 1200 [--mix] [--reverb 127]   # engine vs ROM (needs unicorn)

make SCRIPT=tests/perf.txt FREEZE=1   # 10,000 frames; AUDIO lines every 120 frames
make SCRIPT=tests/soak.txt FREEZE=1 AUDIOCHECK=1   # 30,000 frames, plus gap counting and the
                                                  # engine behaviour census at the end
python3 tools/perfstats.py serial.txt # frame rate, sound cost, lag frames, underruns
python3 tools/audiocheck.py capture.raw   # loudness, gaps inside sound, clicks

tools/audio_regress.sh ../..          # stage-by-stage sound test in flycast (§8): test tone,
                                      # Namco voice vs the cartridge's engine, model vs AICA
                                      # output for the Namco voice, title music and menus
python3 tools/audiotrace.py --help    # the checks it runs, usable on any capture
```

The difftest needs `arm-none-eabi-gcc` (already required to build kleod) and `pip install
unicorn`. A full run of 165 songs × 1,200 frames takes a few minutes on two cores.

To capture what the AICA plays in flycast, the runs here used a locally patched flycast:
`tools/flycast-test-hooks.patch`. It adds two environment-guarded hooks: `FC_AUDIO_DUMP` writes
the 44.1 kHz output, and `FC_RAMDUMP` lets an `AUDIODUMP` build hand its records over (it prints
`FCDUMP <address> <length>` and flycast writes that range of RAM to the file). `audiotrace.py`
needs numpy, and its comparison with the cartridge's engine needs unicorn, like the difftest. The
captures hold the game's sound: keep them out of the repository (`audio_regress.sh` leaves them,
with WAV copies, in `build/audio_regress/`).

## 8. Fix: the crunchy, bassy sound (2026-11-01)

**Symptom.** In flycast the sound was muffled and harsh at the same time, and the opening "NAMCO"
voice came out as noise rather than speech.

**Cause.** KallistiOS v2.3.0's `snd_pcm16_split` (`snd_pcm_split.s`), which a stereo
`snd_stream` uses to split interleaved 16-bit samples into the two AICA channel buffers. For two
stereo frames L0 R0 L1 R1 it writes R1 R0 to the left buffer and L1 L0 to the right one: the
channels are swapped, and every pair of samples is played in reverse order. The routine's `xtrct`
operands assume the opposite of the SH-4's XTRCT semantics (Rn = Rm<<16 | Rn>>16, as the SH-4
manual gives it and flycast implements). The pair reversal predates KallistiOS commit 37b3886d,
"Fix panning for splitting 16-bit PCM"; that commit swapped the routine's two output registers,
adding the channel swap, and changed the C fallback for unaligned data so that it reverses pairs
too. Reversing sample pairs leaves the signal scaled by cos(2πf/fs) and adds a copy mirrored about
half the stream rate (fs − f) at sin(2πf/fs): treble is lost and replaced by inharmonic high
frequencies, the muffled-but-crunchy sound. From about 2 kHz up the mirrored copy is within 10 dB
of the signal, which is where speech is intelligible, so the voice suffered most.

**How it was found** — each stage captured and compared with the one before it:

| Stage | How it was captured | Result before the fix |
|---|---|---|
| 1. ROM data and commands | song 33 (`MUS_NAMCO`, started by `NamcoScreenHandler`): track 1 plays voice 54, an 8-bit signed sample at 13,379 Hz, 11,117 samples (0.83 s), peak −128, RMS 33; tracks 2–3 a 7,955 Hz looped voice | the sample peaks at full scale in the ROM itself; nothing to fix there |
| 2. Engine output (`pcmBuffer` segments DMA plays) | `AUDIODUMP` records vs the cartridge's own engine running song 33 in Unicorn | identical until the first lag frame, then one segment replayed from the previous DMA cycle (below) |
| 3. Hardware model output | `AUDIODUMP` blocks | levels as designed (NAMCO peaks ≈ ±12,500 of 32,767), no clipping |
| 4–5. AICA output | flycast `FC_AUDIO_DUMP`, compared with stage 3 through a reproduction of the AICA's resampling | **diverges**: median residual 0.0 dB (Namco) and −7.9 dB (title music), i.e. not the same signal. Applying the split routine's swap to stage 3 reproduces the capture to −52 dB |
| Test tone through stages 4–5 (`AUDIOPROBE=1`: 440 Hz left, 1 kHz right) | flycast capture | 1 kHz on the **left**, 440 Hz on the right; THD+N −21 and −28 dB; images at 19.1/21.1 kHz (−23 dB) and 19.7/20.6 kHz (−31 dB) |

So the earliest stage that diverged was the Dreamcast output path. The engine, the model's levels,
the sample format (8-bit signed in, 16-bit signed little-endian out), the rate (40,280 Hz
requested; the AICA's nearest is 40,267 Hz, 0.03% flat) and the gain (one software gain stage;
the AICA at full volume with its two channels panned hard left and right, measured gain 1.000)
were all as intended.

**The fix.** `hal_audio.c` registers a direct stream callback (`snd_stream_set_callback_direct`):
it splits the ring's L,R itself and writes each channel's buffer to sound RAM with `spu_memload`.
It no longer depends on KallistiOS's split routine, so a later KallistiOS fix cannot break it
either. (The store-queue copy `spu_memload_sq` was tried first: it waits on `sq_lock` while the
renderer uploads textures, which held the poll thread up for 1–3 ms a frame.)

**Two smaller faults found on the way**, both in the engine-output stage and both now fixed:

- *A stale segment after lag frames.* A lag frame ran `SoundMain` then `m4aSoundVSync`. When it
  fell after the game's own `m4aSoundMain` of that frame, the segment was mixed twice and the next
  VSync exposed one that had not been mixed: DMA then played 16.7 ms left over from the previous
  DMA cycle. The boot capture had 8 such replays in 900 frames, one inside the Namco voice. A lag
  frame now mixes before its VSync only if nothing has mixed since the last one, and after it only
  if the game has already mixed this frame, so every VSync has exactly one mix behind it.
- *The game thread preempting a lag frame.* KallistiOS v2.3 ages the priority of waiting threads
  (`thd_calc_prio`), so the game thread could run its VBlank and audio frame in the middle of the
  poll thread's lag frame (the trace showed a block that began before one VSync and ended after the
  next). Lag frames now run with interrupts off.

**Verification** (`tools/audio_regress.sh`, flycast; "before" is 35e4ea8 with the same recording
and test-tone code added):

| Check | Before | After |
|---|---|---|
| Test tone: correct sides, pitch | swapped | 440 Hz left, 1 kHz right, 8,000 peak |
| Test tone: THD+N / mirror image / leak into the other side | −21 dB / −23 dB / −106 dB (swapped, not mixed) | −48 dB / below −110 dB / below −106 dB |
| Namco voice: engine segments identical to the cartridge's engine, in order | 67/100 (stale segment at frame 102) | 100/100 |
| Segments replayed / blocks torn (boot, 900 frames; route, 1,250 frames) | 8 replays in the boot run (tearing not recorded yet) | 0 / 0 in both |
| AICA output vs model, Namco jingle and voice: median / worst residual, gain | 0.0 dB, unrelated | −60 / −56 dB, 1.000 |
| AICA output vs model, title music (8.6 s) | −7.9 dB median | −67 / −56 dB, 1.000 |
| AICA output vs model, menus with sound effects (route, 148 windows) | — | −66 / −50 dB, 1.000 |
| Title music spectrum, AICA output minus model, 2.5–5 / 5–10 / 10+ kHz | −1.3 / −2.9 / +5.2 dB | 0.0 / 0.0 / 0.0 dB |
| Clipped samples in the captures | 0 | 0 |

**Cost** (`tests/perf.txt`, 10,000 frames, sound on; the silent build does not contain the changed
code, and measured exactly the same before and after):

| | Before (35e4ea8) | After |
|---|---:|---:|
| Gameplay: mean / slowest window / worst frame | 59.8 / 58.1 fps / 56.5 ms | 59.8 / 58.1 fps / 58.3 ms |
| Boot → level: mean / slowest window | 58.7 / 54.2 fps | 58.7 / 54.1 fps |
| Gameplay idle time per frame | 8.16 ms | 8.15 ms |
| Stream polling per frame, boot / gameplay | 0.18 / 0.17 ms | 0.29 / 0.30 ms |
| Underruns | 1 (90 samples, audible) | 0 |
| Clipped samples / clicks in the capture (`audiocheck.py`) | 0 / 1 | 0 / 1 (the same one in both, ~frame 2526 in Vision 1-1) |
| Main RAM heap in use | 768 KiB | 768 KiB |

**The disc image.** `port/dc/build-cdi.sh` (both the default padded image and `CDI_PADDING=0`)
built from this code; `tools/cdicheck.py` passes on both (the boot file is byte for byte the
executable). Booted in flycast, the disc's Namco jingle and voice match the hardware model's
output from the `AUDIODUMP` run to −58…−71 dB in 13 of 15 windows; the other two straddle a lag
frame whose block that run dropped (§6), so the two runs differ by one 16.7 ms block there.

The residual that remains, −50 to −67 dB, is small enough to be the AICA's own rounding (its
15-bit volume and pan multiplies) plus the comparison's reproduction of its interpolation; it was
not traced further. What is **not** verified: real hardware (the fix
relies on the SH-4 manual's XTRCT semantics, which flycast follows); a comparison with a real GBA
or with mGBA's output (the hardware model's accuracy is as in §4); and listening — no one has
listened to the captures in this session, the checks above are numerical.

## 9. Fix: lag frames at the start of a song (2026-11-01)

**Symptom.** After the VMU saves and the 3:2 display were merged, the sound regression failed on
the Namco jingle. Only 3 of 100 engine segments matched the cartridge's engine in order, and the
first difference was the song's third frame. Nothing was audibly wrong for more than a frame:
the song had started one segment early, with one silent segment three frames in.

**Cause.** The rule from §8, "after the game has already mixed this frame", was too narrow:

- The jingle starts with `m4aSoundVSyncOn` part-way through a frame. Earlier in that frame, the
  engine's lock had turned the game's `SoundMain` away, because sound was still off.
- A long frame then ran three poll-thread lag frames. Why a long frame fell exactly there in that
  run was not traced; the boot's timing had moved, since it now reads the VMUs first. The lag
  frames saw no game mix this frame, so they mixed *before* each VSync, standing in for a call
  the game had already made.
- The game's next VBlank therefore had no mix behind it, and played an unmixed segment.
- The cartridge instead plays its one unmixed segment first, right after `VSyncOn`, with the mixes
  following one VSync behind.

The check "one `SoundMain` behind every VSync" did not catch it, because it only looked at audible
segments and this stale one was silent.

**The fix.**
- `SoundMain` calls the lock refuses are now counted too (`hal_m4a_main_entries`).
- A lag frame mixes after its VSync whenever the game's call for the frame has been *made*, whether
  or not it mixed.
- The baseline is taken just before the VBlank handler runs (`hal_audio_vblank_begin`).

**Tests.**
- `STALL=a,b` test builds make those frames 50 ms longer.
- `audio_regress.sh` now runs the boot a second time with `STALL=82,130`, which puts lag frames
  right after `VSyncOn` and in the middle of the jingle.
  - With the old logic: 5/100 segments, plus an exception at frame 82.
  - With the fix: 100/100, and the stream check passes.
- The engine check now counts a VSync with no mix as an exception whenever sound was already on,
  silent or not. Only the first VSync after `VSyncOn` is exempt, as on the cartridge.
- The stream check now searches the capture's whole start-up silence for its first window,
  because that silence grows with anything the boot does before the first frame.
