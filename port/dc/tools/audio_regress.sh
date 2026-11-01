#!/bin/bash
# Sound regression test, stage by stage, run headless in flycast.
#
#   tools/audio_regress.sh <kleod checkout> [flycast wrapper] [outdir]
#
#   1. probe  AUDIOPROBE=1: a 440 Hz / 1 kHz test signal in place of the game's sound, through the
#             ring, the stream and the AICA -- the Dreamcast output path alone. Frames 1-360: the
#             title scene's load at frame 370 runs the ring dry (during silence in the game, but
#             not in the tone), which is a transport event, counted by the AUDIO lines instead.
#   2. boot   AUDIODUMP=1-900, no input: the Namco jingle and voice (song 33) and the title music.
#             The engine's output against the cartridge's own engine (needs the Python module
#             unicorn and <kleod>/kleod.elf); every block whole and one mix per VSync; the hardware
#             model's output against what the AICA played; the capture's format, peak and clipping.
#   3. stall  the boot again with frames 82 and 130 made 50 ms long (STALL=), so that lag frames run
#             right after the game turns sound on and in the middle of the jingle: the song
#             comparison and the stream check again.
#   4. route  tests/audio.txt with AUDIODUMP=1-1250: menu sound effects over the title and
#             file-select music; the same checks without the song comparison.
#
# Needs a flycast with the FC_AUDIO_DUMP and FC_RAMDUMP test hooks (tools/flycast-test-hooks.patch)
# and Python 3 with numpy. The captures stay in <outdir> (raw, and .wav for listening): they hold
# the game's sound, so keep them out of the repository. Exit status 0 when every check passes.
set -eu
ROOT=$(realpath "$1"); RUN=${2:-/home/claude/dc-emu/run-elf.sh}; OUT=${3:-build/audio_regress}
PY=${PYTHON:-python3}
cd "$(dirname "$0")/.."
: "${KOS_BASE:?source your KallistiOS environ.sh first}"
mkdir -p "$OUT"; OUT=$(realpath "$OUT")
rc=0

run() { # name, seconds, make options...
    local name=$1 secs=$2; shift 2
    make -j2 ROOT="$ROOT" RENDERER=pvr "$@" >"$OUT/$name.build.log" 2>&1 || { tail "$OUT/$name.build.log"; exit 2; }
    cp build/klonoa-dc.elf "$OUT/$name.elf"
    rm -rf "$OUT/$name"
    FC_AUDIO_DUMP="$OUT/$name.raw" FC_RAMDUMP="$OUT/$name.recs" "$RUN" "$OUT/$name.elf" "$secs" "$OUT/$name" >/dev/null
}
step() { echo; echo "== $*"; }

run probe 25 AUDIOPROBE=1 MAX_FRAMES=360  # steady state: the boot's next load (frame 370) underruns
step "probe: Dreamcast output path"
$PY tools/audiotrace.py probe "$OUT/probe.raw" || rc=1

run boot 45 AUDIODUMP=1-900 MAX_FRAMES=920
[ -s "$OUT/boot.recs" ] || { echo "boot: no AUDIODUMP records (does flycast have the FC_RAMDUMP hook?)"; exit 3; }
step "boot: engine (Namco jingle and voice, song 33) against the cartridge's engine"
$PY tools/audiotrace.py engine "$OUT/boot.recs" --rom "$ROOT/baserom.gba" --elf "$ROOT/kleod.elf" --song 33 \
    --frames 100 || rc=1
step "boot: hardware model against the AICA output -- Namco jingle and voice"
$PY tools/audiotrace.py stream "$OUT/boot.recs" "$OUT/boot.raw" --frames 83-180 || rc=1
step "boot: hardware model against the AICA output -- title music"
$PY tools/audiotrace.py stream "$OUT/boot.recs" "$OUT/boot.raw" --frames 400-900 || rc=1
step "boot: capture"
$PY tools/audiotrace.py capture "$OUT/boot.raw" --wav "$OUT/boot.wav" || rc=1

# The same boot with two 50 ms frames: frame 82, in which the game turns sound back on for the
# jingle (m4aSoundVSyncOn), and frame 130, in the middle of it. Each runs the poll thread's lag
# frames, which must leave the engine's output exactly the cartridge's.
run stall 30 AUDIODUMP=1-300 MAX_FRAMES=320 STALL=82,130
[ -s "$OUT/stall.recs" ] || { echo "stall: no AUDIODUMP records"; exit 3; }
step "stall: engine with lag frames at the jingle's start and middle, against the cartridge's engine"
$PY tools/audiotrace.py engine "$OUT/stall.recs" --rom "$ROOT/baserom.gba" --elf "$ROOT/kleod.elf" --song 33 \
    --frames 100 --expect-lag || rc=1
step "stall: hardware model against the AICA output"
$PY tools/audiotrace.py stream "$OUT/stall.recs" "$OUT/stall.raw" --frames 83-180 || rc=1

run route 70 SCRIPT=tests/audio.txt AUDIODUMP=1-1250
[ -s "$OUT/route.recs" ] || { echo "route: no AUDIODUMP records"; exit 3; }
step "route: engine (menu sound effects over the music)"
$PY tools/audiotrace.py engine "$OUT/route.recs" || rc=1
step "route: hardware model against the AICA output"
$PY tools/audiotrace.py stream "$OUT/route.recs" "$OUT/route.raw" --frames 300-1250 || rc=1
step "route: capture"
$PY tools/audiotrace.py capture "$OUT/route.raw" --wav "$OUT/route.wav" || rc=1

echo
[ $rc = 0 ] && echo "audio regression: all checks passed" || echo "audio regression: FAILED"
exit $rc
