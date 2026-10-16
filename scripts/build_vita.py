#!/usr/bin/env python3
"""Cross-compile a game's Vita package from its host recompilation.

  build_vita.py [--game GAME] [--set SET] [--gpu-gl] [--fast-inaccuracy] [--diagnostics | --release] ...

First recompile the game on the host with your own ROM set (python3
scripts/recompile.py --game GAME, once, in any mode; --recomp-only is
enough). Everything stays in the game's folder (scripts/games.py):
build_<game>/vita/<game>.vpk, or build_<game>/vita_diag/<game>_diag.vpk with
--diagnostics. --fast-inaccuracy and --fast-gen make the Vita's own game code
here from the host tools and ROM cache, whatever mode the host code was
generated in (see vita_code). The package's images: platform/vita/logo_livearea/<game>/.
This command never executes cross-built importers, copies ROMs, or packages
ROM data. --compile-check builds objects without ROMs and does not make a VPK.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import games  # noqa: E402
from games import SETS  # noqa: E402

# The generated sources a set's build needs, under gen/ ({set}: the set).
REQUIRED = ("{set}/gen_table.cpp", "{set}_tgp/tgp_gen.cpp", "{set}_snd/snd_gen.cpp")
# The Dreamcast's pass over the generated i960 code, used unchanged by the Vita build
# (its runtime support is M2_FAST_GEN, platform/vita/CMakeLists.txt).
FAST_GEN = Path("platform/dreamcast/scripts/fast_gen.py")


def run(command, env):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)


SCE_DATA_ROOM = 8192  # bytes vita-elf-create needs after the code segment (it asked 4,308 for VF2)


def code_segment_gap(sdk, elf):
    """Bytes between the end of the ELF's code segment and the start of its data segment."""
    out = subprocess.run([str(sdk / "bin/arm-vita-eabi-readelf"), "-lW", str(elf)],
                         capture_output=True, text=True, check=True).stdout
    loads = [line.split() for line in out.splitlines() if line.strip().startswith("LOAD")]
    return int(loads[1][2], 16) - (int(loads[0][2], 16) + int(loads[0][5], 16))


def host_tool(host, name):
    """A tool of the host build: in the build directory, or build/Release/ (Visual Studio)."""
    exe = name + (".exe" if os.name == "nt" else "")
    for path in (host / exe, host / "Release" / exe):
        if path.is_file():
            return path
    return None


def write_if_changed(path, data):
    """Rewritten only when it changes: make recompiles only those files."""
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def sync(src, dst):
    """dst becomes a copy of src's files; unchanged files keep their date and
    files gone from src are removed (the build compiles every *.cpp there)."""
    dst.mkdir(parents=True, exist_ok=True)
    names = set()
    for f in sorted(src.iterdir()):
        if f.is_file():
            names.add(f.name)
            write_if_changed(dst / f.name, f.read_bytes())
    for stale in dst.iterdir():
        if stale.is_file() and stale.name not in names:
            stale.unlink()


def load_fast_gen():
    import importlib.util
    spec = importlib.util.spec_from_file_location("vita_fast_gen", ROOT / FAST_GEN)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def direct_ram(folder):
    """--fast-inaccuracy: fast_gen.py's direct work-RAM accesses only, in place. A load or
    store at a fixed, aligned address in 0x00500000-0x005fffff (plain RAM: no
    device, nothing watches its writes) becomes gen::wram_* instead of a call
    through the bus; the frame-wait byte (0x00500000) keeps its call. The lockstep
    code (--fast_inaccuracy's checks per block, chunk chaining) is not touched."""
    direct_wram = load_fast_gen().direct_wram
    stats = {"wram": 0}
    for f in sorted(folder.glob("chunk_*.cpp")):
        text = f.read_text()
        f.write_text("\n".join(direct_wram(line, stats) for line in text.split("\n")), newline="\n")
    print(f"build_vita: {stats['wram']} work-RAM accesses direct", flush=True)


def vita_code(host, gen, build, mode, game, tgp_profile=False, i960_profile=False, inline_bus=False):  # game: the set
    """The game code of the Vita build, made here from the host build's tools and ROM
    cache (scripts/recompile.py, run once in any mode): the host's own generated code
    and the desktop build are left as they are. Into BUILD_DIR/gen_vita:

      --fast-inaccuracy  i960 m2recomp --fast_inaccuracy (lockstep checks once per
                         block, direct chunk chaining: interrupts a few instructions late)
                         plus direct work-RAM accesses (direct_ram)
      --fast-gen         i960 default output rewritten by fast_gen.py (exact; slower
                         on the Vita)

    TGP always m2tgprecomp --fast_inaccuracy: the game's unbudgeted calls counted once
    per basic block, without the budget test; exact (the TGP has no interrupts). The
    68000 code as the host generated it. tgp_profile: the TGP code with m2tgprecomp's
    block counters (--profile); i960_profile: the i960 code with m2recomp's (--profile).
    inline_bus (--inline-bus; off by default, its gain on the Vita too small to
    measure): the i960 code with the set's board bus inlined (m2recomp --board,
    runtime M2_INLINE_BUS)."""
    cache = ROOT / games.rom_cache(game)
    i960_tool, tgp_tool = host_tool(host, "m2recomp"), host_tool(host, "m2tgprecomp")
    for need in (i960_tool, tgp_tool, cache / "program.bin", cache / "tgp_program.bin"):
        if not need or not Path(need).is_file():
            raise RuntimeError(f"missing {need or 'm2recomp/m2tgprecomp in ' + str(host)}: "
                               "run python3 scripts/recompile.py once first (it builds the tools and the ROM cache)")
    tmp = build / "gen_tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    (tmp / "i960").mkdir(parents=True)
    (tmp / "tgp").mkdir(parents=True)
    i960 = [i960_tool, cache / "program.bin", tmp / "i960", "--seeds", f"seeds/{game}.txt", *SETS[game]["recomp"]]
    if (ROOT / f"seeds/{game}_hooks.txt").is_file():
        i960 += ["--hooks", f"seeds/{game}_hooks.txt"]
    if mode != "fast_gen":
        i960.append("--fast_inaccuracy")
    if i960_profile:
        i960.append("--profile")
    if inline_bus:
        i960 += ["--board", SETS[game]["board"]]
    run(i960, os.environ)
    i960_out = tmp / "i960"
    if mode == "fast_inaccuracy":
        direct_ram(i960_out)
    elif mode == "fast_gen":
        run([sys.executable, ROOT / FAST_GEN, i960_out, tmp / "i960_fast_gen"], os.environ)
        i960_out = tmp / "i960_fast_gen"
    run([tgp_tool, cache / "tgp_program.bin", tmp / "tgp/tgp_gen.cpp", "--fast_inaccuracy",
         *(["--profile"] if tgp_profile else [])], os.environ)
    out = build / "gen_vita"
    sync(i960_out, out / game)
    sync(tmp / "tgp", out / f"{game}_tgp")
    sync(gen / f"{game}_snd", out / f"{game}_snd")
    if (gen / f"{game}_dsp").is_dir():  # the recompiled SCSP DSP program (optional)
        sync(gen / f"{game}_dsp", out / f"{game}_dsp")
    shutil.rmtree(tmp, ignore_errors=True)
    for old in ("gen_fast", "gen_ram"):  # earlier versions of this script
        shutil.rmtree(build / old, ignore_errors=True)
    return out, {"fast_inaccuracy": "i960 --fast_inaccuracy + direct work-RAM accesses",
                 "fast_gen": "i960 fast_gen.py rewrite"}[mode] + \
        (", bus inlined (--board " + SETS[game]["board"] + ")" if inline_bus else "") + ", TGP fast mode"


LIVEAREA_FILES = ("icon0.png", "bg.png", "startup.png", "template.xml")


def livearea_dir(game):
    """The game's own icon and LiveArea images (platform/vita/logo_livearea/<game>/)."""
    return ROOT / "platform/vita/logo_livearea" / game


def livearea_summary(game):
    """Images from the game's livearea_dir packaged by platform/vita/livearea.cmake."""
    folder = livearea_dir(game)
    found = [name for name in LIVEAREA_FILES if (folder / name).is_file()]
    return ", ".join(found) if found else "none (Vita defaults)"


def vitagl_summary(prefix):
    """Which vitaGL a --gpu-gl build links (platform/vita/CMakeLists.txt makes the same choice)."""
    try:
        stamp = json.loads((prefix / "build.json").read_text())
    except (OSError, ValueError):
        stamp = None
    if not stamp or not (prefix / "lib/libvitaGL.a").is_file():
        return "the VitaSDK's (not pinned: run python3 scripts/setup_vitagl.py)"
    flags = " ".join(f"{k}={v}" for k, v in stamp.get("flags", {}).items())
    return f"pinned {stamp['pins']['vitaGL'][:10]}, {stamp.get('profile')}: {flags or 'default flags'}"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game", choices=sorted(games.GAMES),
                    help=f"the game (scripts/games.py; default {games.DEFAULT}); each game is its own app, with its own "
                         "title ID (Daytona DAYT00093, Sega Rally SRAL00094)")
    ap.add_argument("--set", choices=sorted(SETS), help="another set of the game (default: its main set)")
    ap.add_argument("--title-id", default="", help="override the package's title ID (4 capital letters, 5 digits)")
    ap.add_argument("--revision-a-self", type=Path,
                    help="bundle the companion set's built eboot.bin as <companion>.self (daytona93: Revision A's, daytona.self)")
    ap.add_argument("--build-dir", type=Path, help="default: build_<game>/vita, or build_<game>/vita_diag with --diagnostics")
    ap.add_argument("--vitasdk", type=Path, default=os.environ.get("VITASDK"))
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4))
    ap.add_argument("--compile-check", action="store_true")
    ap.add_argument("--reference-renderer", action="store_true", help="disable OPT03 renderer changes for comparison")
    ap.add_argument("--inline-bus", action="store_true",
                    help="--fast-inaccuracy/--fast-gen: the i960 code with the board's bus inlined (m2recomp --board, "
                         "M2_INLINE_BUS; off by default: no measurable gain on the Vita), for comparison")
    ap.add_argument("--no-fast-paths", action="store_true",
                    help="build the shared runtime without its fast paths (src/runtime/fast_paths.h), for comparison")
    gpu = ap.add_mutually_exclusive_group()
    gpu.add_argument("--gpu-fast", action="store_true", help="build experimental vita2d/GXM 3D renderer (requires vdpm libvita2d)")
    gpu.add_argument("--gpu-gl", action="store_true", help="build experimental vitaGL 3D renderer, Model 2 draw priority "
                     "(vitaGL pinned by scripts/setup_vitagl.py, else the VitaSDK's; libshacccg.suprx on the Vita)")
    ap.add_argument("--vitagl-dir", type=Path, default=Path("extern/vitagl/install"),
                    help="--gpu-gl: vitaGL built by scripts/setup_vitagl.py (default extern/vitagl/install)")
    ap.add_argument("--free-core", action="store_true",
                    help="GPU builds: leave the main, geometry, sound and 2D threads unpinned (default: one core each); "
                         "the 4TH CORE option can then take any of them")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--diagnostics", action="store_true", help="enable GXM startup/performance file logging (faults always recorded)")
    mode.add_argument("--release", action="store_true", help="fastest build: no diagnostic logging + link-time optimization (LTO; longer link, more host memory)")
    ap.add_argument("--tgp-profile", action="store_true",
                    help="TGP profiling in perf.log (time, work, unit costs, hottest blocks; src/runtime/tgp_profile.h); "
                         "implies --diagnostics, needs --fast-inaccuracy or --fast-gen (the TGP code is made here)")
    ap.add_argument("--i960-profile", action="store_true",
                    help="i960 profiling in perf.log (time, work, bus calls by region, hottest blocks; "
                         "src/runtime/i960_profile.h); implies --diagnostics, needs --fast-inaccuracy (the i960 code is made here)")
    code = ap.add_mutually_exclusive_group()
    code.add_argument("--fast-inaccuracy", dest="code", action="store_const", const="fast_inaccuracy",
                      help="Vita game code made here (BUILD_DIR/gen_vita): i960 m2recomp --fast_inaccuracy with direct "
                           "work-RAM accesses (fast_gen.py's), TGP fast mode")
    code.add_argument("--fast-gen", dest="code", action="store_const", const="fast_gen",
                      help="i960 default output rewritten by platform/dreamcast/scripts/fast_gen.py (exact, slower on the Vita), "
                           "TGP fast mode. Without either: the host-generated code as it is")
    args = ap.parse_args(argv)
    try:
        args.game, args.set = games.resolve(args.game, args.set)
    except ValueError as error:
        ap.error(str(error))
    game = dict(games.GAMES[args.game], **SETS[args.set])
    if args.tgp_profile or args.i960_profile:
        args.diagnostics = True  # checked against --release below
    if args.build_dir is None:  # diagnostics: its own build directory, the release one is left as it is
        args.build_dir = Path(games.vita_dir(args.game, args.diagnostics))
    if args.title_id and not re.fullmatch(r"[A-Z]{4}[0-9]{5}", args.title_id):
        ap.error("--title-id must be 4 capital letters and 5 digits")
    gpu_build = args.gpu_fast or args.gpu_gl
    if args.set != "daytona93" and not gpu_build:  # the old CPU frontend (main.cpp) only knows daytona93
        ap.error(f"{args.set} requires --gpu-fast or --gpu-gl")
    if args.revision_a_self and (args.set != games.main_set(args.game) or not game["companion"] or not gpu_build):
        ap.error(f"--revision-a-self requires a game with a companion set, its main set and --gpu-fast or --gpu-gl")
    if args.code and args.code not in game["vita_fast"]:
        # Only the modes checked on the set (scripts/games.py vita_fast).
        flag = "--" + args.code.replace("_", "-")
        ap.error(f"{flag} has not been checked on {args.set}; build it without it (the host-generated code)")
    for flag, on in (("--tgp-profile", args.tgp_profile), ("--i960-profile", args.i960_profile)):
        if not on:
            continue
        if args.release:
            ap.error(f"{flag} writes perf.log: it needs --diagnostics, not --release")
        if not args.code:
            ap.error(f"{flag} makes the game code here: add --fast-inaccuracy" + (" or --fast-gen" if flag == "--tgp-profile" else ""))
        if not gpu_build:
            ap.error(f"{flag} reports in perf.log: --gpu-fast or --gpu-gl")
    if args.i960_profile and args.code == "fast_gen":
        ap.error("--i960-profile needs --fast-inaccuracy (fast_gen.py does not know the block counters)")
    if args.free_core and not gpu_build:
        ap.error("--free-core applies to the GPU builds (--gpu-fast or --gpu-gl)")
    if args.jobs < 1:
        ap.error("--jobs must be positive")
    if args.vitasdk is None:
        ap.error("set VITASDK or pass --vitasdk /path/to/vitasdk")
    sdk = args.vitasdk.expanduser().resolve()
    toolchain = sdk / "share/vita.toolchain.cmake"
    if not toolchain.is_file():
        ap.error(f"missing VitaSDK toolchain: {toolchain}")
    host = (ROOT / games.pc_dir(args.set)).resolve()
    build = (ROOT / args.build_dir).resolve()
    gen = (ROOT / games.gen_root(args.set)).resolve()
    if build == host or host in build.parents or build == ROOT or build == gen or gen in build.parents:
        ap.error("use a separate cross-build directory, not the host build or its generated sources")
    if not args.compile_check:
        required = tuple(name.format(set=args.set) for name in REQUIRED)
        missing = [str(gen / name) for name in required if not (gen / name).is_file()]
        if missing:
            ap.error("missing host-generated code:\n" + "\n".join(missing) +
                     f"\nRun python3 scripts/recompile.py --game {args.game}" +
                     (f" --set {args.set}" if args.set != games.main_set(args.game) else "") +
                     " with the host compiler first.")
    # Every hook of the set's seeds/<set>_hooks.txt must be in its generated code.
    hooks = ROOT / f"seeds/{args.set}_hooks.txt"
    if gpu_build and not args.compile_check and hooks.is_file():
        names = {line.split()[1] for line in hooks.read_text().splitlines()
                 if line.strip() and not line.lstrip().startswith("#") and len(line.split()) > 1}
        sources = [source.read_text() for source in (gen / args.set).glob("chunk_*.cpp")]
        missing = sorted(n for n in names if not any(f"rt::hook_{n}(" in text for text in sources))
        if missing:
            ap.error(f"generated game code lacks the hooks {', '.join(missing)}; regenerate with scripts/recompile.py")
    # A diagnostics build is its own app (title ID with a 9 as its first digit,
    # "(diagnostics)" in its name), installed next to the release, not over it.
    title_id = args.title_id or (game["vita_title_id"][:4] + "9" + game["vita_title_id"][5:] if args.diagnostics
                                 else game["vita_title_id"])
    app_name = game["vita_name"] + (" (diagnostics)" if args.diagnostics and not args.title_id else "")
    package_name = games.vpk_name(args.game, args.diagnostics)
    code = "host-generated (scripts/recompile.py output as is)"
    if args.code and not args.compile_check:
        gen, code = vita_code(host, gen, build, args.code, args.set, args.tgp_profile, args.i960_profile,
                              args.inline_bus)
        if args.tgp_profile:
            code += ", TGP block counters (--tgp-profile)"
        if args.i960_profile:
            code += ", i960 block counters (--i960-profile)"
    vitagl_dir = (ROOT / args.vitagl_dir).resolve()
    vitagl = vitagl_summary(vitagl_dir) if args.gpu_gl else "-"
    env = os.environ.copy()
    env["VITASDK"] = str(sdk)
    env["PATH"] = str(sdk / "bin") + os.pathsep + env.get("PATH", "")
    run(["cmake", "-S", ROOT / "platform/vita", "-B", build,
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
         f"-DDAYTONA_GEN_ROOT={gen}",
         f"-DDAYTONA_VITA_ROMSET={args.set}",
         f"-DDAYTONA_VITA_TITLEID={title_id}",
         f"-DDAYTONA_VITA_GAME_NAME={app_name}",
         f"-DDAYTONA_VITA_VPK={package_name}",
         f"-DDAYTONA_VITA_LIVEAREA_DIR={livearea_dir(args.game)}",
         f"-DDAYTONA_VITA_COMPANION={game['companion'] or ''}",
         f"-DDAYTONA_VITA_REV_A_SELF={(ROOT / args.revision_a_self).resolve() if args.revision_a_self else ''}",
         f"-DDAYTONA_VITA_RENDER_OPT={'OFF' if args.reference_renderer else 'ON'}",
         f"-DDAYTONA_VITA_FAST_PATHS={'OFF' if args.no_fast_paths else 'ON'}",
         f"-DDAYTONA_VITA_INLINE_BUS={'ON' if args.inline_bus else 'OFF'}",
         f"-DDAYTONA_VITA_DIAGNOSTICS={'ON' if args.diagnostics else 'OFF'}",
         f"-DDAYTONA_VITA_TGP_PROFILE={'ON' if args.tgp_profile else 'OFF'}",
         f"-DDAYTONA_VITA_I960_PROFILE={'ON' if args.i960_profile else 'OFF'}",
         f"-DDAYTONA_VITA_LTO={'ON' if args.release else 'OFF'}",
         f"-DDAYTONA_VITA_GPU_FAST={'ON' if args.gpu_fast else 'OFF'}",
         f"-DDAYTONA_VITA_GPU_GL={'ON' if args.gpu_gl else 'OFF'}",
         f"-DDAYTONA_VITA_FREE_CORES={'ON' if args.free_core else 'OFF'}",
         f"-DDAYTONA_VITA_COMPILE_CHECK={'ON' if args.compile_check else 'OFF'}",
         "-DDAYTONA_VITA_TEXT_PAD=0",
         *([f"-DDAYTONA_VITAGL_DIR={vitagl_dir}"] if args.gpu_gl else [])], env)
    try:
        run(["cmake", "--build", build, "--parallel", args.jobs], env)
    except subprocess.CalledProcessError:
        # vita-elf-create fails when the code segment ends too close to the data segment:
        # pad the code so the data segment moves to the next 64 KB step, and relink.
        elf = build / "daytona_vita"
        gap = code_segment_gap(sdk, elf) if not args.compile_check and elf.is_file() else SCE_DATA_ROOM
        if gap >= SCE_DATA_ROOM:
            raise
        pad = gap + SCE_DATA_ROOM
        print(f"build_vita: {gap} bytes left after the code segment for vita-elf-create; relinking with {pad} bytes "
              "of padding (DAYTONA_VITA_TEXT_PAD)", flush=True)
        run(["cmake", "-S", ROOT / "platform/vita", "-B", build, f"-DDAYTONA_VITA_TEXT_PAD={pad}"], env)
        run(["cmake", "--build", build, "--parallel", args.jobs], env)
    if args.compile_check:
        print("Compile check complete. No linked game or VPK was built." + (f"\nvitaGL: {vitagl}" if args.gpu_gl else ""))
    else:
        package = build / package_name
        if not package.is_file():
            raise RuntimeError(f"build completed without the expected package: {package}")
        mode = "GPU GL (vitaGL)" if args.gpu_gl else ("GPU FAST" if args.gpu_fast else "CPU EXACT")
        cores = ("free (--free-core)" if args.free_core else "pinned: main 0, geometry 1, sound 2") if gpu_build else "-"
        kind = "release (LTO, no logs)" if args.release else ("diagnostics (logs)" if args.diagnostics else "normal (no logs)")
        paths = "off (--no-fast-paths)" if args.no_fast_paths else "on (M2_FAST_BUS/TGP/CPU/GEO)"
        print(f"VPK: {package}\nRenderer build: {mode}\nvitaGL: {vitagl}\nCores: {cores}\nBuild: {kind}\nGame code: {code}\n"
              f"Runtime fast paths: {paths}\nLiveArea images: {livearea_summary(args.game)}\nTitle ID: {title_id}\n"
              f"ROM location on Vita: ux0:data/{args.set}/{args.set}.zip")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"build_vita: {error}", file=sys.stderr)
        sys.exit(1)
