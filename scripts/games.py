"""The Model 2 games the scripts know: the games, then their MAME sets.

This is the scripts' half of a game's description; the runtime's half (ROM
files and layout, idle loops, input wiring, hooks) is its file in src/games/.
Adding a game: docs/adding-a-game.md.

Everything a game builds goes under one folder, build_<game>/ (git-ignored):

  build_<game>/recomp/<set>/rom_cache/  the ROM images m2import makes
  build_<game>/recomp/<set>/gen/        the recompiled code (<set>/, <set>_tgp/,
                                        <set>_snd/: CMake's M2_GEN_ROOT)
  build_<game>/pc/<set>/                the desktop build (the tools, m2run, the game)
  build_<game>/vita/, vita_diag/        the Vita builds: <game>.vpk, <game>_diag.vpk

Per game (GAMES):
  title         the game's name
  sets          its MAME sets, the main one (what --game alone builds) first
  vita_title_id the Vita package's title ID (4 capital letters, 5 digits);
                one per game, so the games install side by side
  vita_name     the Vita package's name
  vita_link     link play compiled into the Vita package (build_vita.py
                --link/--no-link override it): only where it was tested on
                Vitas, Daytona. The menu also needs a set with the
                communication board (Game::link)
  vita          the Vita package works (build_vita.py --game all builds only
                these; default True): Fighting Vipers waits for its SHARC
  The Vita's LiveArea images: platform/vita/logo_livearea/<game>/.

Per set (SETS):
  game          the game it belongs to
  label         this set among the game's sets
  roms          archive names to look for under roms/ (.zip or .7z); the
                importer checks every file's CRC, so a merged archive named
                after a clone is fine
  board         the i960 board: "model2o" (Model 2), "model2a" (2A-CRX) or
                "model2b" (2B-CRX), as rt::BoardType (src/games/game.h);
                m2recomp --board inlines its bus (recompile.py --inline-bus,
                build_vita.py --inline-bus; not for model2b)
  recomp        extra m2recomp options: the i960 memory map
                (tools/m2recomp/main.cpp)
  sndrecomp     extra m2sndrecomp options: the sound board's 68000 map
                (--scsp for the SCSP board; tools/m2sndrecomp/main.cpp)
  companion     the game's other set a Vita package can bundle and switch
                to (build_vita.py --revision-a-self; the launcher runs
                app0:/<companion>.self), or None. As Game::companion
                (src/games/).
  vita_fast     the build_vita.py game code modes checked on this set:
                "fast_inaccuracy" (--fast-inaccuracy), "fast_gen" (--fast-gen).
                Both turn fixed accesses to work RAM (0x00500000-0x005fffff,
                every Model 2 board) into direct ones
  inputs        make_input.py's script names -> (MAME port, mask) where the
                game's differ from the common driving-cabinet layout
                (CONTROLS), and "full_range": its analog inputs are 0x00-0xff
                rather than 0x20-0xe0
"""

import os

# The common layout: MAME's daytona ports. name -> (port, mask).
CONTROLS = {
    "coin": (":IN0", 0x01), "service": (":IN0", 0x08), "test": (":IN0", 0x04),
    "start": (":IN0", 0x10),
    "vr1": (":IN0", 0x20), "vr2": (":IN0", 0x40), "vr3": (":IN0", 0x80), "vr4": (":IN1", 0x01),
    "gear0": (":GEARS", 0x01), "gear1": (":GEARS", 0x02), "gear2": (":GEARS", 0x04),
    "gear3": (":GEARS", 0x08), "gear4": (":GEARS", 0x10),
    "steer": (":STEER", 0xFF), "accel": (":ACCEL", 0xFF), "brake": (":BRAKE", 0xFF),
}

GAMES = {
    "daytona": {
        "title": "Daytona USA", "sets": ("daytona93", "daytona"),
        "vita_title_id": "DAYT00093", "vita_name": "Daytona USA", "vita_link": True,
    },
    "segarally": {
        "title": "Sega Rally Championship", "sets": ("srallyc",),
        "vita_title_id": "SRAL00094", "vita_name": "Sega Rally Championship", "vita_link": False,
    },
    "virtuafighter2": {
        "title": "Virtua Fighter 2", "sets": ("vf2",),
        "vita_title_id": "VFTR00095", "vita_name": "Virtua Fighter 2", "vita_link": False,
    },
    "virtuacop": {
        "title": "Virtua Cop", "sets": ("vcop",),
        "vita_title_id": "VCOP00096", "vita_name": "Virtua Cop", "vita_link": False,
    },
    "virtuacop2": {
        "title": "Virtua Cop 2", "sets": ("vcop2",),
        "vita_title_id": "VCOP00097", "vita_name": "Virtua Cop 2", "vita_link": False,
    },
    "manxtt": {
        "title": "Manx TT Superbike", "sets": ("manxttc",),
        "vita_title_id": "MANX00098", "vita_name": "Manx TT Superbike", "vita_link": False,
    },
    "fightingvipers": {
        "title": "Fighting Vipers", "sets": ("fvipers",),
        "vita_title_id": "FVIP00099", "vita_name": "Fighting Vipers", "vita_link": False,
        "vita": False,  # the board does not run its SHARC yet
    },
    "deadoralive": {
        "title": "Dead or Alive", "sets": ("doaa",),
        "vita_title_id": "DOAL00100", "vita_name": "Dead or Alive", "vita_link": False,
    },
}

# The game the scripts build when none is given.
DEFAULT = "daytona"

SETS = {
    "daytona93": {
        "game": "daytona", "label": "Deluxe '93",
        "roms": ["daytona93"], "board": "model2o", "recomp": [], "sndrecomp": [],
        "vita_fast": ("fast_inaccuracy", "fast_gen"),
        "companion": "daytona", "wram_bus": (), "inputs": {}, "full_range": False,
    },
    "daytona": {
        "game": "daytona", "label": "Revision A, 1994",
        "roms": ["daytona"], "board": "model2o", "recomp": [], "sndrecomp": [],
        "vita_fast": ("fast_inaccuracy", "fast_gen"),
        "companion": "daytona93", "wram_bus": (0x00500000,), "inputs": {}, "full_range": False,
    },
    # Model 2A-CRX: RAM at 0x200000 (no ROM mirror); the boot copies ROM
    # 0x1000-0x60fff to work RAM 0x5a0000 and runs from there. Inputs: MAME's
    # srallyc ports (start IN0 0x40, one VR button, the hand brake on IN2).
    "srallyc": {
        "game": "segarally", "label": "Revision C",
        "roms": ["srallyc", "srallycb"], "board": "model2a", "recomp": ["--no-mirror", "--alias", "5a0000:1000:60000"], "sndrecomp": ["--scsp"],
        "vita_fast": ("fast_inaccuracy",), "companion": None, "wram_bus": (),
        "inputs": {"start": (":IN0", 0x40), "vr1": (":IN0", 0x20), "vr2": (":IN0", 0x20),
                   "vr3": (":IN0", 0x20), "vr4": (":IN0", 0x20), "handbrake": (":IN2", 0xFF)},
        "full_range": True,
    },
    # Model 2A-CRX like srallyc, but its program runs from ROM (no alias). One
    # player: the joystick and Punch/Kick/Guard on MAME's IN1 (src/games/vf2.cpp
    # maps them from the common controls). --fast-inaccuracy checked on the PC:
    # vf2_fight and 8,000 frames of attract run through (one more seed), the same
    # scenes and sound level as the exact code, not the same frames (timing).
    "vf2": {
        "game": "virtuafighter2", "label": "Version 2.1",
        "roms": ["vf2"], "board": "model2a", "recomp": ["--no-mirror"], "sndrecomp": ["--scsp"],
        "vita_fast": ("fast_inaccuracy",), "companion": None, "wram_bus": (0x00500000,),
        "inputs": {"vr1": (":IN1", 0x01), "vr2": (":IN1", 0x02), "vr3": (":IN1", 0x04)},
        "full_range": False,
    },
    # Original Model 2 like Daytona (Model 1 sound board), its I/O board with
    # the light gun interface (MAME model1io2). One player: the trigger on IN1
    # 0x01, the gun's aim in MAME's P1_X/P1_Y units (gunx/guny in scripts/inputs).
    # --fast-inaccuracy checked on the PC: vcop_shoot, a scripted 9,000-frame game
    # and 8,000 frames of attract run through (no new seed); shots and the reload
    # as with the exact code, not the same frames (timing).
    "vcop": {
        "game": "virtuacop", "label": "Revision B",
        "roms": ["vcop"], "board": "model2o", "recomp": [], "sndrecomp": [],
        "vita_fast": ("fast_inaccuracy",), "companion": None, "wram_bus": (),
        "inputs": {"trigger": (":IN1", 0x01), "gunx": (":P1_X", 0x3FF), "guny": (":P1_Y", 0x3FF)},
        "full_range": False,
    },
    # Model 2A-CRX like vf2 (program in ROM, --no-mirror; SCSP sound), the light
    # gun through the 315-5649's serial channel 2 (MAME lightgun_mux_r/w). One
    # player: the trigger on IN1 0x01, the aim in MAME's P1_X/P1_Y units.
    # --fast-inaccuracy checked on the PC: vcop2_shoot, a scripted 9,000-frame
    # game (the same 898 sound commands as the exact code) and 8,000 frames of
    # attract run through (no new seed).
    "vcop2": {
        "game": "virtuacop2", "label": "Virtua Cop 2",
        "roms": ["vcop2"], "board": "model2a", "recomp": ["--no-mirror"], "sndrecomp": ["--scsp"],
        "vita_fast": ("fast_inaccuracy",), "companion": None, "wram_bus": (0x00500000,),
        "inputs": {"trigger": (":IN1", 0x01), "gunx": (":P1_X", 0x3FF), "guny": (":P1_Y", 0x3FF)},
        "full_range": False,
    },
    # Manx TT Superbike, Revision C, the Twin cabinet (MAME's manxttc: its
    # EEPROM is set to Twin, with the Twin sound ROMs; the DX's Model 1 sound
    # board and moving bike are not used). Model 2A-CRX like vf2 (program in
    # ROM, --no-mirror; SCSP sound). The bike: THROTTLE, BRAKE and BANK (the
    # lean) as accel, brake and steer; Start 0x40; shift up/down on IN1
    # 0x10/0x20 as vr1/vr2. Its ROMs are in MAME's merged manxtt.zip.
    # --fast-inaccuracy checked on the PC: manxttc_race (the same 9,079 sound
    # command bytes as the exact code), 8,000 frames of attract and the test
    # mode walk run through (no new seed).
    "manxttc": {
        "game": "manxtt", "label": "Twin, Revision C",
        "roms": ["manxttc", "manxtt"], "board": "model2a", "recomp": ["--no-mirror"], "sndrecomp": ["--scsp"],
        "vita_fast": ("fast_inaccuracy",), "companion": None, "wram_bus": (),
        "inputs": {"start": (":IN0", 0x40), "vr1": (":IN1", 0x10), "vr2": (":IN1", 0x20),
                   "steer": (":BANK", 0xFF), "accel": (":THROTTLE", 0xFF), "brake": (":BRAKE", 0xFF)},
        "full_range": True,
    },
    # Model 2B-CRX: the 2A-CRX i960 board with a SHARC copro (program in ROM,
    # --no-mirror; SCSP sound). Bring-up: the i960 side only, the SHARC
    # replayed from MAME's trace (m2native in M1 mode); no game build yet.
    # MAME's inputs are vf2's.
    "fvipers": {
        "game": "fightingvipers", "label": "Revision D",
        "roms": ["fvipers"], "board": "model2b", "recomp": ["--no-mirror"], "sndrecomp": ["--scsp"],
        "vita_fast": ("fast_inaccuracy",), "companion": None, "wram_bus": (0x00500000,),
        "inputs": {"vr1": (":IN1", 0x01), "vr2": (":IN1", 0x02), "vr3": (":IN1", 0x04),
                   "down": (":IN1", 0x10), "up": (":IN1", 0x20), "right": (":IN1", 0x40), "left": (":IN1", 0x80)},
        "full_range": False,
    },
    # Dead or Alive, Revision A (Tecmo, Dec 4 1996): Model 2A-CRX like vf2
    # (program in ROM, --no-mirror; SCSP sound) with Sega's 317-0229
    # protection chip (Game::protection). The parent, doa, is a Model 2B set;
    # doaa's ROMs are in MAME's merged doa.zip. One player: Hold, Punch, Kick
    # on IN1 0x01/0x02/0x04 (vr3, vr1, vr2), the joystick as vf2's.
    # --fast-inaccuracy checked on the PC: doaa_fight 6,000 and 8,000 frames of
    # attract run through (no new seed; the same sound command bytes, 625 and
    # 877, as the exact code; the wait loop still skipped: wram_bus).
    "doaa": {
        "game": "deadoralive", "label": "Revision A",
        "roms": ["doaa", "doa"], "board": "model2a", "recomp": ["--no-mirror"], "sndrecomp": ["--scsp"],
        "vita_fast": ("fast_inaccuracy",), "companion": None, "wram_bus": (0x0054b150,),
        "inputs": {"vr1": (":IN1", 0x02), "vr2": (":IN1", 0x04), "vr3": (":IN1", 0x01),
                   "down": (":IN1", 0x10), "up": (":IN1", 0x20), "right": (":IN1", 0x40), "left": (":IN1", 0x80)},
        "full_range": False,
    },
}


def rom_archives(romset):
    """The set's archives present under roms/ (.zip or .7z, any of its names)."""
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "roms")
    return [p for n in SETS[romset]["roms"] for ext in ("zip", "7z")
            if os.path.exists(p := os.path.join(root, f"{n}.{ext}"))]


def game_of(romset):
    """The game a set belongs to."""
    return SETS[romset]["game"]


def main_set(game):
    """The set --game alone builds: the game's first."""
    return GAMES[game]["sets"][0]


def resolve(game=None, romset=None):
    """(game, set) from the command line's --game and --set, either or both:
    the set alone gives its game, the game alone its main set. ValueError
    when the set is not one of the game's."""
    if romset is None:
        game = game or DEFAULT
        return game, main_set(game)
    if game is not None and game_of(romset) != game:
        raise ValueError(f"{romset} is not a set of {game} (its sets: {', '.join(GAMES[game]['sets'])})")
    return game_of(romset), romset


def title(romset):
    """'Daytona USA Deluxe '93 (daytona93)'."""
    return f"{GAMES[game_of(romset)]['title']} {SETS[romset]['label']} ({romset})"


# The folders, relative to the repository (see the module's docstring).
def game_dir(game):
    return f"build_{game}"


def recomp_dir(romset):
    return f"{game_dir(game_of(romset))}/recomp/{romset}"


def rom_cache(romset):
    return f"{recomp_dir(romset)}/rom_cache"


def gen_root(romset):
    return f"{recomp_dir(romset)}/gen"


def pc_dir(romset):
    return f"{game_dir(game_of(romset))}/pc/{romset}"


def vita_dir(game, diagnostics=False):
    return f"{game_dir(game)}/vita" + ("_diag" if diagnostics else "")


def vpk_name(game, diagnostics=False):
    return f"{game}_diag.vpk" if diagnostics else f"{game}.vpk"


def controls(romset):
    """make_input.py's name -> (port, mask) table for a set (by MAME name;
    unknown names get the common layout)."""
    return dict(CONTROLS, **SETS.get(romset, {}).get("inputs", {}))
