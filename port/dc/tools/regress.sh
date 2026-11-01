#!/bin/bash
# Renderer regression test: build the route script with the PowerVR renderer, run it headless in
# flycast (framebuffer emulation on, so the PowerVR picture can be read back), and compare every
# dumped frame against the software renderer's picture of the same frame.
#
#   tools/regress.sh <kleod checkout> [flycast wrapper] [outdir]
# Exit status 0 when no pixel differs by more than one colour level.
set -eu
ROOT=$(realpath "$1"); RUN=${2:-/home/claude/dc-emu/run-elf.sh}; OUT=${3:-build/regress}
cd "$(dirname "$0")/.."
: "${KOS_BASE:?source your KallistiOS environ.sh first}"
make -j2 ROOT="$ROOT" RENDERER=pvr SCRIPT=tests/route.txt FREEZE=1 >"$OUT.build.log" 2>&1 || { tail "$OUT.build.log"; exit 2; }
mkdir -p "$OUT"; cp build/klonoa-dc.elf "$OUT/route.elf"
FLYCAST_ARGS="-config config:rend.EmulateFramebuffer=yes" "$RUN" "$OUT/route.elf" 200 "$OUT/run" >/dev/null
grep -q "stopping after 3260 frames" "$OUT/run/serial.txt" || { echo "run did not finish"; exit 3; }
python3 tools/framedump.py "$OUT/run/serial.txt" "$OUT/images" --compare sw pvr | tee "$OUT/result.txt"
N=$(grep -c "^dump" tests/route.txt)
grep -q "compared $N frames, 0 pixels differ" "$OUT/result.txt"
