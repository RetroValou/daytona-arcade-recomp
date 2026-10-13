"""Job count for `cmake --build --parallel N` (scripts/recompile.py, setup.py).

A bare `--parallel` is unbounded with the Unix Makefiles generator (`make -j`):
a fresh build directory then starts every compile at once (433 C/C++ compilers
in one srallyc build, about 26 GB, the out-of-memory killer took the desktop
down). One compiler on the largest generated files peaks at about 250 MB
(chunk_000.cpp, tgp_gen.cpp at -O3); 600 MB per job leaves room for the link.
M2_BUILD_JOBS overrides it."""

import os

MB_PER_JOB = 600


def available_mb():
    """Available memory in MB (Linux /proc/meminfo), or None where unknown."""
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) // 1024
    except OSError:
        pass
    return None


def build_jobs():
    env = os.environ.get("M2_BUILD_JOBS")
    if env and env.isdigit() and int(env) > 0:
        return int(env)
    jobs = os.cpu_count() or 2
    mb = available_mb()
    if mb is not None:
        jobs = min(jobs, mb // MB_PER_JOB)
    return max(1, jobs)
