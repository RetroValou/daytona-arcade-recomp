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

Games so far: Daytona USA (daytona93, daytona), Sega Rally Championship
(srallyc), Virtua Fighter 2 (vf2, Version 2.1, one player), Virtua Cop
(vcop, Revision B) and Virtua Cop 2 (vcop2), one player each: the light gun
is the Vita's touch screen or the PC's mouse; Manx TT Superbike (manxttc: the
Twin cabinet, Revision C, one player). What is specific to a game (its ROMs, addresses,
input wiring, hooks) lives in `src/games/` and `scripts/games.py`; the rest
is the Model 2 board, shared by every game. To add one, see
[docs/adding-a-game.md](docs/adding-a-game.md).

## ROM sets

You need your own ROM sets, as complete MAME sets: **full merged** archives
(`.zip`, or `.7z` on the computer), each holding every file of the set,
including the files a clone shares with its parent. A split set (a clone's
archive with only the files that differ from its parent) is refused: the
importer checks every file of the set by name and CRC. Put them in `roms/`
as `daytona93.zip`, `srallyc.zip` (or MAME's merged `srallycb.zip`),
`vf2.zip`, `vcop.zip`, `vcop2.zip` and `manxtt.zip`; on the Vita, see [HOW_TO_COMPILE_RELEASE.md](HOW_TO_COMPILE_RELEASE.md).

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

The original README is kept in [README_fork.md](README_fork.md).
