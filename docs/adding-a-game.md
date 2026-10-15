# Adding a Model 2 game

The runtime is the Model 2 board. A game is described in a few places, and
the board code is not touched (design doc, Architecture, "Games"). The
existing games are the examples to follow: `src/games/daytona.cpp`
(original Model 2, Model 1 sound board, two sets) and `src/games/srallyc.cpp`
(Model 2A-CRX, program copied to RAM, SCSP sound board).

Below, `<set>` is the game's MAME set name, such as `srallyc`, and `<game>`
the game's short name in the scripts, such as `segarally`: everything the
game builds goes in its folder, `build_<game>/`, and its Vita package is
`<game>.vpk`.

## 1. The game's record: `src/games/<game>.cpp`

Define one `extern const rt::Game <set>_game = {...};` per set
(`src/runtime/game.h` describes every field):

- `board`: `Model2o` (original) or `Model2a` (2A-CRX). The 2B and 2C boards
  are not built yet.
- `roms`: MAME's `ROM_START(<set>)` as `RomLoad` lines (file, CRC, region,
  offset, size). `ROM_LOAD32_WORD` pairs go to offsets 0 and 2. Load only the
  regions the runtime uses. `mirror_from`: main_data's `ROM_COPY` source, or
  0 for none.
- `tgp`: where the TGP program the i960 uploads at boot sits, with its length
  in words and its CRC. Find it by dumping MAME's `copro_tgp_program` after
  boot and searching the ROMs for it.
- `idle`: the i960 address ranges of the game's wait-for-vblank loops. Find
  them by looking at where MAME takes the vblank interrupts. Without them,
  every frame runs to the instruction cap.
- `sound`: the sound board. `Model1` is the original Model 2's board (load
  its `SoundProgram`, `Pcm1` and `Pcm2` files). `Scsp` is the 2A/2B/2C board
  (load `SoundProgram` and `Samples`, and pass `--scsp` to m2sndrecomp in
  `scripts/games.py`). `native_sound`: for a Model 1 board, the driver's
  table addresses for the native audio engine, or nullptr.
- `io` (2A-CRX): which of the cabinet's controls (`rt::Inputs`) each
  315-5649 port and analog channel carries, from MAME's
  `INPUT_PORTS(<set>)`.
- `link`: whether it has the communication board. `companion`: the game's
  other set that one Vita package can switch to, or nullptr.

Then add the record to `kGames` in `src/games/games.cpp`, and add the file to
the runtime's sources in `CMakeLists.txt`, `platform/vita/CMakeLists.txt` (the
`foreach` over `src/games`), and the Dreamcast build files if you use them.

Hooks into the game's code (enhancements that need its RAM addresses) are
declared in `src/games/hooks.h` and defined in the game's file. Each one must
do nothing while its option is off.

## 2. The scripts' entry: `scripts/games.py`

Add the game to `GAMES` under its `<game>` name (lower case: it names the
`build_<game>/` folder and the `<game>.vpk` package): its title, its sets (the
main one first), the Vita title ID (4 capital letters and 5 digits, one per
game) and package name. Then add each `<set>` to `SETS`: its game, its label,
the archive names to look for under `roms/`, the board, the extra `m2recomp`
options (`--no-mirror`, and an `--alias` for code the boot copies to RAM),
`vita_fast` (the `--fast-*` Vita modes checked on it: empty until then), the
companion set, and the input ports where they differ from the common layout.
`recompile.py`, `setup.py`, `seed_scan.py`, `make_input.py`, `m2_check.sh` and
`build_vita.py` all read it.

Create the game's image folder for the Vita, `platform/vita/logo_livearea/<game>/`,
with an empty `.gitkeep` in it (the images themselves are never committed).

## 3. Seeds: `seeds/<set>.txt`

Create the file. CMake rejects a set without one. Then:

```sh
python3 scripts/recompile.py --game <game>   # imports the ROMs, recompiles, builds (build_<game>/)
python3 scripts/seed_scan.py --game <game> --append
python3 scripts/recompile.py --game <game>
./build_<game>/pc/<set>/m2run build_<game>/recomp/<set>/rom_cache 1800 --dump out --every 300
```

(`--set <set>` picks another of the game's sets.)

A jump to code with no recompiled block stops the game and names the
address. Add that address to the seeds and recompile. Optional hooks go in
`seeds/<set>_hooks.txt` ("ADDRESS name" lines).

The sound 68000's seeds go in `seeds/<set>_snd.txt`. Collect them from a
MAME run with `M2TRACE_SNDLOG` and `M2TRACE_SNDPCS` set (patch 0004); the
second lists every address the 68000 executed. Check the result with
`m2sndcheck SOUND_PROGRAM.bin SNDLOG [--scsp SAMPLES.bin]`.

## 4. Check it

- Look at m2run's dumps (`scripts/rgb2png.py`) next to MAME at the same
  frames.
- Lockstep against MAME: record a stream header once, then
  `SET=<set> scripts/m2_check.sh <scenario>` with an input
  script from `scripts/inputs/`.
- Vita: `python3 scripts/build_vita.py --game <game> --gpu-gl` gives
  `build_<game>/vita/<game>.vpk`. For testing on the console, never pass
  `--release` (LTO is much slower to build).
