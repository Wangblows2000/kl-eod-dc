# Klonoa: Empire of Dreams — native Dreamcast port (work in progress)

This branch holds only the port code: `port/dc/`. It builds the game logic from
[testyourmine/kleod](https://github.com/testyourmine/kleod) (MIT, © 2026 testyourmine), a matching
decompilation, natively for the Dreamcast's SH-4 with KallistiOS. It contains no game code, data,
or assets. The ROM and everything derived from it stay on your machine.

## Status

Boots in flycast and runs the original game logic from the Namco logo to the world map. See
[docs/dreamcast-port-report.md](docs/dreamcast-port-report.md).

## Use

1. Clone kleod (tested at `ac62cf6`), put your own `baserom.gba` (USA, SHA-1
   `a0a298d9dba1ba15d04a42fc2eb35893d1a9569b`) in it, and build the GBA ROM once:
   `make extract && make compare`. This produces `kleod.elf`, which the port reads symbol addresses from.
2. Copy this branch's `port/` directory into the kleod checkout.
3. `source /path/to/kos/environ.sh && make -C port/dc` produces `port/dc/build/klonoa-dc.elf`.
   Test builds: `MAX_FRAMES=N` stops after N frames, `AUTOPLAY=K` taps START every K frames, `FREEZE=1` holds the last frame instead of exiting. Delete `build/hal` when changing these.

`port/dc/tools/census.py --root .` prints the portability figures used in the report.
