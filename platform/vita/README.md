# Native PS Vita target (experimental)

This is a VitaSDK/SDL2 frontend for the existing native runtime. It is a
native Vita application, **not a PSP/Adrenaline build**. The i960, TGP and
68000 programs still come from the host recompilation pipeline. No
interpreter, replacement game logic, ROM bytes or generated code is added.

The desktop SDL3/SDL_GPU application and root CMake build are unchanged.
The Vita frontend uploads the existing 496x384 software-composited screen
through SDL2's Vita renderer. This does **not** move the Model 2 rasterizer
onto the Vita GPU. Performance, memory headroom and full-race parity must
be measured on a real Vita before this target is considered supported.

## Build

Use a homebrew-enabled Vita, a host C++20 toolchain and VitaSDK with its
SDL2 development package. Reference SDK release: 2026.08. Set `VITASDK`
and install the package with that release's package manager:

```sh
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
vdpm install sdl2
```

Follow the SDK's installation documentation at https://vitasdk.org/ for a
new SDK install. No proprietary SDK or runtime module is required by this
frontend; it uses SDL2's normal Vita renderer, not PVR/PIB.

From the repository root, prepare the host build using your own complete
`daytona93` ROM set in `roms/daytona93.zip` (or `.7z`):

```sh
python3 scripts/setup.py
# After changing the seeds/recompilers, regenerate with the HOST compiler:
python3 scripts/recompile.py
# Build a separate ARM executable and installable package:
python3 scripts/build_vita.py
```

`setup.py` already recompiles when the ROM set is present. The explicit
`recompile.py` command is only necessary after changes or when adding the
ROM set later. The default output is:

```text
build/vita/daytona_vita.vpk
```

A different host build directory is supported:

```sh
python3 scripts/build_vita.py --host-build-dir build-host --build-dir build/vita --jobs 4
```

The equivalent CMake configuration is:

```sh
cmake -S platform/vita -B build/vita \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDAYTONA_GEN_ROOT="$PWD/build/gen"
cmake --build build/vita --parallel 4
```

Do not configure the host build directory with the Vita toolchain. The
importer and three recompilers run on your computer, not on the Vita. The
Vita configuration fails rather than creating an empty game if any required
generated source group is missing. Keep VPKs, ROM caches and generated C++
local, under the ignored `build/` directory.

## Install and play

Install your locally built VPK with VitaShell, then put your **complete
ZIP ROM set** at:

```text
ux0:data/daytona93/daytona93.zip
```

Launch **Daytona Recomp** and choose **START GAME**. ROM CRC and size
validation uses the existing importer. Renaming a different Daytona set
will not make it compatible. The Vita target deliberately omits the 7z
SDK to avoid solid-archive decoding memory spikes. A host `.7z` source can
still be used for recompilation; repack the complete set as `.zip` with
unchanged ROM filenames before copying it to the Vita.

| Action | Vita control |
| --- | --- |
| Steering | Left stick, or D-pad left/right |
| Accelerate / brake | R / L |
| Analogue accelerate / brake | Right stick up / down |
| Shift up / down | D-pad up / down (one shift per press) |
| View 1 / 2 / 3 / 4 | Cross / Circle / Square / Triangle |
| Coin / start | Select / Start |
| Pause menu | Start + Select together |
| Navigate / select / resume | D-pad / Cross / Circle |
| Test switch / service coin | Pause-menu entries |

The menu chord suppresses coin/start while both buttons are held. Pressing
Select significantly before Start can still insert a coin before the chord
exists. Buttons must be released after loading, pausing or resuming so menu
presses do not leak into the game.

The frontend retains the desktop's square-pixel framebuffer aspect ratio,
with side bars on the Vita display. Simulation steps use
`rt::GameLoop::kFrameHz`, not a hard-coded 60 Hz. The Vita frontend now runs
at most one complete simulation frame before presenting it. Fractional host
time is retained at normal speed; overdue whole steps are discarded under
load rather than rendering four complete frames and displaying only the last.
This slows wall-clock progress when the device cannot keep up; it does not
skip guest instructions, increase the guest timestep, or make the simulation
itself four times faster. The generic FrameClock default remains four steps.

See [PERFORMANCE.md](PERFORMANCE.md) for the stage timings now written to
`vita.log`. This is a diagnostic build, not a confirmed full-speed fix.

## Sound and saved data

FM and PCM are resampled independently to the output device rate, mixed,
clamped and played as stereo signed 16-bit audio. The callback only consumes
samples; board execution stays on the main thread. Queues are bounded and
cleared on pause/reset. Mute is saved independently of board state.

EEPROM and backup RAM are saved when changed, every five seconds and on
pause/reset/quit. Writes use a temporary file and retain a `.bak` generation;
loads reject incorrect sizes and try the backup. A sudden power loss can
still lose changes since the last successful save. Use SAVE AND QUIT for a
clean exit. All files are under `ux0:data/daytona93/`:

```text
ioboard_eeprom.bin
backup_ram.bin
mute.bin
vita.log
```

`vita.log` is replaced on each launch. Copy it before reopening the app when
reporting a crash. Missing/incorrect ROM errors and runtime faults are also
shown in the menu. A decoder/runtime fault requires a reset rather than
resuming a possibly inconsistent board state.

## Validation and limitations

Host tests (no ROM or SDK required):

```sh
mkdir -p build
c++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  tests/test_vita_controls.cpp -o build/test_vita_controls
build/test_vita_controls
python3 -m unittest discover -s tests -p test_build_vita.py -v
```

ROM-free ARM compile check (requires VitaSDK, SDL2, and fetched SoftFloat/ymfm):

```sh
python3 scripts/setup.py --no-build
python3 scripts/build_vita.py --compile-check
```

The included workflow performs the host tests and this ARM compile check.
**Compile-check mode makes objects only: it does not link generated game
code, produce a VPK, prove floating-point parity on ARM, or test gameplay.**
It intentionally does not upload artifacts or use ROM secrets.

This target still needs a full ROM-generated ARM link and real-device
verification: boot, all courses, manual/automatic gears, sound, sustained
frame time, peak memory, saved settings, and suspend/resume. The Vita
frontend handles SDL background/foreground events and limits post-stall
catch-up, but whether the installed SDL build emits those events during
system suspend must be verified on hardware. No overclock is forced.

SoftFloat keeps the existing 8086-SSE *semantic specialization* (not x86
machine instructions) but uses a project-owned portable platform header
without GCC `__int128`. Its state is main-thread-only in this target. Do
not move board execution onto multiple threads without restoring TLS or
introducing explicitly separate SoftFloat state.

See [HANDOFF.md](HANDOFF.md) for the port's status and
[THIRD_PARTY.md](THIRD_PARTY.md) for SDK dependency references.

## vitaGL renderer (`--gpu-gl`, experimental)

`scripts/build_vita.py --gpu-gl` builds `main_gpu.cpp` + `gpu_gl.cpp` against vitaGL
instead of libvita2d (`--gpu-fast`). The two flags are mutually exclusive because both
initialise sceGxm. Without either flag the CPU-exact build is produced as before.

* Build requirements: vitaGL (built with `HAVE_SHARK=1`), vitaShaRK, mathneon, and
  `libshacccg.suprx` installed on the console (runtime shader compiler).
* Polygons follow the Model 2 draw priority, as in the CPU reference renderer (higher
  window first, then smaller z sort key, then newest polygon first). The Model 2 has no
  depth buffer; the GPU one only reproduces that order: each polygon gets one depth from
  its rank (`GL_GEQUAL`, never cleared during the frame), so whole polygons are in front
  of or behind each other and intersecting polygons do not cut, as on the arcade board.
  Polygons are grouped by (clip, shader, texture) into one draw call per group.
* System 24 layers are placed by the same depth buffer: foreground before the polygons
  (in front of every polygon), background after them (behind every polygon), so hidden
  pixels are rejected before their shader runs. `k2DLayersByDepth = false` in
  `gpu_gl.cpp` restores the plain painter order (same image). The menu font never uses
  the depth buffer.
  
### How the 2D layers (System 24) are drawn

The HUD, the sky and the other 2D layers come from the System 24 tilemap chip of the
board. The vitaGL renderer does not turn them into RGB images: like the arcade
hardware, it keeps **indexed images** and resolves the colours on the GPU.

* **Layer textures hold colour numbers, not colours.** Each of the 4 layers is a
  512x512 texture (one for the background pass, one for the foreground pass: 8 in
  all). A texel stores the pen number (0-8191) of that pixel, encoded as palette
  texture coordinates: column (`pen % 128`) in red, row (`pen / 128`) in green.
  Alpha is 0 for an empty pixel, which the shader discards (it never reads a colour).
  See `system24_index_texel` in `system24_upload.h`.
* **One palette texture holds every colour.** A 128x64 texture, one texel per pen:
  all 8192 System 24 colours, shared by every layer and both passes
  (`upload_system24_palette`).
* **The shader does the lookup.** For each pixel the `Layer` shader reads the layer
  texture at the scrolled position, gets the pen number, then reads exactly that
  texel of the palette texture (bound on texture unit 2, point sampled: no filtering,
  which would mix unrelated colours). The palette is a lookup table: it is never
  displayed or scaled.

  ```
  layer texture (512x512)          palette texture (128x64)
  texel = pen 1234  ──────────────▶ texel (1234 % 128, 1234 / 128) = colour ──▶ pixel
  ```
* **Fades cost almost nothing.** A fade to black is done by the game itself: it
  rewrites its palette RAM step by step, the tile pixels do not change. The renderer
  only copies the 8192 colours (32 KB) into the palette texture when a colour changed.
  With RGB textures, every fade step meant rewriting all 16384 tiles (8 MB, ~67 ms on
  the Vita): the old stutters during fades.
* **Only changed tiles are rewritten.** Each 8x8 tile has a generation number;
  `upload_system24_layer_indices` rewrites the tiles changed since the last upload,
  row of tiles by row of tiles: changed neighbouring tiles become one run, and each
  texel line of a run is written in one go (NEON, 8 texels per step). A full rewrite
  (scene change) is then long sequential lines, which suits the write-combined GPU
  memory.
* **One frame late, prepared on core 2.** The geometrizer runs pipelined (the 3D shown
  is the previous frame's), so the 2D shown is the previous frame's too. There are two
  sets of layer + palette textures: while a frame draws the set prepared during the
  previous frame, the `daytona_2d` worker thread (core 2, `DAYTONA_VITA_2D_CORE`)
  uploads the current frame's tiles and palette into the other set and computes the
  layer rectangles (scroll, split screen, windows). The main core only emits the quads.
* **Placement by depth.** Foreground layers are drawn before the polygons and
  background layers after them, with fixed depths (see above).

Model 2 polygon textures work on the same principle, with one difference: their texels
are 4-bit indices that the GPU **filters** (blends between neighbours) before the
lookup, as the real board did. Each polygon gets one 128-entry row of a palette texture
(one row per luma table, colour and face brightness), so the filtered in-between values
also have a colour. The rows are built by the CPU the first time a combination appears,
then cached.
