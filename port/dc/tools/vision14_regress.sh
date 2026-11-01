#!/bin/bash
# Vision 1-4 regression in flycast: the hover-board level to its end and on into Vision 1-5, with
# the PowerVR picture compared against the software renderer at every dumped frame.
#
#   tools/vision14_regress.sh <kleod checkout> [flycast wrapper] [outdir]
#
# The level's room transitions write MOSAIC = 0x1110 (blocks one pixel wide, two tall). Before
# the fix in hal_video_pvr.c (docs/vision14-mosaic.md), the PowerVR renderer's mosaic buffer
# overflowed into its BG texture records there, and every BG was drawn from garbage for the rest
# of the session.
#
# 1. tests/save.txt from an empty VMU: a new game, which the game saves on the way to the world
#    map (the same run as tools/save_regress.sh case 1).
# 2. tools/vmuimg.py vision: that save's game file continues at World 1's vision select with 1-1 to
#    1-3 cleared. Only the save file is edited, through the game's own record format; nothing in
#    the running game's memory is touched.
# 3. tests/vision14.txt: continue, choose Vision 1-4, play it to the end and on into 1-5.
# Checks: the run reaches Vision 1-4's last room and then Vision 1-5; every dumped frame's PowerVR
# picture is within one colour level of the software renderer's; flycast saw no malformed TA input;
# the run completes. Exit status 0 when every check passes.
set -eu
ROOT=$(realpath "$1"); RUN=${2:-/home/claude/dc-emu/run-elf.sh}; OUT=${3:-build/vision14_regress}
cd "$(dirname "$0")/.."
: "${KOS_BASE:?source your KallistiOS environ.sh first}"
mkdir -p "$OUT"; OUT=$(realpath "$OUT")
fails=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }

echo "== 1. a saved game (tests/save.txt from an empty VMU)"
make -j2 ROOT="$ROOT" RENDERER=pvr SCRIPT=tests/save.txt >"$OUT/build_save.log" 2>&1 || { tail "$OUT/build_save.log"; exit 2; }
cp build/klonoa-dc.elf "$OUT/save.elf"
rm -rf "$OUT/vmu" "$OUT/save"; mkdir -p "$OUT/vmu"
FLYCAST_ARGS="-config config:Dreamcast.VMUPath=$OUT/vmu -config config:PerGameVmu=no" \
    "$RUN" "$OUT/save.elf" 120 "$OUT/save" >/dev/null
if grep -aq "written and verified" "$OUT/save/serial.txt"; then ok "the game saved"; else bad "the game saved"; exit 1; fi

echo "== 2. continue at World 1's vision select with 1-1..1-3 cleared"
python3 tools/vmuimg.py vision "$OUT/vmu/vmu_save_A1.bin" 1 4

echo "== 3. Vision 1-4 to the end, then Vision 1-5"
make -j2 ROOT="$ROOT" RENDERER=pvr SCRIPT=tests/vision14.txt >"$OUT/build.log" 2>&1 || { tail "$OUT/build.log"; exit 2; }
cp build/klonoa-dc.elf "$OUT/vision14.elf"
rm -rf "$OUT/run" "$OUT/images"
FLYCAST_ARGS="-config config:rend.EmulateFramebuffer=yes -config config:Dreamcast.VMUPath=$OUT/vmu -config config:PerGameVmu=no" \
    "$RUN" "$OUT/vision14.elf" 900 "$OUT/run" >/dev/null
S="$OUT/run/serial.txt"
grep -a "^SCENE" "$S" | grep -a "world=1 level=4" | head -1 | grep -q . && ok "Vision 1-4 started" || bad "Vision 1-4 started"
last=$(grep -a "^SCENE.*world=1 level=4" "$S" | sed 's/.*room=//' | sort -n | tail -1)
[ "${last:-0}" -ge 3 ] && ok "Vision 1-4's last room (room $last) reached" || bad "Vision 1-4's last room reached (got room ${last:-none})"
grep -aq "^SCENE.*world=1 level=5" "$S" && ok "on into Vision 1-5" || bad "on into Vision 1-5"
grep -aq "stopping after 9600 frames" "$S" && ok "run completed (9,600 frames)" || bad "run completed"
n=$(grep -c "Unhandled param" "$OUT/run/flycast.log" || true)
[ "$n" = 0 ] && ok "no malformed TA input (flycast)" || bad "malformed TA input: $n times (flycast)"
python3 tools/framedump.py "$S" "$OUT/images" --compare sw pvr > "$OUT/compare.txt" 2>&1 || true
frames=$(grep -c "^frame " "$OUT/compare.txt" || true)
worse=$(awk '/^frame / && $3 != 0 {n++} END {print n + 0}' "$OUT/compare.txt")
[ "$frames" -ge 15 ] && ok "$frames frames dumped and compared" || bad "only $frames frames dumped"
[ "$worse" = 0 ] && ok "PowerVR within one colour level of the software renderer in every frame" \
    || bad "$worse frames with pixels more than one level off: $(awk '/^frame / && $3 != 0 {printf "%s%s ", $2, $3}' "$OUT/compare.txt")"
echo
[ $fails = 0 ] && echo "vision 1-4 regression: all checks passed" || echo "vision 1-4 regression: $fails check(s) failed"
exit $((fails > 0))
