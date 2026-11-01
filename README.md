# Klonoa: Empire of Dreams, Dreamcast port.

It builds the game logic from
[testyourmine/kleod](https://github.com/testyourmine/kleod) (MIT, © 2026 testyourmine), a matching
decompilation, natively for the Dreamcast's SH-4 with KallistiOS. This repository holds only the
port code, `port/dc/`: no game code, data or assets. You supply your own ROM, and everything built from it stays on your machine.

## Status

- Runs the original game logic, at 59.8 fps in gameplay.
- PowerVR renderer by default; the software renderer is a fallback (hold **Y** at boot).
- Music, sound effects and PSG voices through the AICA; `AUDIO=0` builds a silent port.
- **Tested in the Flycast emulator only, never on a real Dreamcast.**

## Make a disc image (CDI)

You need, under Linux or WSL on Windows:

- KallistiOS and its `sh-elf` toolchain (tested: KOS v2.3.0, GCC 15.2.0);
- [mkdcdisc](https://gitlab.com/simulant/mkdcdisc);
- a [kleod](https://github.com/testyourmine/kleod) checkout (tested at `ac62cf6`) at a path
  without spaces, with this repository's `port/` directory copied into it;
- your own Klonoa: Empire of Dreams (USA) ROM, SHA-1 `a0a298d9dba1ba15d04a42fc2eb35893d1a9569b`.

Then drag the ROM onto `port/dc/build-cdi.sh` (Linux) or `port/dc/build-cdi.bat` (Windows), or
run:

```sh
port/dc/build-cdi.sh /path/to/your/rom.gba
```

The script checks the ROM by content, builds kleod if needed, builds the port and writes
`port/dc/out/klonoa-dc.cdi` (log: `port/dc/out/build-cdi.log`). It never modifies the ROM.

If KallistiOS or mkdcdisc are not found, create `port/dc/build-cdi.conf`:

```sh
KOS_ENV=/opt/toolchains/dc/kos/environ.sh
MKDCDISC=/home/me/mkdcdisc/builddir/mkdcdisc
# KLEOD_DIR=/home/me/kleod    # if port/ is not inside the kleod checkout
# CDI_PADDING=0               # 10 MB image for emulators and ODEs (default ~740 MB, for burning)
```

The CDI contains your ROM's data: keep it for your own use. Error messages say what is wrong;
the script's header lists every check and exit status. The Windows `.bat` only hands the ROM to
WSL and has been tested under WINE, not on real Windows.

## Build from source

```sh
cd kleod && make extract && make compare        # once: builds kleod.elf, which the port reads
source /path/to/kos/environ.sh
make -C port/dc                                  # -> port/dc/build/klonoa-dc.elf
```

Common options: `RENDERER=sw|pvr`, `AUDIO=0`, `SCRIPT=tests/<name>.txt`, `MAX_FRAMES=N`,
`FREEZE=1`. Test-only options (`AUDIOTEST`, `AUDIOCHECK`, `AUDIOPROBE`, `AUDIODUMP`, `AUTOPLAY`)
are described at the top of `port/dc/Makefile`.

## Tests

- `port/dc/tools/regress.sh` — visual regression in flycast (PowerVR vs software renderer).
- `port/dc/tools/audio_regress.sh` — sound, stage by stage, in flycast (needs
  `port/dc/tools/flycast-test-hooks.patch`).
- `make -C port/dc test-audio` and `port/dc/tools/m4a_difftest/difftest.py` — the sound model
  and the sound engine against the original ROM code, on the host.

The reports have the exact commands and results.

## Reports

- [Report 1](docs/dreamcast-port-report.md) — first boot, the hardware layer, what kleod provides.
- [Report 2](docs/powervr-renderer.md) — the PowerVR renderer.
- [Report 3](docs/audio.md) — sound; §8 covers the fix for the muffled, crunchy output.
