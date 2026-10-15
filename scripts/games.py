"""The Model 2 games the scripts know, one entry per MAME set.

This is the scripts' half of a game's description; the runtime's half (ROM
files and layout, idle loops, input wiring, hooks) is its file in src/games/.
Adding a game: docs/adding-a-game.md.

Per set:
  title         the game's name
  label         this set among the game's sets
  roms          archive names to look for under roms/ (.zip or .7z); the
                importer checks every file's CRC, so a merged archive named
                after a clone is fine
  board         the i960 board: "model2o" (Model 2) or "model2a" (2A-CRX), as
                rt::BoardType (src/runtime/game.h); m2recomp --board inlines
                its bus (recompile.py --inline-bus, build_vita.py's fast modes)
  recomp        extra m2recomp options: the i960 memory map
                (tools/m2recomp/main.cpp)
  sndrecomp     extra m2sndrecomp options: the sound board's 68000 map
                (--scsp for the SCSP board; tools/m2sndrecomp/main.cpp)
  vita_title_id the Vita package's title ID (4 capital letters, 5 digits);
                sets of one game sharing a package share it
  vita_name     the Vita package's name
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
    "daytona93": {
        "title": "Daytona USA", "label": "Deluxe '93",
        "roms": ["daytona93"], "board": "model2o", "recomp": [], "sndrecomp": [],
        "vita_title_id": "DAYT00093", "vita_name": "Daytona USA", "vita_fast": ("fast_inaccuracy", "fast_gen"),
        "companion": "daytona", "inputs": {}, "full_range": False,
    },
    "daytona": {
        "title": "Daytona USA", "label": "Revision A, 1994",
        "roms": ["daytona"], "board": "model2o", "recomp": [], "sndrecomp": [],
        "vita_title_id": "DAYT00093", "vita_name": "Daytona USA", "vita_fast": ("fast_inaccuracy", "fast_gen"),
        "companion": "daytona93", "inputs": {}, "full_range": False,
    },
    # Model 2A-CRX: RAM at 0x200000 (no ROM mirror); the boot copies ROM
    # 0x1000-0x60fff to work RAM 0x5a0000 and runs from there. Inputs: MAME's
    # srallyc ports (start IN0 0x40, one VR button, the hand brake on IN2).
    "srallyc": {
        "title": "Sega Rally Championship", "label": "Revision C",
        "roms": ["srallyc", "srallycb"], "board": "model2a", "recomp": ["--no-mirror", "--alias", "5a0000:1000:60000"], "sndrecomp": ["--scsp"],
        "vita_title_id": "SRAL00094", "vita_name": "Sega Rally Championship", "vita_fast": ("fast_inaccuracy",), "companion": None,
        "inputs": {"start": (":IN0", 0x40), "vr1": (":IN0", 0x20), "vr2": (":IN0", 0x20),
                   "vr3": (":IN0", 0x20), "vr4": (":IN0", 0x20), "handbrake": (":IN2", 0xFF)},
        "full_range": True,
    },
}

# The set builds use when none is given.
DEFAULT = "daytona93"


def build_dir(game, base="build"):
    """The host build directory setup.py makes for a set: build/ for the
    default set, build-SET for the others."""
    return base if game == DEFAULT else f"{base}-{game}"


def controls(game):
    """make_input.py's name -> (port, mask) table for a set (by MAME name;
    unknown names get the common layout)."""
    return dict(CONTROLS, **GAMES.get(game, {}).get("inputs", {}))
