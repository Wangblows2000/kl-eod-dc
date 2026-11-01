#!/usr/bin/env bash
# Build a bootable Dreamcast disc image (CDI) of the port from your own Klonoa: Empire of Dreams
# (USA) GBA ROM.
#
#   port/dc/build-cdi.sh /path/to/your/rom.gba        (or drag the ROM onto this script)
#
# Steps, all with the project's existing build rules:
#   1. check the ROM by content: GBA header (KLONOA / AKLE / AF), size and the SHA-1 kleod is
#      built against. Its file name and extension do not matter; the file is only read.
#   2. put a copy at <kleod>/baserom.gba, where kleod and this port expect it (never replacing a
#      different file that is already there);
#   3. build kleod once if it has not been built (kleod's own `make extract` + `make compare`);
#   4. build the Dreamcast executable with port/dc/Makefile (release options: PowerVR renderer,
#      sound on);
#   5. turn it into a CDI with mkdcdisc.
#
# Output: port/dc/out/klonoa-dc.cdi (and out/build-cdi.log). Intermediate files: port/dc/build/.
# Both are ignored by Git. The CDI contains your ROM's data: keep it for your own use only.
#
# Settings (environment variables, or lines like `KOS_ENV=...` in port/dc/build-cdi.conf):
#   KOS_ENV    KallistiOS environ.sh (default: $KOS_BASE if set, else common install paths)
#   MKDCDISC   the mkdcdisc program (default: mkdcdisc on PATH)
#   KLEOD_DIR  the kleod checkout (default: two directories above this script)
#   CDI_PADDING=0  no data-track padding: a small image (~10 MB) for emulators and drive
#                  emulators, instead of the ~740 MB padded image meant for burning
#
# Exit status: 0 success, 2 usage, 3 ROM rejected, 4 missing tool or setup, 5 build failed,
# 6 CDI creation failed.

set -u
PROG=build-cdi
EXPECTED_SHA1=a0a298d9dba1ba15d04a42fc2eb35893d1a9569b
ROM_SIZE=4194304

# This script's directory, wherever it is launched from. A symlink *to the script* (e.g. on the
# desktop) is followed; directories are kept as the user sees them, so a port/ directory that is
# itself a symlink into the kleod checkout still finds that checkout two levels up.
SRC=${BASH_SOURCE[0]}
while [ -L "$SRC" ]; do
    LINK=$(readlink -- "$SRC")
    case $LINK in /*) SRC=$LINK ;; *) SRC=$(dirname -- "$SRC")/$LINK ;; esac
done
HERE=$(cd -- "$(dirname -- "$SRC")" && pwd) || exit 4
OUT="$HERE/out"
LOG="$OUT/build-cdi.log"

say() { printf '%s: %s\n' "$PROG" "$*"; }
fail() { # exit-status message...
    local rc=$1
    shift
    printf '\n%s: error: %s\n' "$PROG" "$*" >&2
    exit "$rc"
}
need() { command -v "$1" > /dev/null 2>&1 || fail 4 "'$1' is not installed or not on PATH. $2"; }

# ---- arguments ----------------------------------------------------------------------------
if [ $# -ne 1 ] || [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    cat >&2 << EOF
Usage: $0 /path/to/Klonoa-Empire-of-Dreams-USA.gba

Builds port/dc/out/klonoa-dc.cdi from your own copy of the game. Any file name works; the ROM is
checked by content and is never modified. See "Building a CDI" in the repository README.
EOF
    [ $# -eq 1 ] && exit 0
    exit 2
fi
ROM_ARG=$1
[ -e "$ROM_ARG" ] || fail 2 "no such file: '$ROM_ARG'"
ROM=$(readlink -f -- "$ROM_ARG") || fail 2 "cannot resolve '$ROM_ARG'"
[ -f "$ROM" ] || fail 2 "not a regular file: '$ROM'"
[ -r "$ROM" ] || fail 2 "cannot read '$ROM' (permissions)"

# ---- optional local settings ------------------------------------------------------------------
if [ -f "$HERE/build-cdi.conf" ]; then
    set +u
    # shellcheck disable=SC1091
    . "$HERE/build-cdi.conf" || fail 4 "could not read $HERE/build-cdi.conf"
    set -u
fi
KLEOD_DIR=${KLEOD_DIR:-"$HERE/../.."}
KLEOD_DIR=$(cd -- "$KLEOD_DIR" 2> /dev/null && pwd) || fail 4 "kleod checkout not found: '${KLEOD_DIR}'"

need python3 "Install Python 3."
need sha1sum "Install coreutils."
need make "Install make (build-essential)."

# ---- 1. the ROM -----------------------------------------------------------------------------
say "checking ROM: $ROM"
python3 - "$ROM" "$ROM_SIZE" << 'EOF' || exit 3
import sys
path, size = sys.argv[1], int(sys.argv[2])
with open(path, "rb") as f:
    d = f.read(size + 1)
def reject(msg):
    print(f"\nbuild-cdi: error: ROM rejected: {msg}", file=sys.stderr)
    sys.exit(3)
if len(d) < 0xC0:
    reject("the file is too small to be a GBA ROM")
if d[0xB2] != 0x96 or ((-sum(d[0xA0:0xBD]) - 0x19) & 0xFF) != d[0xBD]:
    reject("this is not a GBA ROM image (no valid cartridge header)")
title = d[0xA0:0xAC].rstrip(b"\0").decode("ascii", "replace")
code = d[0xAC:0xB0].decode("ascii", "replace")
if title != "KLONOA" or code[:3] != "AKL":
    reject(f"this GBA ROM is '{title}' ({code}), not Klonoa: Empire of Dreams")
if code != "AKLE":
    reject(f"this is Klonoa: Empire of Dreams, but not the USA release (game code {code}, "
           "the port needs AKLE)")
if len(d) != size:
    reject(f"wrong size: {len(d) if len(d) <= size else 'more than ' + str(size)} bytes, expected {size} "
           "(trimmed, padded or patched dump?)")
EOF
SHA1=$(sha1sum < "$ROM" | cut -d' ' -f1)
[ "$SHA1" = "$EXPECTED_SHA1" ] || fail 3 "ROM rejected: it is Klonoa (USA) but its SHA-1 is $SHA1,
not $EXPECTED_SHA1. kleod and this port are built against that exact dump; a patched, hacked or
bad dump will not build or will not match."
say "ROM ok: Klonoa: Empire of Dreams (USA), SHA-1 $SHA1"

# ---- 2. place it where kleod expects it ------------------------------------------------------------
[ -f "$KLEOD_DIR/Makefile" ] && [ -f "$KLEOD_DIR/src/m4a.c" ] && [ -d "$KLEOD_DIR/tools/preproc" ] ||
    fail 4 "'$KLEOD_DIR' does not look like a kleod checkout. Put this repository's port/ directory
inside your kleod checkout (see README), or set KLEOD_DIR."
# make cannot handle spaces in the paths it builds with (the ROM's own path may have them).
case "$KLEOD_DIR$HERE" in *[[:space:]]*)
    fail 4 "the kleod checkout or this port is in a folder whose path contains a space:
  $KLEOD_DIR
make cannot build there. Move the checkout to a path without spaces (the ROM itself can be
anywhere)." ;;
esac
BASEROM="$KLEOD_DIR/baserom.gba"
if [ "$(readlink -f -- "$BASEROM" 2> /dev/null)" = "$ROM" ]; then
    say "the ROM is already $BASEROM"
elif [ -e "$BASEROM" ]; then
    [ "$(sha1sum < "$BASEROM" | cut -d' ' -f1)" = "$EXPECTED_SHA1" ] ||
        fail 4 "$BASEROM already exists and is a different file. Not overwriting it; move it away first."
    say "kleod already has an identical baserom.gba"
else
    cp -- "$ROM" "$BASEROM.tmp" && mv -- "$BASEROM.tmp" "$BASEROM" || fail 4 "could not copy the ROM to $BASEROM"
    say "copied the ROM to $BASEROM (your file is unchanged)"
fi

mkdir -p "$OUT" || fail 4 "cannot create $OUT"
: > "$LOG" || fail 4 "cannot write $LOG"

# ---- tools ---------------------------------------------------------------------------------
if [ -z "${KOS_BASE:-}" ]; then
    for env in "${KOS_ENV:-}" /opt/toolchains/dc/kos/environ.sh "$HOME/kos/environ.sh" "$HOME/dc/kos/environ.sh"; do
        if [ -n "$env" ] && [ -f "$env" ]; then
            KOS_ENV=$env
            break
        fi
    done
    [ -n "${KOS_ENV:-}" ] && [ -f "$KOS_ENV" ] || fail 4 "KallistiOS not found. Install KallistiOS and its
sh-elf toolchain (https://kos-docs.dreamcast.wiki), then set KOS_ENV to its environ.sh, e.g.
  KOS_ENV=/opt/toolchains/dc/kos/environ.sh $0 \"rom.gba\""
    set +u # KallistiOS's environ scripts use unset variables
    # shellcheck disable=SC1090
    . "$KOS_ENV" || fail 4 "sourcing $KOS_ENV failed"
    set -u
fi
[ -n "${KOS_CC:-}" ] && [ -x "$KOS_CC" ] || fail 4 "the KallistiOS compiler (\$KOS_CC='${KOS_CC:-}') is missing.
Build the toolchain with KallistiOS's utils/dc-chain first."
MKDCDISC=${MKDCDISC:-$(command -v mkdcdisc || true)}
[ -n "$MKDCDISC" ] && [ -x "$MKDCDISC" ] || fail 4 "mkdcdisc not found. It builds the CDI. Get it from
https://gitlab.com/simulant/mkdcdisc (meson setup builddir && meson compile -C builddir), then put
it on PATH or set MKDCDISC=/path/to/mkdcdisc."
say "KallistiOS: $KOS_BASE"
say "mkdcdisc:   $MKDCDISC"

JOBS=$(nproc 2> /dev/null || echo 2)

# ---- 3. kleod once -------------------------------------------------------------------------------
# The port reads kleod.elf, kleod.map and kleod's object files (build/kleod/).
if [ ! -f "$KLEOD_DIR/kleod.elf" ] || [ ! -f "$KLEOD_DIR/kleod.map" ] || [ ! -d "$KLEOD_DIR/build/kleod" ]; then
    say "kleod has not been built yet: building it once (make extract, make compare) -- see $LOG"
    need arm-none-eabi-as "Install binutils-arm-none-eabi (kleod's INSTALL.md)."
    [ -x "$KLEOD_DIR/tools/agbcc/bin/agbcc" ] || fail 4 "kleod needs agbcc: run pret/agbcc's build.sh and
'install.sh $KLEOD_DIR' first (kleod's INSTALL.md)."
    (cd "$KLEOD_DIR" && make extract && make compare -j"$JOBS") >> "$LOG" 2>&1 ||
        { tail -n 20 "$LOG" >&2; fail 5 "the kleod build failed (full log: $LOG)"; }
fi

# ---- 4. the Dreamcast executable -------------------------------------------------------------------
say "building the Dreamcast executable (this takes a few minutes the first time) -- see $LOG"
# Release options, set explicitly so that an earlier test build's settings are not reused.
make -C "$HERE" ROOT="$KLEOD_DIR" -j"$JOBS" RENDERER=pvr AUDIO=1 SCRIPT= MAX_FRAMES= AUTOPLAY= FREEZE= \
    PVRDBG= AUDIOTEST= AUDIOCHECK= >> "$LOG" 2>&1 ||
    { tail -n 25 "$LOG" >&2; fail 5 "the Dreamcast build failed (full log: $LOG)"; }
ELF="$HERE/build/klonoa-dc.elf"
[ -s "$ELF" ] || fail 5 "the build finished but $ELF is missing"

# ---- 5. the CDI -------------------------------------------------------------------------------
CDI="$OUT/klonoa-dc.cdi"
say "creating the CDI"
rm -f -- "$CDI.tmp.cdi"
# CDI_PADDING=0 skips mkdcdisc's data-track padding: an image of about 10 MB instead of about 740 MB.
# Padding places the data at the fast outer edge of a burned CD; emulators and drive emulators
# do not need it.
PAD_OPT=
[ "${CDI_PADDING:-1}" = 0 ] && PAD_OPT=--disable-data-track-padding
# shellcheck disable=SC2086
"$MKDCDISC" --no-banner $PAD_OPT -e "$ELF" -o "$CDI.tmp.cdi" -n "KLONOA EOD DC PORT" -a "KL-EOD-DC" \
    >> "$LOG" 2>&1 || { tail -n 20 "$LOG" >&2; rm -f -- "$CDI.tmp.cdi"; fail 6 "mkdcdisc failed (full log: $LOG)"; }
[ -s "$CDI.tmp.cdi" ] || { rm -f -- "$CDI.tmp.cdi"; fail 6 "mkdcdisc produced no output"; }
mv -f -- "$CDI.tmp.cdi" "$CDI" || fail 6 "could not write $CDI"

say "done: $CDI ($(stat -c %s "$CDI") bytes)"
cat << EOF

It contains your ROM's data: keep it for your own use, do not share it.
Tested in the Flycast emulator only; it has not been tried on a real Dreamcast.
EOF
exit 0
