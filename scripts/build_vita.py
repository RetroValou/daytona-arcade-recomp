#!/usr/bin/env python3
"""Cross-compile the Vita frontend using already generated host game code.

First run the existing setup/recompile pipeline with your own daytona93 ROMs.
This command never executes cross-built importers, copies ROMs, or packages
ROM data. --compile-check builds objects without ROMs and does not make a VPK.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ("daytona93/gen_table.cpp", "daytona93_tgp/tgp_gen.cpp", "daytona93_snd/snd_gen.cpp")


def run(command, env):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host-build-dir", type=Path, default=Path("build"))
    ap.add_argument("--build-dir", type=Path, default=Path("build/vita"))
    ap.add_argument("--vitasdk", type=Path, default=os.environ.get("VITASDK"))
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 4))
    ap.add_argument("--compile-check", action="store_true")
    ap.add_argument("--reference-renderer", action="store_true", help="disable OPT03 renderer changes for comparison")
    gpu = ap.add_mutually_exclusive_group()
    gpu.add_argument("--gpu-fast", action="store_true", help="build experimental vita2d/GXM 3D renderer (requires vdpm libvita2d)")
    gpu.add_argument("--gpu-gl", action="store_true", help="build experimental vitaGL 3D renderer with a hardware depth buffer (requires vitaGL + vitaShaRK; libshacccg.suprx on the Vita)")
    ap.add_argument("--diagnostics", action="store_true", help="enable GXM startup/performance file logging (faults always recorded)")
    gl = ap.add_argument_group("vitaGL renderer options (only with --gpu-gl)")
    gl.add_argument("--gl-depth", type=int, choices=(0, 1), default=1,
                    help="1: hardware depth test (default); 0: no depth, painter order")
    gl.add_argument("--gl-painter", action="store_true",
                    help="draw in CPU painter order (kept together with the depth test unless --gl-depth 0)")
    gl.add_argument("--gl-batch", type=int, choices=(0, 1), default=1,
                    help="regroup polygons by material (ignored in painter order)")
    gl.add_argument("--gl-full-range", type=int, choices=(0, 1), default=1)
    gl.add_argument("--gl-binary-alpha", type=int, choices=(0, 1), default=1)
    
    args = ap.parse_args(argv)
    if args.jobs < 1:
        ap.error("--jobs must be positive")
    if args.vitasdk is None:
        ap.error("set VITASDK or pass --vitasdk /path/to/vitasdk")
    sdk = args.vitasdk.expanduser().resolve()
    toolchain = sdk / "share/vita.toolchain.cmake"
    if not toolchain.is_file():
        ap.error(f"missing VitaSDK toolchain: {toolchain}")
    host = (ROOT / args.host_build_dir).resolve()
    build = (ROOT / args.build_dir).resolve()
    gen = host / "gen"
    if build == host or build == ROOT or build == gen or gen in build.parents:
        ap.error("use a separate cross-build directory, not the host build or its generated sources")
    if not args.compile_check:
        missing = [str(gen / name) for name in REQUIRED if not (gen / name).is_file()]
        if missing:
            ap.error("missing host-generated code:\n" + "\n".join(missing) +
                     "\nRun python3 scripts/recompile.py with the host compiler first.")
    env = os.environ.copy()
    env["VITASDK"] = str(sdk)
    env["PATH"] = str(sdk / "bin") + os.pathsep + env.get("PATH", "")
    run(["cmake", "-S", ROOT / "platform/vita", "-B", build,
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
         f"-DDAYTONA_GEN_ROOT={gen}",
         f"-DDAYTONA_VITA_RENDER_OPT={'OFF' if args.reference_renderer else 'ON'}",
         f"-DDAYTONA_VITA_DIAGNOSTICS={'ON' if args.diagnostics else 'OFF'}",
         f"-DDAYTONA_VITA_GPU_FAST={'ON' if args.gpu_fast else 'OFF'}",
         f"-DDAYTONA_VITA_GPU_GL={'ON' if args.gpu_gl else 'OFF'}",
         f"-DDAYTONA_VITA_GL_DEPTH={args.gl_depth}",
         f"-DDAYTONA_VITA_GL_PAINTER={1 if args.gl_painter else 0}",
         f"-DDAYTONA_VITA_GL_BATCH={args.gl_batch}",
         f"-DDAYTONA_VITA_GL_FULL_RANGE={args.gl_full_range}",
         f"-DDAYTONA_VITA_GL_BINARY_ALPHA={args.gl_binary_alpha}",
         f"-DDAYTONA_VITA_COMPILE_CHECK={'ON' if args.compile_check else 'OFF'}"], env)
    run(["cmake", "--build", build, "--parallel", args.jobs], env)
    if args.compile_check:
        print("Compile check complete. No linked game or VPK was built.")
    else:
        package = build / "daytona_vita.vpk"
        if not package.is_file():
            raise RuntimeError(f"build completed without the expected package: {package}")
        mode = "GPU GL (vitaGL)" if args.gpu_gl else ("GPU FAST" if args.gpu_fast else "CPU EXACT")
        print(f"VPK: {package}\nRenderer build: {mode}\nROM location on Vita: ux0:data/daytona93/daytona93.zip")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"build_vita: {error}", file=sys.stderr)
        sys.exit(1)
