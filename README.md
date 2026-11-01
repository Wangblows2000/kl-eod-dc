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
- Plays the game's music, sound effects and PSG voices through the AICA:
  - The engine's assembly half, ported to C, matches the ROM's own code byte for byte on all 165
    songs.
  - Gameplay stays at 59.8 fps with sound on.
  - `AUDIO=0` builds the silent port.
- **Not yet tested on real hardware.**

See:
- [docs/dreamcast-port-report.md](docs/dreamcast-port-report.md) — report 1: first boot, the HAL,
  and what kleod provides;
- [docs/powervr-renderer.md](docs/powervr-renderer.md) — report 2: the PowerVR renderer, the GBA
  effects Klonoa uses, visual verification, performance and limitations;
- [docs/audio.md](docs/audio.md) — report 3: sound, covering the engine port and its
  differential test, the GBA sound hardware model, the AICA stream, performance and limitations.

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
| `AUDIO=0` | silent build |
| `AUDIOTEST=1` | add a 440 Hz test tone |
| `AUDIOCHECK=1` | count gaps in the produced sound and report the engine behaviours used |

Options are recorded in `build/options.h`, and changing one rebuilds what depends on it.

Tests, with flycast:
- `port/dc/tools/regress.sh` — visual regression, PowerVR against the software renderer;
- `tests/perf.txt` and `tests/soak.txt` with `tools/perfstats.py` — frame rate, timing and
  sound statistics.

Tests on the host:
- `make test-audio` — the GBA sound model against GBATEK;
- `tools/m4a_difftest/difftest.py` — the sound engine port against the ROM's code.

Report 2, §7, and report 3, §7, have the exact commands. `port/dc/tools/census.py --root .` prints the
portability figures used in report 1.

## Building a CDI (drag and drop)

`port/dc/build-cdi.sh` (Linux) and `port/dc/build-cdi.bat` (Windows, through WSL) turn your own
copy of the game into a bootable Dreamcast disc image, `port/dc/out/klonoa-dc.cdi`.

> **Emulator-tested only.** The CDI this produces has been booted in the Flycast emulator (HLE
> BIOS), reaching the title screen with sound, and checked with `port/dc/tools/cdicheck.py`. It
> has **not** been tried on a real Dreamcast, burned to a CD-R or loaded from an optical drive
> emulator. Flycast running it is not evidence that hardware will.

> **The CDI contains your ROM's data.** It is for your own use. Do not share it. The ROM, the
> build directory and the output directory are all ignored by Git, and nothing here uploads them.

### Prerequisites

All under Linux, or inside WSL on Windows:

| | |
|---|---|
| KallistiOS and its `sh-elf` toolchain | built with KallistiOS's `utils/dc-chain`; tested with KOS v2.3.0 and GCC 15.2.0 |
| [mkdcdisc](https://gitlab.com/simulant/mkdcdisc) | creates the CDI: `meson setup builddir && meson compile -C builddir`, then put `builddir/mkdcdisc` on `PATH` |
| kleod | the [kleod](https://github.com/testyourmine/kleod) checkout, tested at `ac62cf6`, with this repository's `port/` directory copied into it, at a path **without spaces** (`make` cannot build in one; the ROM can be anywhere). If kleod has not been built yet, its own prerequisites too: agbcc and `binutils-arm-none-eabi` (kleod's `INSTALL.md`) |
| Other tools | `make`, `python3`, `sha1sum` |
| The ROM | Klonoa: Empire of Dreams (USA), SHA-1 `a0a298d9dba1ba15d04a42fc2eb35893d1a9569b`, dumped from your own cartridge |

### Setup

- If KallistiOS is not in `/opt/toolchains/dc/kos` or `~/kos`, tell the script where its
  `environ.sh` is. Create `port/dc/build-cdi.conf` (ignored by Git) containing, for example:

  ```sh
  KOS_ENV=/opt/toolchains/dc/kos/environ.sh
  MKDCDISC=/home/me/mkdcdisc/builddir/mkdcdisc   # if it is not on PATH
  # KLEOD_DIR=/home/me/kleod                     # if port/ is not inside the kleod checkout
  # CDI_PADDING=0                                # small image (~10 MB) for emulators and ODEs
  ```

  The default image is about 740 MB, because mkdcdisc pads the data track so a burned CD reads
  from its fast outer edge. `CDI_PADDING=0` makes a 10 MB image with the same contents.

  The same three names also work as environment variables.
- **Windows:**
  - Install WSL (`wsl --install` in an administrator PowerShell) and a distribution, for example
    Ubuntu.
  - Inside it, install everything above. Keep kleod and this port where WSL can reach them: inside
    the WSL file system (`\\wsl$\...`) builds much faster than on `C:`.
  - Native Windows compilation is not supported; the `.bat` only hands the ROM to WSL.
  - **Windows testing so far:** the `.bat` has been run under WINE, not on real Windows. The
    tests covered its error messages and exit statuses, a missing WSL, and paths with spaces,
    `&`, parentheses and commas.
    - WINE has no WSL, so a stand-in `wsl.exe` (`port/dc/tools/wsl_stub.c`) recorded the call the
      `.bat` makes.
    - The Linux script was then run with exactly those arguments.
    - The real WSL hand-off is untested.

### Usage

- **Drag and drop:** drag the ROM file onto `build-cdi.sh` in a Linux file manager that runs
  scripts, or onto `build-cdi.bat` in Windows Explorer. On Windows a console window shows the
  progress and stays open at the end.
- **Command line:** it works from any directory, and the ROM's name and extension do not matter.

  ```sh
  port/dc/build-cdi.sh "/path/to/Klonoa - Empire of Dreams (USA).GBA"
  ```

  ```bat
  port\dc\build-cdi.bat "D:\ROMs\klonoa.gba"
  ```

What the script does:

1. **Checks the ROM by content.** That means a valid GBA header, title `KLONOA`, game code
   `AKLE`, 4 MiB, and the SHA-1 above. Anything else is rejected with the reason. The file is only
   read, never renamed or modified.
2. **Copies it to `<kleod>/baserom.gba`**, where kleod and the port expect it. If a different
   `baserom.gba` is already there, the script stops rather than replace it.
3. **Builds kleod once**, with `make extract && make compare`, but only if it has not been built:
   the port reads `kleod.elf`, `kleod.map` and kleod's object files.
4. **Builds the Dreamcast executable** with the existing `port/dc/Makefile` and release options
   (`RENDERER=pvr AUDIO=1`).
5. **Runs mkdcdisc** on `port/dc/build/klonoa-dc.elf`.

### Output

| Path | Contents |
|---|---|
| `port/dc/out/klonoa-dc.cdi` | the disc image |
| `port/dc/out/build-cdi.log` | the full build and mkdcdisc log |
| `port/dc/build/` | intermediate files (the normal build directory) |

To inspect an image independently of mkdcdisc:

```sh
python3 port/dc/tools/cdicheck.py port/dc/out/klonoa-dc.cdi --elf port/dc/build/klonoa-dc.elf \
        --scramble "$KOS_BASE/utils/scramble/scramble" --objcopy sh-elf-objcopy
```

It reads the CDI's sessions and tracks, the IP.BIN and the ISO9660 directory. It then checks
that `1ST_READ.BIN`, descrambled with KallistiOS's `scramble -d`, is byte for byte the built
executable.

Exit status: 0 for success, 2 for a usage error, 3 for a rejected ROM, 4 for a missing tool or
setup problem, 5 for a failed build, 6 for a failed CDI creation.

### Troubleshooting

| Message | What to do |
|---|---|
| `ROM rejected: ... SHA-1 is ...` | The file is Klonoa (USA) but a different dump: patched, trimmed or bad. Only the dump above works. |
| `ROM rejected: not the USA release` | The port is built from the USA version (game code `AKLE`). |
| `KallistiOS not found` | Set `KOS_ENV` to KallistiOS's `environ.sh`, in `build-cdi.conf` or the environment. |
| `mkdcdisc not found` | Build mkdcdisc and put it on `PATH`, or set `MKDCDISC`. |
| `does not look like a kleod checkout` | `port/` must sit inside the kleod checkout, or set `KLEOD_DIR`. |
| `... path contains a space` | Move the kleod checkout to a path without spaces. |
| `baserom.gba already exists and is a different file` | Move that file away; the script never overwrites it. |
| `the Dreamcast build failed` | The last lines are printed; the whole log is in `port/dc/out/build-cdi.log`. |
| Windows: `WSL was not found` / `no Linux distribution answers` | Install WSL and a distribution, open it once, and install the prerequisites inside it. |
| Windows: the window closes, or the path is cut short | `cmd.exe` mangles `%` in paths, and `&` in paths without spaces. Move the ROM to a plain folder such as `C:\ROMs`. |
