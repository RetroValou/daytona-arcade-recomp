# Adding a Model 2 game

The runtime is the Model 2 board. A game is described in a few places, and
the board code is not touched (design doc, Architecture, "Games"). The
existing games are the examples to follow: `src/games/daytona.cpp`
(original Model 2, Model 1 sound board, two sets) and `src/games/srallyc.cpp`
(Model 2A-CRX, program copied to RAM, no sound board yet).

Below, `<set>` is the game's MAME set name, such as `srallyc`.

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
- `model1_sound`: the game has the original Model 2's Model 1 sound board
  (load its `SoundProgram`, `Pcm1` and `Pcm2` files). Without it the game
  runs with no sound. `native_sound`: for a Model 1 board, the driver's
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

Add `<set>` to `GAMES`: its title and label, the archive names to look for
under `roms/`, the extra `m2recomp` options (`--no-mirror`, and an `--alias`
for code the boot copies to RAM), the Vita title ID (4 capital letters and 5
digits, one per game) and package name, `vita_fast` (whether the `--fast-*`
Vita modes apply: false until checked), the companion set, and the input
ports where they differ from the common layout. `recompile.py`, `setup.py`,
`seed_scan.py`, `make_input.py`, `m2_check.sh` and `build_vita.py` all read it.

## 3. Seeds: `seeds/<set>.txt`

Create the file. CMake rejects a set without one. Then:

```sh
python3 scripts/recompile.py --set <set> --build-dir build-<set>   # imports the ROMs, recompiles, builds
python3 scripts/seed_scan.py --set <set> --build-dir build-<set> --append
python3 scripts/recompile.py --set <set> --build-dir build-<set>
./build-<set>/m2run build-<set>/rom_cache/<set> 1800 --dump out --every 300
```

A jump to code with no recompiled block stops the game and names the
address. Add that address to the seeds and recompile. Optional hooks go in
`seeds/<set>_hooks.txt` ("ADDRESS name" lines).

## 4. Check it

- Look at m2run's dumps (`scripts/rgb2png.py`) next to MAME at the same
  frames.
- Lockstep against MAME: record a stream header once, then
  `SET=<set> BUILD=build-<set> scripts/m2_check.sh <scenario>` with an input
  script from `scripts/inputs/`.
- Vita: `python3 scripts/build_vita.py --set <set> --gpu-gl`. For testing on
  the console, never pass `--release` (LTO is much slower to build).
