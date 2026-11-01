#!/bin/bash
# VMU save test in flycast: saves, a restart that loads them through the game's file select, a
# damaged newest copy, no VMU, and a full VMU.
#
#   tools/save_regress.sh <kleod checkout> [flycast wrapper] [outdir]
#
# flycast keeps its VMU images (vmu_save_A1.bin, A2) in a directory per case (Dreamcast.VMUPath),
# so a case can start from the images the previous one left. tools/vmuimg.py reads and prepares
# them independently of the port. Exit status 0 when every check passes.
set -eu
ROOT=$(realpath "$1"); RUN=${2:-/home/claude/dc-emu/run-elf.sh}; OUT=${3:-build/save_regress}
cd "$(dirname "$0")/.."
: "${KOS_BASE:?source your KallistiOS environ.sh first}"
mkdir -p "$OUT"; OUT=$(realpath "$OUT")
make -j2 ROOT="$ROOT" RENDERER=pvr SCRIPT=tests/save.txt >"$OUT/build.log" 2>&1 || { tail "$OUT/build.log"; exit 2; }
cp build/klonoa-dc.elf "$OUT/save.elf"
fails=0
ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }
expect() { # case, pattern, description
    if grep -aq -- "$2" "$OUT/$1/run/serial.txt"; then ok "$3"; else bad "$3"; fi
}
handler() { sh-elf-nm "$OUT/save.elf" | awk -v n="_$1" '$3 == n {print $1}'; }
run() { # case, vmu dir, extra flycast args
    rm -rf "$OUT/$1/run"
    FLYCAST_ARGS="-config config:Dreamcast.VMUPath=$2 -config config:PerGameVmu=no ${3:-}" \
        "$RUN" "$OUT/save.elf" 120 "$OUT/$1/run" >/dev/null
    grep -a "^save:\|^SCENE\|^stopping" "$OUT/$1/run/serial.txt" > "$OUT/$1/save.log" || true
    python3 tools/framedump.py "$OUT/$1/run/serial.txt" "$OUT/$1/images" >/dev/null 2>&1 || true
}
crc_at_exit() { grep -a "written and verified" "$OUT/$1/run/serial.txt" | tail -1 | sed 's/.*EEPROM crc \([0-9a-f]*\).*/\1/'; }

echo "== 1. empty VMU: new game, saved on the way to the world map"
rm -rf "$OUT/1"; mkdir -p "$OUT/1/vmu"
run 1 "$OUT/1/vmu"
expect 1 "save: VMU A1: no save" "boot finds VMU A1 without a save"
expect 1 'status line "VMU A1: NO SAVE YET"' "status line says so"
expect 1 "written and verified" "a save is written and read back"
expect 1 "at exit everything saved" "nothing left unsaved at exit"
expect 1 "stopping after 1600 frames" "run completed"
C1=$(crc_at_exit 1)
python3 tools/vmuimg.py list "$OUT/1/vmu/vmu_save_A1.bin" | tee "$OUT/1/vmu.txt"
grep -q "KLONOAEOD_A  data 3 blocks" "$OUT/1/vmu.txt" && ok "VMU holds KLONOAEOD_A, 3 blocks" || bad "KLONOAEOD_A on the VMU"

echo "== 2. restart with that VMU: the save loads, and file 1 continues it"
rm -rf "$OUT/2"; mkdir -p "$OUT/2"; cp -r "$OUT/1/vmu" "$OUT/2/vmu"
run 2 "$OUT/2/vmu"
expect 2 "save: loaded seq .* from VMU A1 (EEPROM crc $C1)" "boot loads the last save (EEPROM crc $C1)"
T=$(handler TransitionFromFileSelectToLevel_FadeOut)
expect 2 "handler=$T" "file 1 is continued (TransitionFromFileSelectToLevel), not started anew"
if cmp -s "$OUT/1/images/sw_1000.png" "$OUT/2/images/sw_1000.png"; then
    bad "file select screen differs from the empty-VMU run (file 1 shown as saved)"
else ok "file select screen differs from the empty-VMU run (file 1 shown as saved)"; fi
expect 2 "written and verified" "progress saved again (the other of the two files)"
python3 tools/vmuimg.py list "$OUT/2/vmu/vmu_save_A1.bin" | tee "$OUT/2/vmu.txt"
grep -q "KLONOAEOD_B" "$OUT/2/vmu.txt" && ok "both files present after repeated saves" || bad "both files present"

echo "== 3. newest copy damaged: the older one loads"
rm -rf "$OUT/3"; mkdir -p "$OUT/3"; cp -r "$OUT/2/vmu" "$OUT/3/vmu"
A=$(python3 tools/vmuimg.py record "$OUT/3/vmu/vmu_save_A1.bin" KLONOAEOD_A | sed 's/.* seq \([0-9]*\) .*/\1/')
B=$(python3 tools/vmuimg.py record "$OUT/3/vmu/vmu_save_A1.bin" KLONOAEOD_B | sed 's/.* seq \([0-9]*\) .*/\1/')
if [ "$A" -gt "$B" ]; then NEW=KLONOAEOD_A; OLD=$B; else NEW=KLONOAEOD_B; OLD=$A; fi
python3 tools/vmuimg.py corrupt "$OUT/3/vmu/vmu_save_A1.bin" $NEW
run 3 "$OUT/3/vmu"
expect 3 "save: loaded seq $OLD from VMU A1" "the older copy (seq $OLD) loads"
expect 3 "stopping after 1600 frames" "run completed"

echo "== 4. no VMU"
rm -rf "$OUT/4"; mkdir -p "$OUT/4/vmu"
run 4 "$OUT/4/vmu" "-config input:device1.1=10 -config input:device1.2=10"
expect 4 "save: no VMU found" "boot reports no VMU"
expect 4 'status line "NO VMU - PROGRESS WILL NOT BE SAVED"' "status line at boot"
expect 4 'status line "NO VMU - PROGRESS NOT SAVED"' "status line when the game saves"
expect 4 "save: no VMU; previous save kept" "the save attempt fails cleanly"
expect 4 "stopping after 1600 frames" "the game runs on to the world map"

echo "== 5. full VMU (2 blocks free)"
rm -rf "$OUT/5"; mkdir -p "$OUT/5/vmu"
cp "$OUT/1/vmu/vmu_save_A2.bin" "$OUT/5/vmu/vmu_save_A1.bin"   # a formatted, empty VMU image
python3 tools/vmuimg.py fill "$OUT/5/vmu/vmu_save_A1.bin" 2
run 5 "$OUT/5/vmu" "-config input:device1.2=10"
expect 5 "save: VMU full; previous save kept" "the save is refused for lack of space"
expect 5 'status line "VMU A1 FULL (3 FREE BLOCKS NEEDED) - NOT SAVED"' "status line says so"
expect 5 "stopping after 1600 frames" "the game runs on"
python3 tools/vmuimg.py list "$OUT/5/vmu/vmu_save_A1.bin" | grep -q "KLONOAEOD" && bad "nothing written" || ok "nothing written"

echo
[ $fails = 0 ] && echo "save regression: all checks passed" || echo "save regression: $fails check(s) FAILED"
exit $fails
