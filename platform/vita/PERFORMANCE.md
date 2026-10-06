# Vita performance diagnostics

## Calibrated native output (GPU25 / 01.23)

GPU24's 50% boost remained about 8.25 dB below reference RMS. GPU25 sets
shared native master gain to 1.95 (2.6x GPU24, +8.30 dB before limiting).
A stereo-linked 0.98-peak limiter has immediate attack and a 50 ms exponential
recovery time constant. State survives callback boundaries; there are no new
allocations or lookahead latency. Hard clipping and limiter-active frames have
separate counters. Reference audio, saved controls, clocks, game timing and
the quiet logging policy are unchanged.

The same 6,000-frame command replays measured stereo floating-point output
before frontend volume, using each backend's native sample rate:

| Replay | Reference RMS | GPU24 native RMS | GPU25 native RMS | GPU25 native peak |
| --- | ---: | ---: | ---: | ---: |
| Race | 0.184465747 | 0.071283997 | 0.183594874 | 0.980000079 |
| Attract | 0.080995231 | 0.031339877 | 0.081483683 | 0.607261658 |

GPU25 differs from reference RMS by -0.0411 dB in race and +0.0522 dB in
attract. Both replays had zero hard clips, invalid/unsupported events and
native faults; all reference FM samples were zero. Race limiting/recovery
covered 907,526 of 5,006,592 stereo frames (18.13%); attract needed no limiting.
Existing experimental event timing/pitch differences remain unchanged.
These are output-energy measurements, not waveform or perceptual parity.

The separate 6,000-frame health replay with a one-second graphics stall
rendered 5,054,592 stereo frames, RMS 0.183606, peak 0.980000, and 792,816
limiter-active/recovering frames. Hard clips, invalid/unsupported events and
callback allocations were zero; all 4,966 note starts were retained. Synthetic
tests cover the 2.6x gain below threshold, both peak polarities, stereo linking,
recovery/reset, exact render-block invariance and allocation-free 64-voice
rendering. All 19 runnable CTests and ASan/UBSan mixer tests passed (two optional
Lua tests skipped). Desktop and Vita builds passed; on-device listening and
limiter performance still require hardware validation.

## Native output level (GPU24 / 01.22)

GPU24 raises native mixer master gain from 0.5 to 0.75 (1.5x amplitude) in
the shared Vita/desktop code. Reference audio, saved volume/mute controls,
sequencing, renderer and clocks are unchanged. Routine logging stays off as
in GPU23; fault reporting is retained.

The same 6,000-frame host replay, including a one-second graphics stall,
rendered 5,054,592 stereo frames before and after the change. Peak changed
from 0.358508 to 0.537761; RMS from 0.047524 to 0.071286. Both runs reported
zero clipped samples, invalid/unsupported commands and callback allocations;
note and command totals were unchanged. Synthetic signed/stereo/pan/envelope
checks verify the 1.5x ratio and retain the safety-clamp tests. All 19 runnable
CTests passed (two optional Lua tests skipped); ASan/UBSan gain tests passed.
This verifies digital output levels, not perceived loudness on the device.

## Quiet native GXM build (GPU23 / 01.21)

Startup and periodic diagnostic logging are now off by default for the GXM
frontend. The logger does not create/truncate/append/sync the file during
normal startup, gameplay, settings changes or exit; its background writer is
not started. Periodic record formatting and optional board/audio profiling
clocks are disabled too. Game pacing, menu FPS, native audio health checks,
sound commands, renderer behavior and saved clock/audio settings are unchanged.

Unexpected failures still appear on screen where the frontend can display
them and append a bounded fault record to `vita-diag.log`. A quiet launch does
not erase an existing log. This is a routine-diagnostics switch, not suppression
of runtime faults. The older software-rendered diagnostic frontend is unchanged.

Since the build modes, `scripts/build_vita.py` makes this diagnostic build by
default (`-DDAYTONA_VITA_DIAGNOSTICS=ON`); `--release` turns the logging off
and link-time optimization on. The CMake option itself still defaults to OFF.
Keep GPU22 for an unchanged diagnostic comparison.

The supplied GPU22 device log contains 78 active windows covering 157.274 s,
all using native audio without a reference sound board. Audio produced
7,551,488 stereo frames and 7,969 notes. Native callback failures, unsupported
commands, invalid events, queue overflows, GPU draw errors and pool/material
drops all stayed zero. Maximum queue depth was three bytes and callback peak
was 4.662 ms. CPU/GPU clocks stayed at 444/166 MHz. The one startup material
deferral was the existing per-frame construction budget, not a resource error.

Simulation FPS ranged 22.45–57.45 (time-weighted 33.78), so the user's improved
smoothness is not proof of locked full speed. The log ends during normal
gameplay, without a clean-exit marker. Background diagnostic writes reached
39.56 ms; their duration is not main-thread blocking time. No observed health
fault requires routine logging to stay enabled.

## Native GXM builds (GPU22 / 01.20)

GPU22 adds the shared, opt-in native audio replacement used by both desktop
and Vita. Select `AUDIO ENGINE: NATIVE (TEST)` in Options and reset the game.
Reference remains the default. See [native audio](../../docs/native-audio.md)
for architecture, measured fidelity differences and reproduction commands.

Native mode has no reference sound board or sound worker. The SDL device
callback clocks the sequencer and mixer independently of game frames; periodic
records add `audio=NATIVE_TEST`, callback timing, queue depth and health counters.
Keep main CPU/geometry/GXM measurements separate from callback wall time.
The GPU20 road fixes and GPU21 reference pipeline remain unchanged.

The supplied GPU21 hardware run showed zero residual sound waits, but detailed
scenes still reached about 24 FPS. Moving more audio work is therefore not a
guarantee of full-speed graphics. GPU22 needs a fresh device measurement.

Host validation: 19 runnable CTests passed (two optional Lua tests skipped),
including native mixer, sequencer and both callback adapters. The native
6,000-frame race health test passed with no invalid/unsupported events,
clipping or callback heap allocations, exercising 129 samples and up to 41
voices. A one-second graphics stall still produced 48,000 audio frames and
33 new note starts. ASan/UBSan checks passed; this is not Vita runtime proof.

The sections below this one describe the original software-rendered diagnostic
patch, not the current `main_gpu.cpp` path. Diagnostic-enabled builds write
`ux0:data/daytona93/vita-diag.log`; copy it before relaunching.

- `geo_ms` measures complete geometrizer execution, including lighting. It is
  not a deadline that abandons later lighting work. Lighting is stored in each
  polygon before that polygon is published.
- `run_ms` measures main-thread board work. `sound_ms` and
  `audio_worker_queue_ms` are worker durations overlapping rendering and the
  following main-board frame; do not add them to main time. Their denominator
  is `sound_frames`, which may differ from `frames` by one at a window boundary.
  `sound_wait_ms` is the residual main-thread join per board frame.
- `poly_ms` includes `sort_ms`. `tiles_ms` and `upload_ms` measure the other
  native GXM preparation stages. These are CPU submission times, not GPU
  execution timestamps. `gpu_wait_ms` measures the pool-reuse fence.
- `menu`, `frames`, `presents`, and `window_ms` expose paused or repeated-frame
  windows. `frame_peak_ms` is the longest active main-loop iteration in this
  window (not GPU elapsed time).
- Periodic samples use a bounded background writer. `log_enqueue_prev_ms`
  measures the previous formatting/enqueue call; `log_write_prev_ms` and
  `log_write_peak_ms` report background I/O. `log_drops` and `log_failures`
  are cumulative counters. A busy/unavailable worker drops telemetry instead
  of blocking a gameplay frame. Startup/error/exit records stay synchronous,
  with a drain barrier before touching the shared file sink.

The GPU20 hardware log supplied for this iteration shows steady CPU 444/GPU
166 MHz, roughly 28-46 FPS in the 3D scenes, residual audio waits up to 7.84 ms,
and synchronous diagnostic writes increasing from about 5 to 20 ms. Geometry
and polygon submission still dominate detailed scenes; clocks were not dropping.
GPU21 keeps the confirmed road fix and moves the sound join past the next
main-board step. Each job owns its UART-byte packet; at most one sound job and
one newly prepared packet exist. Join before the next sound dispatch, audio
settings/pause, save/reset or game destruction. This changes host scheduling,
not guest timing, command order or sample count.

A further non-final-Z geometry decoder shortcut was tested and rejected:
interleaved 6,000-frame host runs averaged 783.56 ms baseline versus 792.19 ms
candidate, with identical polygon output. No geometry or GPU rendering change
is retained in GPU21. Host results do not establish device frame rate.

GPU21 host validation: all 15 runnable CTests pass (two optional Lua tests
skip), as do the renderer/lifetime ASan+UBSan checks and sound/audio/logger
thread-sanitizer tests. A 6,000-frame race compared every CPU screen hash and
11,589,332 FM plus 9,312,856 PCM float samples bit-for-bit, with identical
i960/TGP/68000 instruction counts and UART totals. The first sound job was
held until the next actual board frame completed. This checks overlap, packet
ownership, profile isolation and sample ordering, not Vita frame rate.

Reproduce the opt-in ROM-backed comparison after generating/importing the
user-supplied ROM set:

```sh
bash scripts/test_vita_sound_pipeline.sh build 6000
```

The banked-corner sampler audit (attract frames 1000-1120) found coordinates
outside the nominal texture dimensions on 48,582 polygons. The CPU masks those
coordinates even when the texture wrap flag is clear; that flag changes
bilinear neighbors at the seam, not the overall repetition. GPU19 selected
CLAMP in that case. Of 85,113 visible centroid samples, 4,037 became near-black
under CLAMP but bright under the CPU's repeating coordinates. GPU20 retains
MIRROR where requested and uses REPEAT otherwise. Exact seam interpolation and
mip/microtexture behavior remain separate GPU approximations.

GPU20's integer-key ordering preserves the CPU reference's window/depth/tie
order; the painter path still traverses that order in reverse. A 6,000-frame
host race compared 6,564,621 indices with no differences. Host timings do not
prove Vita speed or pixel output.

The rendering audit also found solid checker polygons filled as opaque quads
in GPU19. In the same host race, 27,614 such polygons were fully black, including
9,880 with bounding boxes over 5,000 native-screen pixels. Alternating-pixel
coverage must be retained, not replaced with an opaque shadow. Other GPU
approximations (including textured checker patterns, mip/microtexture sampling
and quantized lighting) remain separate fidelity limits; a black-road photo
alone does not distinguish them.

## Original software-rendered diagnostic patch

This patch is based on branch `psvita-native-frontend` at `d39e47a`, with
the separate 192 MiB startup correction already applied. It has not been
pushed to GitHub and is not a verified full-speed fix.

## Apply and build

Run from the repository root, after applying the startup patch. Save any
local edits first. The check does not overwrite conflicting changes.

```sh
git apply --check "$HOME/Downloads/daytona-vita-performance.patch" &&
git apply "$HOME/Downloads/daytona-vita-performance.patch" &&
python3 scripts/build_vita.py
```

The companion ZIP contains only changed/new source and test files. Its full
main.cpp and CMakeLists.txt include the startup correction as well. Prefer the
checked patch when retaining your own edits. Do not copy files into a different
revision without reviewing the differences.

The same existing host-generated game code is used; it need not be regenerated.
The build script still configures Release. Check a manually configured build:

```sh
grep -E 'CMAKE_BUILD_TYPE:|CMAKE_CXX_FLAGS_RELEASE:' build/vita/CMakeCache.txt
```

Install the rebuilt `build/vita/daytona_vita.vpk` with VitaShell. Leave the ROM
and saves under `ux0:data/daytona93/` in place. Keep the matching ELF locally.

## Capture

Leave the launcher open long enough for a `mode=MENU` sample, start the game,
and reproduce the slow scene until several `mode=GAME` samples have been
written. Copy `ux0:data/daytona93/vita.log` before relaunching: it is overwritten
on every launch. The new lines start with `perf:` and are written at most once
every two seconds, not per instruction. No file upload happens automatically.

## Read the measurements

`sim_fps` counts completed Model 2 frames per real second. `present_fps` counts
SDL presentations, which can include repeated images. Neither is a benchmark
result until observed on the device. CPU, GPU and bus clock readings are in MHz;
negative values indicate an SDK query error. The patch does not change clocks.

`total_ms`, `core_ms`, `geo_ms`, `video_ms` and `sound_ms` are averages per
completed simulation frame. `core_ms` is total minus the other three stages:
i960 execution, synchronous TGP work, scheduler and remaining frame overhead.
`geo_ms` wraps the geometry/vblank-start work; `video_ms` wraps software polygon
rasterization and screen composition; `sound_ms` wraps the 68000 and chip
simulation. Native-rate gameplay targets about 17.384 ms per board frame;
total work, presentation and scheduling all have to fit the real-time budget.

`audio_ms` measures conversion/queueing on the main thread, not the callback's
independent CPU cost. `upload_ms` is per screen upload; `draw_ms` and
`present_ms` are per SDL presentation; `save_ms` is per autosave check. They
have different denominators and must not simply be summed in MENU or when
frames are repeated. `present_ms` includes SDL command flushing, any driver
waits and vsync; it is not a GPU timestamp. These are elapsed host durations,
not CPU utilization percentages.

The loop presents each completed frame rather than calculating up to four
complete software rasters before displaying one. This can improve visible
updates and input-polling frequency when overloaded; it does not accelerate
the actual guest instructions or guarantee a higher simulation frame rate.
It deliberately discards overdue host-time debt under load. Fractional time
is retained to avoid accidentally halving the normal frame rate.

Text drawing now batches the same pixels in groups of up to 256 rectangles.
This reduces menu API calls; it is not a GPU conversion of the game renderer.
No geometry, sound, floating-point results or game instructions are skipped.

## Host checks

```sh
mkdir -p build
g++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc \
  tests/test_vita_performance.cpp -o build/test_vita_performance
build/test_vita_performance
g++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/vita_sdl_shim tests/test_vita_text.cpp -o build/test_vita_text
build/test_vita_text
```

The SDL shim is a unit-test drawing recorder only; do not include it in any
game target. Unit tests use synthetic timer values, not synthetic game code.
The real ROM-generated game, ARM compilation/linking, device performance,
audio, suspend/resume and race parity remain to be tested for this patch.
