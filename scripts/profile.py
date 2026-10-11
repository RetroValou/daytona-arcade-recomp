#!/usr/bin/env python3
"""Block profile of the recompiled game: run m2run --profile on every scenario
in scripts/inputs and sum the results into profiles/<set>.txt, which
scripts/recompile.py --profile (and scripts/build_vita.py --profile) give to
m2recomp --profile: the very hot code grouped into its own chunks.

  profile.py [--set daytona93|daytona] [--build-dir DIR] [--config Release]
             [--inputs FILE ...] [--with-tests] [--jobs N] [--out FILE]

Needs the game recompiled first (scripts/recompile.py; build/ for daytona93,
build-daytona/ for daytona). Each scenario runs for its own "frames N" line.
The test-menu scenarios (scripts/inputs/test*.txt) are left out unless
--with-tests: the game in play is what the layout is for.
The profile holds block addresses and counts (our own measurement, like the
seeds), no game data: it is committed. Block addresses come from the
recompiler's seeds; after a seed change, profile again (addresses the
recompiler no longer has are ignored, not an error).
"""

import argparse
import concurrent.futures
import glob
import os
import subprocess
import sys
import tempfile
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tool(build, config, name):
    exe = name + (".exe" if os.name == "nt" else "")
    for p in (os.path.join(build, exe), os.path.join(build, config, exe)):
        if os.path.exists(p):
            return p
    sys.exit(f"profile: {name} was not built in {build} (run scripts/recompile.py first)")


def frames_of(script):
    with open(script) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[0] == "frames":
                return int(parts[1])
    sys.exit(f"profile: {script} has no 'frames N' line")


def run_one(m2run, images, script, out):
    frames = frames_of(script)
    r = subprocess.run([m2run, images, str(frames), "--inputs", script, "--profile", out],
                       cwd=ROOT, capture_output=True, text=True)
    summary = next((l for l in r.stdout.splitlines() if l.startswith("m2run: ") and "frames," in l), r.stdout.strip())
    return r.returncode, summary, r.stderr


def merge(paths):
    blocks, lengths, edges = defaultdict(int), {}, defaultdict(int)
    for path in paths:
        with open(path) as f:
            for line in f:
                p = line.split()
                if not p or p[0].startswith("#"):
                    continue
                if p[0] == "block":
                    blocks[p[1]] += int(p[3])
                    lengths[p[1]] = p[2]
                elif p[0] == "edge":
                    edges[(p[1], p[2])] += int(p[3])
    return blocks, lengths, edges


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", default="daytona93", choices=["daytona93", "daytona"])
    ap.add_argument("--build-dir", help="default: build (daytona93), build-daytona (daytona)")
    ap.add_argument("--config", default="Release")
    ap.add_argument("--inputs", nargs="+", help="scenarios (default: every scripts/inputs/*.txt but test*.txt)")
    ap.add_argument("--with-tests", action="store_true", help="also the test-menu scenarios (scripts/inputs/test*.txt)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--out", help="default: profiles/<set>.txt")
    args = ap.parse_args()
    build = os.path.join(ROOT, args.build_dir or ("build" if args.set == "daytona93" else "build-daytona"))
    images = os.path.join(build, "rom_cache", args.set)
    if not os.path.isdir(images):
        sys.exit(f"profile: no {images} (run scripts/recompile.py --set {args.set} first)")
    m2run = tool(build, args.config, "m2run")
    scripts = args.inputs or sorted(s for s in glob.glob(os.path.join(ROOT, "scripts", "inputs", "*.txt"))
                                    if args.with_tests or not os.path.basename(s).startswith("test"))
    out = args.out or os.path.join(ROOT, "profiles", args.set + ".txt")

    with tempfile.TemporaryDirectory() as tmp:
        parts = {s: os.path.join(tmp, os.path.basename(s) + ".prof") for s in scripts}
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            jobs = {pool.submit(run_one, m2run, images, s, parts[s]): s for s in scripts}
            for job in concurrent.futures.as_completed(jobs):
                s = jobs[job]
                code, summary, err = job.result()
                name = os.path.basename(s)
                if code or not os.path.exists(parts[s]):
                    failed.append(name)
                    print(f"profile: {name}: FAILED\n{summary}\n{err}", flush=True)
                else:
                    print(f"profile: {name}: {summary.removeprefix('m2run: ')}", flush=True)
        if failed:
            sys.exit(f"profile: {len(failed)} scenario(s) failed: {', '.join(sorted(failed))}")
        blocks, lengths, edges = merge(parts[s] for s in scripts)

    os.makedirs(os.path.dirname(out), exist_ok=True)
    executed = sum(n * int(lengths[a]) for a, n in blocks.items())
    with open(out, "w", newline="\n") as f:
        f.write(f"# Block profile of the recompiled i960 code, ROM set {args.set} (scripts/profile.py;\n")
        f.write("# format: tools/m2run/block_profile.h). Summed over the scenarios:\n")
        for s in scripts:
            f.write(f"#   {os.path.relpath(s, ROOT).replace(os.sep, '/')} ({frames_of(s)} frames)\n")
        f.write(f"# {len(blocks)} blocks run, {executed} instructions, {len(edges)} distinct transfers.\n")
        for a in sorted(blocks):
            f.write(f"block {a} {lengths[a]} {blocks[a]}\n")
        for (a, b) in sorted(edges):
            f.write(f"edge {a} {b} {edges[(a, b)]}\n")
    print(f"profile: {os.path.relpath(out, ROOT)}: {len(scripts)} scenarios, {len(blocks)} blocks run, "
          f"{executed} instructions, {len(edges)} distinct transfers")


if __name__ == "__main__":
    main()
