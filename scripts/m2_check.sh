#!/usr/bin/env sh
# Lockstep check: record a run in MAME (trace + IRQ log), build the images
# from the user's ROM set, and replay the run through the recompiled native
# code (m2native) and the reference interpreter (m2replay), and MAME's TGP
# log through the recompiled TGP program (m2tgpcheck); m2native also holds
# the native geometrizer to MAME's display lists at every vblank; and MAME's
# sound-CPU log through the recompiled sound 68000 (m2sndcheck).
# Game-derived files stay in git-ignored traces/ and build_<game>/.
#
#   scripts/m2_check.sh                 600 frames of attract, no input
#   scripts/m2_check.sh SCENARIO        scripts/inputs/SCENARIO.txt (coin up,
#                                       race, test mode...), its own frame count
#
# A scenario's input stream is built from the script with make_input.py; it
# needs one recorded stream for the field layout (traces/hdr.m2in, recorded
# once with M2TRACE_RECORD_INPUT). The trace is deleted after the check
# unless M2_CHECK_KEEP is set.
#
# SET picks another ROM set (default daytona93), e.g. SET=srallyc; its desktop
# build and images are the game's folder's (scripts/games.py: build_<game>/pc/<set>,
# build_<game>/recomp/<set>/rom_cache; BUILD overrides the first). That set's
# stream header is traces/hdr_SET.m2in (traces/hdr.m2in for daytona93). A
# set with no sound program skips m2sndcheck.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:-attract}"
SET="${SET:-daytona93}"
games_path() { python3 -c "import sys; sys.path.insert(0, 'scripts'); import games; print(games.$1('$SET'))"; }
BUILD="${BUILD:-$(cd "$ROOT" && games_path pc_dir)}"
IMG="$(cd "$ROOT" && games_path rom_cache)"
cd "$ROOT"
if [ "$SET" = daytona93 ]; then
    HDR=traces/hdr.m2in
    [ -f "$IMG/tgp_program.bin" ] || python3 scripts/m2import.py roms/daytona93.zip "$IMG"
else
    HDR="traces/hdr_$SET.m2in"
    [ -f "$IMG/tgp_program.bin" ] || { echo "no $IMG: run scripts/recompile.py --set $SET" >&2; exit 1; }
    # MAME wants roms/SET.zip; when the set is in another archive
    # (scripts/games.py: srallycb.zip holds srallyc), link it under that
    # name in traces/roms and point MAME there.
    if [ ! -f "roms/$SET.zip" ]; then
        A="$(python3 -c "import sys; sys.path.insert(0, 'scripts'); import games, os
print(next(f'roms/{n}.zip' for n in games.SETS['$SET']['roms'] if os.path.exists(f'roms/{n}.zip')))")"
        mkdir -p traces/roms
        ln -sf "$ROOT/$A" "traces/roms/$SET.zip"
        export ROMPATH="$ROOT/traces/roms;$ROOT/roms"
    fi
fi
export SYSTEM="$SET"
# Scenario names already prefixed by the set (srallyc_race) are not doubled.
case "$NAME" in "${SET}_"*|"$SET") OUT="traces/check_$NAME" ;; *) OUT="traces/check_${SET}_$NAME" ;; esac
[ "$SET" = daytona93 ] && OUT="traces/check_$NAME"
if [ "$NAME" = attract ]; then
    M2TRACE_FRAMES=600 M2TRACE_SNDLOG="$ROOT/$OUT.snd" M2TRACE_IRQLOG="$ROOT/$OUT.irq" M2TRACE_TGPLOG="$ROOT/$OUT.tgp" M2TRACE_GEOLOG="$ROOT/$OUT.geo" ./scripts/run_trace.sh "$OUT" >/dev/null 2>&1
else
    python3 scripts/make_input.py "$HDR" "scripts/inputs/$NAME.txt" "$OUT.m2in"
    FRAMES="$(awk '/^frames/{print $2}' "scripts/inputs/$NAME.txt")"
    M2TRACE_FRAMES="$FRAMES" M2TRACE_REPLAY_INPUT="$OUT.m2in" M2TRACE_SNDLOG="$ROOT/$OUT.snd" M2TRACE_IRQLOG="$ROOT/$OUT.irq" M2TRACE_TGPLOG="$ROOT/$OUT.tgp" M2TRACE_GEOLOG="$ROOT/$OUT.geo" \
        ./scripts/run_trace.sh "$OUT" >/dev/null 2>&1
fi
STATUS=0
[ -x "$BUILD/m2native" ] && { "./$BUILD/m2native" "$IMG" "$OUT/trace.m2tr" "$OUT.irq" "$OUT.geo" || STATUS=1; }
[ -n "${M2_CHECK_REPLAY:-}" ] || [ "$NAME" = attract ] && { "./$BUILD/m2replay" "$IMG" "$OUT/trace.m2tr" "$OUT.irq" || STATUS=1; }
[ -x "$BUILD/m2tgpcheck" ] && { "./$BUILD/m2tgpcheck" "$IMG" "$OUT.tgp" || STATUS=1; }
[ -x "$BUILD/m2sndcheck" ] && [ -s "$IMG/sound_program.bin" ] && { "./$BUILD/m2sndcheck" "$IMG/sound_program.bin" "$OUT.snd" || STATUS=1; }
[ -n "${M2_CHECK_KEEP:-}" ] || rm -f "$OUT/trace.m2tr" "$OUT.tgp" "$OUT.geo" "$OUT.snd"
exit $STATUS
