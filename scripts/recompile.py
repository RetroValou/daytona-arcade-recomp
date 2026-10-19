#!/usr/bin/env python3
"""Recompile a game's i960 code, its TGP program and the sound board's 68000
program to native C++ and build them. Same steps on Linux, macOS and Windows.

  recompile.py [--game GAME] [--set SET] [--config Release] [--fast_inaccuracy] [--tgp-profile]
               [--i960-profile] [--inline-bus] [--recomp-only]

The game is one of scripts/games.py's (daytona, the default, segarally);
--set picks another of its sets (daytona: daytona93, the default, or
daytona, Revision A). Needs the user's ROM set at roms/<set>.zip or
roms/<set>.7z (git-ignored), or under one of the other archive names
scripts/games.py gives that set (a merged srallycb archive holds srallyc's
files too). Everything derived from it goes under the game's folder,
build_<game>/ (git-ignored: never commit it):

  build_<game>/recomp/<set>/rom_cache/  the images m2import makes from the ROMs
  build_<game>/recomp/<set>/gen/        the recompiled C++ (the recompilation)
  build_<game>/pc/<set>/                the desktop build: the tools that
                                        recompile, m2run and the game

--recomp-only stops after the recompilation (the Vita build needs only that
and the tools).

--fast_inaccuracy turns on every recompiler speed option
for slow targets, above all the PS Vita:
  i960 (tools/m2recomp/main.cpp)
  1. the lockstep bookkeeping (interrupt and event checks, instruction count)
     once per basic block instead of before every instruction: no longer
     exact against MAME (interrupts are taken a few instructions later);
  2. direct chaining: the generated gen::run moves from chunk to chunk itself
     instead of returning to GameLoop on each transfer between chunks.
  TGP (tools/m2tgprecomp/main.cpp)
  3. for calls without an instruction budget (the game's): the instruction
     count once per basic block and no budget test at each branch. Results
     and counts stay those of the default output (the TGP has no interrupts).
--tgp-profile: TGP code with block counters (m2tgprecomp --profile) and the
runtime's TGP profiling (CMake M2_TGP_PROFILE): m2run prints where the TGP's
time goes (src/runtime/tgp/tgp_profile.h). Results unchanged; run again without
it to go back. --i960-profile: the same for the i960 (m2recomp --profile,
CMake M2_I960_PROFILE, src/runtime/cpu/i960_profile.h).
--inline-bus: the i960 code with the set's board bus inlined (m2recomp
--board, CMake M2_INLINE_BUS; build_vita.py --inline-bus on the Vita, off by
default there too: no measurable gain): the same results, for checking it
on the PC. m2native refuses that code.
The lockstep and trace comparisons need the default output. Off by default;
run again without it to go back.
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_jobs import build_jobs  # noqa: E402
import games  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# Exit status for "the ROM set was rejected", so setup.py can tell it from a
# failure to recompile or build (which exit 1, with the tool's own errors).
ROM_REJECTED_EXIT = 3

# Per set: the archive names under roms/ and the m2recomp options
# (scripts/games.py).
SETS = games.SETS


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=ROOT, **kw)


def tool(build, config, name):
    """Path of a built tool: single-config generators put it in the build
    directory, Visual Studio in build/<config>/."""
    exe = name + (".exe" if os.name == "nt" else "")
    for p in (os.path.join(build, exe), os.path.join(build, config, exe)):
        if os.path.exists(p):
            return p
    sys.exit(f"recompile: {name} was not built in {build}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", choices=sorted(games.GAMES), help=f"the game (default {games.DEFAULT})")
    ap.add_argument("--set", choices=sorted(SETS), help="another set of the game (default: its main set)")
    ap.add_argument("--config", default="Release")
    ap.add_argument("--fast_inaccuracy", action="store_true",
                    help="recompiler speed options (i960 block bookkeeping and direct chunk chaining, TGP block counting): faster (PS Vita), "
                         "not interrupt-exact against MAME")
    ap.add_argument("--tgp-profile", action="store_true",
                    help="TGP profiling: block counters in the TGP code, time and work report in m2run")
    ap.add_argument("--i960-profile", action="store_true",
                    help="i960 profiling: block counters in the i960 code, time, work and bus report in m2run")
    ap.add_argument("--inline-bus", action="store_true",
                    help="the board's bus inlined in the i960 code (m2recomp --board, CMake M2_INLINE_BUS)")
    ap.add_argument("--recomp-only", action="store_true",
                    help="stop after the recompilation: build only the tools, not the desktop game")
    args = ap.parse_args()
    try:
        args.game, args.set = games.resolve(args.game, args.set)
    except ValueError as error:
        ap.error(str(error))
    build = os.path.join(ROOT, games.pc_dir(args.set))
    cache = os.path.join(ROOT, games.rom_cache(args.set))
    gen_root = os.path.join(ROOT, games.gen_root(args.set))
    print(f"recompile: {games.title(args.set)} -> {games.game_dir(args.game)}/", flush=True)
    roms = [os.path.join(ROOT, "roms", n + "." + ext) for n in SETS[args.set]["roms"] for ext in ("zip", "7z")]
    roms = [r for r in roms if os.path.exists(r)]

    build_cmd = ["cmake", "--build", build, "--config", args.config]
    # the desktop build is for one ROM set (CMake's M2_ROMSET); a new one gets
    # --config as its build type, as setup.py does (none is -O0, where SoftFloat's
    # inline helpers are not emitted and test_fp does not link)
    fresh = not os.path.exists(os.path.join(build, "CMakeCache.txt"))
    run(["cmake", "-S", ".", "-B", build, "-DM2_ROMSET=" + args.set, "-DM2_GAME=" + args.game, "-DM2_GEN_ROOT=" + gen_root,
         "-DM2_TGP_PROFILE=" + ("ON" if args.tgp_profile else "OFF"),
         "-DM2_I960_PROFILE=" + ("ON" if args.i960_profile else "OFF"),
         "-DM2_INLINE_BUS=" + ("ON" if args.inline_bus else "OFF")] +
        (["-DCMAKE_BUILD_TYPE=" + args.config] if fresh else []))
    if not all(os.path.exists(os.path.join(cache, f))
               for f in ("tgp_program.bin", "dsp_program.bin", "sound_program.bin", "pcm1.bin", "samples.bin")):
        if not roms:
            sys.exit(f"recompile: put your ROM set at roms/{args.set}.zip (or .7z) first")
        run(build_cmd + ["--target", "m2import"])
        if subprocess.run([tool(build, args.config, "m2import"), roms[0], cache], cwd=ROOT).returncode:
            sys.exit(ROM_REJECTED_EXIT)  # m2import printed which file is missing or wrong
    run(build_cmd + ["--target", "m2recomp", "m2tgprecomp", "m2sndrecomp", "m2dsprecomp", "m2sharcrecomp"])

    gen = os.path.join(gen_root, args.set)
    if os.path.isdir(gen):
        for f in os.listdir(gen):
            os.remove(os.path.join(gen, f))
    os.makedirs(gen, exist_ok=True)
    recomp = [tool(build, args.config, "m2recomp"), os.path.join(cache, "program.bin"), gen,
              "--seeds", os.path.join("seeds", args.set + ".txt")] + SETS[args.set]["recomp"]
    hooks = os.path.join("seeds", args.set + "_hooks.txt")
    if os.path.exists(os.path.join(ROOT, hooks)):
        recomp += ["--hooks", hooks]
    inline = os.path.join("seeds", args.set + "_inline.txt")  # helpers whose arguments follow their balx
    if os.path.exists(os.path.join(ROOT, inline)):
        recomp += ["--inline", inline]
    if args.fast_inaccuracy:
        recomp += ["--fast_inaccuracy"]
    if args.i960_profile:
        recomp += ["--profile"]
    if args.inline_bus:
        recomp += ["--board", SETS[args.set]["board"]]
    run(recomp)
    tgp = os.path.join(gen_root, args.set + "_tgp")
    os.makedirs(tgp, exist_ok=True)
    run([tool(build, args.config, "m2tgprecomp"), os.path.join(cache, "tgp_program.bin"),
         os.path.join(tgp, "tgp_gen.cpp")] + (["--fast_inaccuracy"] if args.fast_inaccuracy else []) +
        (["--profile"] if args.tgp_profile else []))

    snd = os.path.join(gen_root, args.set + "_snd")
    os.makedirs(snd, exist_ok=True)
    snd_seeds = os.path.join("seeds", args.set + "_snd.txt")  # the sound 68000's indirect jump targets
    run([tool(build, args.config, "m2sndrecomp"), os.path.join(cache, "sound_program.bin"),
         os.path.join(snd, "snd_gen.cpp")] + SETS[args.set]["sndrecomp"] +
        (["--seeds", snd_seeds] if os.path.exists(os.path.join(ROOT, snd_seeds)) else []))

    # The SCSP DSP program (Game::dsp; an empty file: none, the interpreter runs).
    dsp = os.path.join(gen_root, args.set + "_dsp")
    os.makedirs(dsp, exist_ok=True)
    run([tool(build, args.config, "m2dsprecomp"), os.path.join(cache, "dsp_program.bin"), os.path.join(dsp, "dsp_gen.cpp")])

    # Model 2B: the SHARC program (Game::sharc; m2import writes it only for such a set).
    sharc_program = os.path.join(cache, "sharc_program.bin")
    if os.path.exists(sharc_program):
        sharc = os.path.join(gen_root, args.set + "_sharc")
        os.makedirs(sharc, exist_ok=True)
        run([tool(build, args.config, "m2sharcrecomp"), sharc_program, os.path.join(sharc, "sharc_gen.cpp"),
             "--ops", os.path.join(ROOT, "src/runtime/sharc/sharcops_table.inc"),
             "--sharcops", os.path.join(ROOT, "src/runtime/sharc/sharcops.hxx"),
             "--compute", os.path.join(ROOT, "src/runtime/sharc/compute.hxx")])

    if args.recomp_only:
        print(f"recompile: recompiled into {games.gen_root(args.set)}/ (--recomp-only: the desktop game is not built)")
        return
    run(["cmake", "-S", ".", "-B", build])  # picks up the generated sources
    run(build_cmd + ["--parallel", str(build_jobs())])  # the game (daytona, m2run) and the check tools
    print(f"recompile: built in {games.pc_dir(args.set)}/ (the game: {games.pc_dir(args.set)}/{args.game})")


if __name__ == "__main__":
    main()
