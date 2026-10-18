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
(srallyc), Virtua Fighter 2 (vf2, Version 2.1, one player) and Virtua Cop
(vcop, Revision B, one player: the light gun is the Vita's touch screen or
the PC's mouse). What is specific to a game (its ROMs, addresses,
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
`vf2.zip` and `vcop.zip`; on the Vita, see [HOW_TO_COMPILE_RELEASE.md](HOW_TO_COMPILE_RELEASE.md).

The original README is kept in [README_fork.md](README_fork.md).
