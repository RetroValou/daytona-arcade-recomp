# Vita Model 2 recomp

The goal of this project is a standard Sega Model 2 static recompiler for the
PS Vita: recompile a Model 2 game from your own ROM set and play it on the Vita.

The project is based on
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp).
The PC, Android and Dreamcast parts come from that project as they are; they
are not meant to be tested or to work here.

The aim is to play Sega Model 2 games on the PS Vita in decent conditions. It
is not to reproduce the hardware 100% exactly, nor to turn the game code into
readable source.

Games so far, each from its own MAME ROM set (the list below): Daytona USA,
Sega Rally Championship, Virtua Fighter 2, Virtua Cop, Virtua Cop 2 and Manx
TT Superbike, one player each (the Virtua Cops' light gun is the Vita's
touch screen or the PC's mouse). What is specific to a game (its ROMs,
addresses, input wiring, hooks) lives in `src/games/` and `scripts/games.py`;
the rest is the Model 2 board, shared by every game. To add one, see
[docs/adding-a-game.md](docs/adding-a-game.md). Fighting Vipers is recompiled
too, as a test only (see Limits).

## ROM sets

You need your own ROM sets, as complete MAME sets: **full merged** archives
(`.zip`, or `.7z` on the computer), each holding every file of the set,
including the files a clone shares with its parent. A split set (a clone's
archive with only the files that differ from its parent) is refused: the
importer checks every file of the set by name and CRC. Other versions of a
game (another revision, region or cabinet) are not supported unless listed.

Compatible sets, the archive to put in `roms/` on the computer and the file
the Vita package reads (details in [HOW_TO_COMPILE_RELEASE.md](HOW_TO_COMPILE_RELEASE.md)):

* **Daytona USA**: `daytona93` (Deluxe '93), board Model 2 (original).
  `roms/daytona93.zip`; Vita `ux0:data/daytona93/daytona93.zip`. The
  package can also bundle `daytona` (Revision A, 1994; `roms/daytona.zip`),
  switched by the ROM option.
* **Sega Rally Championship**: `srallyc` (Revision C), board Model 2A.
  `roms/srallyc.zip` or MAME's merged `roms/srallycb.zip`; Vita
  `ux0:data/srallyc/srallyc.zip`.
* **Virtua Fighter 2**: `vf2` (Version 2.1), board Model 2A.
  `roms/vf2.zip`; Vita `ux0:data/vf2/vf2.zip`.
* **Virtua Cop**: `vcop` (Revision B), board Model 2 (original).
  `roms/vcop.zip`; Vita `ux0:data/vcop/vcop.zip`.
* **Virtua Cop 2**: `vcop2`, board Model 2A.
  `roms/vcop2.zip`; Vita `ux0:data/vcop2/vcop2.zip`.
* **Manx TT Superbike**: `manxttc` (Twin cabinet, Revision C), board
  Model 2A. MAME's merged `roms/manxtt.zip` holding the `manxttc` files
  (below); Vita `ux0:data/manxttc/manxttc.zip` (the same archive, renamed).
* **Fighting Vipers** (test only, see Limits): `fvipers` (Revision D),
  board Model 2B. `roms/fvipers.zip`; Vita `ux0:data/fvipers/fvipers.zip`.

Manx TT Superbike is built from its **Twin** version, MAME's `manxttc`
(Revision C, set to the Twin cabinet: the SCSP sound board only). The
DX version (`manxtt`, `manxttdx`) adds a Model 1 sound board for the
engine and a moving bike, which are not emulated. The archive must hold
**both** the parent set `manxtt` and the Twin's Revision C files, at its
root or in a `manxttc/` folder (as MAME's merged `manxtt.zip` has them):
the program `epr-18822c` to `epr-18825c`, the Twin's sound program
`epr-18826.30`, its first sample ROM `mpr-18827.31` and its default
EEPROM `manxttc_twin_nvran`. A merged `manxtt.zip` without the
`manxttc` files does not load. If yours has them, only its name
changes: in `roms/` it can stay `manxtt.zip`; on the Vita, rename it
`manxttc.zip` and copy it to `ux0:data/manxttc/manxttc.zip`.

## Limits

**Only the original Model 2 and the Model 2A boards run on the PS Vita.**
The Model 2B and Model 2C boards replace the Fujitsu TGP geometry
coprocessor of the first two with heavier ones (2B: an Analog Devices SHARC
ADSP-21062; 2C: the TGPx4, Fujitsu MB86235). With the current code, the
Vita's processor cannot run them at the game's speed.

* **Fighting Vipers (Model 2B) is a test, not a game to play on the Vita.**
  It is recompiled and runs on the PC (its SHARC program recompiled too,
  identical to MAME), and a Vita package can be built, but in a fight the
  Vita reaches about 26 frames per second against the arcade's 57.5: the
  SHARC alone takes about 20 ms of a 17.4 ms frame on the main core, after
  its recompiler was aligned on the TGP's. It is kept as a test of the
  Model 2B board; `build_vita.py --game all` leaves it out
  (`scripts/games.py`).
* **Model 2C games are not supported.** Their coprocessor is not
  recompiled; they would be expected to hit the same wall as the 2B.
* Model 2 and 2A games run close to or at full speed on the Vita, each with
  its own limits (one player, the light gun on the touch screen, Manx TT's
  Twin cabinet only).

The original README is kept in [README_fork.md](README_fork.md).
