#!/usr/bin/env bash
# Opt-in game-to-native-audio health run; all generated code and WAVs stay in
# the configured, ignored build tree. Requires the user's imported ROM cache.
set -euo pipefail
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$repo_root/build_daytona/pc/daytona93"}
frames=${2:-6000}
rom_dir=${3:-"$repo_root/build_daytona/recomp/daytona93/rom_cache"}
set_name=daytona93
wav_path=${4:-}

if [[ ${SKIP_BUILD:-0} != 1 ]]; then
    cmake --build "$build_dir" --target m2run -j "${BUILD_JOBS:-2}"
fi
# Sound code objects satisfy the shared runtime's link symbols only: the test
# asserts GameLoop(false).sound()==nullptr, and never creates the sound board.
# The generated code lives outside the build tree (build_<game>/recomp/<set>/gen):
# CMake names its objects after its path, so find them.
gen_objects=$(find "$build_dir/CMakeFiles/m2run.dir" -type d -path "*/gen/$set_name" -print -quit)
if [[ -z "$gen_objects" ]]; then
    printf 'No generated host objects for %s under %s\n' "$set_name" "$build_dir/CMakeFiles/m2run.dir" >&2
    exit 2
fi
objects=("$gen_objects"/*.o "${gen_objects}_tgp"/*.o "${gen_objects}_snd"/*.o)
for object in "${objects[@]}"; do
    if [[ ! -f "$object" ]]; then
        printf 'Missing generated host object: %s\n' "$object" >&2
        exit 2
    fi
done
libraries=("$build_dir/libruntime.a")
if [[ -f "$build_dir/liblzma7z.a" ]]; then libraries+=("$build_dir/liblzma7z.a"); fi
libraries+=("$build_dir/libtrace.a" "$build_dir/libi960.a"
            "$build_dir/libsoftfloat.a" "$build_dir/libymfm.a")
"${CXX:-c++}" -std=c++20 -O2 -fno-fast-math -ffp-contract=off \
    -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -DTHREAD_LOCAL=__thread \
    -I"$repo_root/src" -I"$repo_root/extern/ymfm/src" \
    "$repo_root/tests/test_native_sound_rom.cpp" \
    "$repo_root/src/runtime/native_sample_mixer.cpp" \
    "$repo_root/src/runtime/native_sound_sequencer.cpp" \
    "$repo_root/src/runtime/native_sound_engine.cpp" \
    "${objects[@]}" "${libraries[@]}" -o "$build_dir/test_native_sound_rom"
arguments=("$rom_dir" "$frames")
if [[ -n "$wav_path" ]]; then arguments+=("$wav_path"); fi
"$build_dir/test_native_sound_rom" "${arguments[@]}"
