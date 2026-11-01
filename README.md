# Klonoa: Empire of Dreams — native Dreamcast port (work in progress)

This branch holds only the port code: `port/dc/`. It builds the game logic from
[testyourmine/kleod](https://github.com/testyourmine/kleod) (MIT, © 2026 testyourmine), a matching
decompilation, natively for the Dreamcast's SH-4 with KallistiOS. It contains no game code, data,
or assets. The ROM and everything derived from it stay on your machine.

## Status

- Runs the original game logic from boot into Vision 1-1 gameplay in flycast.
- Draws with a native PowerVR renderer:
  - Gameplay runs at 59.8 fps in flycast, with no 120-frame window below 59.7 over a
    30,000-frame soak.
  - Scene transitions still drop frames.
- The software renderer is kept as a fallback: hold **Y** at boot, or build with `RENDERER=sw`.
- **No sound yet**: the PCM mixer is still stubbed.
- **Not yet tested on real hardware.**

See:
- [docs/dreamcast-port-report.md](docs/dreamcast-port-report.md) — report 1: first boot, the HAL,
  and what kleod provides;
- [docs/powervr-renderer.md](docs/powervr-renderer.md) — report 2: the PowerVR renderer, the GBA
  effects Klonoa uses, visual verification, performance and limitations.

## Use

1. Clone kleod (tested at `ac62cf6`) and put your own `baserom.gba` in it (USA, SHA-1
   `a0a298d9dba1ba15d04a42fc2eb35893d1a9569b`).
2. Build the GBA ROM once: `make extract && make compare`. This produces `kleod.elf`, which the
   port reads symbol addresses from.
3. Copy this branch's `port/` directory into the kleod checkout.
4. `source /path/to/kos/environ.sh && make -C port/dc` produces `port/dc/build/klonoa-dc.elf`.

Build options:

| Option | Effect |
|---|---|
| `RENDERER=sw\|pvr` | the default renderer |
| `SCRIPT=tests/<name>.txt` | a scripted test run |
| `MAX_FRAMES=N` | stop after N frames |
| `AUTOPLAY=K` | tap START every K frames |
| `FREEZE=1` | hold the last frame instead of exiting |

Options are recorded in `build/options.h`, and changing one rebuilds what depends on it.

Tests, with flycast:
- `port/dc/tools/regress.sh` — visual regression, PowerVR against the software renderer;
- `tests/perf.txt` and `tests/soak.txt` with `tools/perfstats.py` — frame rate and timing.

Report 2, §7, has the exact commands. `port/dc/tools/census.py --root .` prints the portability
figures used in report 1.
