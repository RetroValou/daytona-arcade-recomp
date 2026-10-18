# PS Vita port

A native Vita application (VitaSDK, SDL2 for input and audio, vitaGL for the GPU),
**not a PSP/Adrenaline build**. The i960, TGP and 68000 programs are recompiled on
the host from your ROM set, as for the desktop build; nothing is interpreted on the
Vita. One package per game: Daytona USA (`DAYT00093`), Sega Rally Championship
(`SRAL00094`), Virtua Fighter 2 (`VFTR00095`), installable side by side.

## Source layout

```
frontend/   main.cpp (start-up and the loop), launcher (the menu), session (the game
            being played: board, link, sound worker), diagnostics (perf.log,
            vita-diag, gl.log), settings (vita.cfg), storage (the data folder,
            files written whole, NV memories), input (pad, game layout), controls.h
render/     vitaGL renderer: renderer.h (the frontend's API), renderer.cpp (set-up,
            frame entry), model2_textures, model2_polygons, layers_2d (the System 24
            worker), gl_context (vitaGL, frame API, gl.log), shaders, draw_list,
            ui_draw (the menu's triangles), gl_internal.h (what they share); the
            ImGui backend and pure helpers (texel/flat index, polygon order,
            System 24 upload)
audio/      reference and native audio output, the sound worker, rate conversion
system/     core placement, per-core profiling, logs
net/        LAN link (link.h), compiled in only where it was tested (Daytona)
overrides/  runtime/video/geo.h/.cpp: replace the shared runtime's geometrizer (first
            on the include path; the original is compiled unchanged inside)
docs/       performance, optimization and diagnostics notes
```

Headers are included from this folder: `#include "system/core_policy.h"` (CMake and
the host tests add `platform/vita` to the include path).

## Build

Needs a host C++20 toolchain, VitaSDK (reference release 2026.08) with its SDL2
package, and vitaGL built inside the project:

```sh
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
vdpm install sdl2
python3 scripts/setup_vitagl.py          # once; again after a pin or flag change
python3 scripts/setup.py                 # host tools; recompiles the game if roms/<set>.zip exists
python3 scripts/build_vita.py --game GAME --fast-inaccuracy
```

`GAME` is `daytona` (default), `segarally` or `virtuafighter2` (`scripts/games.py`).
After changing the seeds or a recompiler, run `python3 scripts/recompile.py --game
GAME` first. Everything a game makes stays in its folder:

```text
build_<game>/recomp/<set>/   rom_cache/ (the images) and gen/ (the recompiled code)
build_<game>/pc/<set>/       the host build (the recompilers, m2run, the PC game)
build_<game>/vita/           <game>.vpk
build_<game>/vita_diag/      <game>_diag.vpk (--diagnostics: its own app, title ID 9xxxx)
```

`build_vita.py` options:

| Option | Effect |
| --- | --- |
| `--fast-inaccuracy` | i960 lockstep checks per block and direct work-RAM accesses, TGP counted per block: the fastest (checked on every game) |
| `--fast-gen` | `scripts/fast_gen.py`'s rewrite of the exact i960 code (Daytona only) |
| `--diagnostics` | `vita-diag.log`, `perf.log` (per-core report, `system/core_profile.h`) and `gl.log` |
| `--tgp-profile`, `--i960-profile` | their sections in `perf.log` (imply `--diagnostics`) |
| `--release` | link-time optimization, no logs; slow to build, only for a release package |
| `--free-core` | threads unpinned, so the 4TH CORE option can move any of them |
| `--link`, `--no-link` | link play in or out (default: in for Daytona only, `vita_link`) |
| `--no-fast-paths`, `--reference-renderer`, `--inline-bus` | comparison builds |
| `--compile-check` | objects only, no ROM needed (what CI runs) |
| `--build-dir`, `--title-id` | another build folder or title ID, to keep two variants |

Icon and LiveArea images, per game: `icon0.png` (128x128), `bg.png` (840x500),
`startup.png` (280x158) and optionally `template.xml` in `logo_livearea/<game>/`
(8-bit indexed PNGs; see the README there). They are not committed.

Do not configure the host build directory with the Vita toolchain: the importer and
the recompilers run on the computer. The Vita configuration fails rather than
building an empty game if a generated source group is missing.

## Install and play

Install the VPK with VitaShell and put your complete (MAME full merged) ZIP ROM set at
`ux0:data/<set>/<set>.zip`: `daytona93`, `srallyc` (MAME's merged `srallycb.zip`
works, renamed) or `vf2`. The importer checks every file's CRC; a renamed other set
is refused. The Vita reads `.zip` only (no 7z decoder: its memory peaks); repack a
`.7z` set as `.zip` with the same file names.

Each game's controls come from its record (`Game::control_list`, `src/games/*.cpp`):

| Daytona USA | Sega Rally | Virtua Fighter 2 | Vita control |
| --- | --- | --- | --- |
| Steering | Steering | Joystick | Left stick, or D-pad left/right (VF2: D-pad or left stick, four directions) |
| Accelerate / brake | Accelerate / brake | - | R / L (analogue: right stick up / down) |
| Shift up / down | Shift up / down | - | D-pad up / down (one shift per press) |
| View 1 / 2 / 3 / 4 | View (one button) | - | Cross / Circle / Square / Triangle (Sega Rally: Triangle) |
| - | Hand brake | - | Cross |
| - | - | Punch / Kick / Guard | Cross / Circle / Square |
| Coin / start | Coin / start | Coin / start | Select / Start |

Virtua Cop and Virtua Cop 2 (one player) are played on the front touch screen: a touch on the
picture aims there and pulls the trigger; a touch on the black margins left or
right of it points the gun off the screen and pulls the trigger, which reloads.
Cross is the trigger too (at the last aim). The touch panel's coordinates are
scaled to the screen from its own display corners (`sceTouchGetPanelInfo`:
not exactly twice the screen), the picture's place from `render/gl_internal.h`
(496x384 scaled to 544 lines, centred); `rt::aim_gun` turns a picture position
into the gun's units (`Game::gun`, MAME's range).

Fixed: Start+Select opens the menu; Select+Triangle is the cabinet's Test switch,
Select+Square its Service switch (in test mode Cross gives VR1, menu next, and Start
selects); plain Select inserts a coin on release. The Controls page rebinds each of
the game's controls to one button (Cross then the new button, or Left/Right to cycle;
a button already used swaps). Bindings are saved as `bind_*` keys; an invalid or
duplicated set goes back to the game's defaults.

The menu: D-pad, Cross, Circle, or the front touch screen. Options lists only what
can change something for this game in this build (`option_available`,
`frontend/launcher.cpp`):

* CPU / GPU clock (default 444 / 166 MHz; 500 MHz needs an overclock plugin and
  falls back to 444; the menu shows the actual clock), 4TH CORE (needs a core-unlock
  plugin; checked by reading the affinity back).
* Volume, mute, Music and Effects (the MultiPCM voices split by the game's channel;
  shown when the board can tell them apart, `rt::has_music_effects`), the audio
  engine (Daytona: the native engine, still being validated).
* Dead zone, invert steering and steering curve (Linear, Soft, Extra Soft; for a game
  with a wheel), draw distance (a game with its hook, `Game::draw_distance`), polygon
  order (ARCADE, the default, or HYBRID: see the renderer below; every game).
* Skip launcher, save records (backup RAM), hold Test button (holds Test for three
  game seconds at the next start, not saved), the ROM switch (a package holding the
  companion set), link play (below), reset defaults.

The picture is the original 496x384 at 4:3, scaled to the 544 lines and centred.

### Daytona: two sets and link play

`--revision-a-self` bundles Revision A with daytona93; the ROM option then switches
executables (each set keeps its own folder and settings):

```sh
python3 scripts/recompile.py --set daytona
python3 scripts/build_vita.py --set daytona --build-dir build_daytona/vita_revision_a
python3 scripts/build_vita.py --game daytona --revision-a-self build_daytona/vita_revision_a/eboot.bin
```

Link play (Daytona's packages only) needs the communication board, which only
Revision A has (`Game::link`), on every cabinet, and a Wi-Fi LAN: the dual package
above, switched to Revision A. In Options: Link Play on,
the next cabinet's IPv4 address and port, the listen port (default 15112), then
reset. Two cabinets point at each other; more form a ring. The desktop build uses the
same TCP protocol. The menu shows the Vita's IP, both connections and the cabinet
number. In test mode, GAME SYSTEM: one MASTER, the others SLAVE, unique car numbers,
the same cabinet type and region. Frame sync is optional. Vita-to-Vita play still
needs hardware validation.

## Threads and cores

| core (default) | thread | work |
|---|---|---|
| 0 | main | input and menu; the i960 game code; the TGP (run by the i960, no thread of its own); at vblank start, a copy of the display list RAM (128 KiB) for the geometry thread; at vblank end, the System 24 tile update; the sound hand-off; then the GPU frame: polygon priority sort, materials, vertices, submission and swap |
| 1 | geometry | the display list parse (the MAME geometrizer, unmodified) of the frame the main core just finished |
| 2 | sound worker | the reference sound board of the frame just finished |
| 2 | 2D worker | System 24 tile and palette uploads into the second texture set, layer rectangles |
| any | SDL audio callbacks | mixing the queued samples (reference engine), or the whole native sound engine |

The 3D and the 2D are shown one frame late, in phase: the geometrizer parses frame N
while the game runs frame N+1, and the 2D worker fills the other of two texture sets.
Each worker is joined before its data is touched again; at most one sound packet is
in flight. `--free-core` leaves every thread unpinned; single cores move with
`DAYTONA_VITA_{MAIN,GEO,SOUND,2D}_CORE`.

At most one emulated frame runs before each present: when the Vita cannot keep up,
the game slows down rather than skipping frames (no guest instruction is skipped).

Reference audio: the board's samples are queued at its own rates and converted once,
in the callback (cubic interpolation, `audio/audio_rate.h`), with a 64 ms cushion kept
by a slight speed change, one short silent gap after an underrun.

SoftFloat's state is used by the main thread only; moving board execution to more
threads would need separate state.

## Saved data

In `ux0:data/<set>/`: `vita.cfg` (settings), `ioboard_eeprom.bin` (test mode
settings, always kept), `backup_ram.bin` (records and unused coins, only with SAVE
RECORDS on). Saved when the menu opens, on a ROM switch and on quitting, each file
written to a temporary and renamed. Logs: `vita-diag.log` (faults; everything in a
diagnostics build), `perf.log` and `gl.log` (diagnostics builds).

## vitaGL renderer

* Polygons follow the Model 2 draw priority (higher window first, then smaller z sort
  key, then newest polygon first). The Model 2 has no depth buffer; the GPU's only
  reproduces that order: each polygon gets one depth from its rank (`GL_GEQUAL`), so
  intersecting polygons do not cut, as on the arcade board. Polygons are grouped by
  (clip, shader, texture), one draw call per group, as indexed quads.
* Option POLYGON ORDER: HYBRID (off by default: ARCADE) draws with a second set of the
  7 programs, compiled with `upgrade_z_depth` (`render/shaders.cpp`). Each Model 2
  window keeps its own band of the depth range (`kZDepthBands`, 32), as the board draws
  a window over the ones before it, so a game's UI windows stay in front; the CPU codes
  each polygon's depth as 1 + band + its rank in the window (`model2_polygons.cpp`).
  A game can put several windows in one band (`Game::update_z_depth_window_groups`:
  VF2's 1-7, so its scenery and fighters mix).
  Inside the band, each vertex's depth is the rank, weighted 0.9999 - `update_z_depth_LOCAL_WEIGHT`, plus a nearness
  from its distance w on a log scale, weighted `update_z_depth_LOCAL_WEIGHT` (1 at w 1,
  0 at e^`update_z_depth_SCALE`), so shapes that cross (a car's shadow and its wheel)
  cut by distance while coplanar ones keep the rank order. The 2D layers keep their
  depths (0, `kForegroundDepth`); the result stays below the HUD. Both values are the
  game's (`Game::update_z_depth_local_weight`, `update_z_depth_scale`: 0.9 / 12.5,
  Sega Rally 0.91 / 12.5, Daytona 6.0, VF2 4.5, Virtua Cop 7.2, Virtua Cop 2 7.3, from each game's w
  99.9th percentile on the PC). The switch picks the set at draw time
  (`gl_set_upgrade_z_depth`, also read by the CPU coding); the first boot compiles
  14 programs.
* System 24 layers are placed by the same depth buffer: foreground before the polygons,
  background after them, so hidden pixels are rejected before their shader runs
  (`k2DLayersByDepth` in `render/gl_internal.h`; false restores the painter order,
  same image).
* Needs `libshacccg.suprx` on the console (runtime shader compiler); the compiled
  shaders are cached in `ux0:data/shader_cache/<TITLE_ID>/`.

### How the 2D layers (System 24) are drawn

The HUD, the sky and the other 2D layers come from the System 24 tilemap chip. Like
the arcade hardware, the renderer keeps **indexed images** and resolves the colours
on the GPU.

* **Layer textures hold colour numbers.** Each of the 4 layers is a 512x512 texture
  (one per pass: 8 in all). A texel stores the pen number (0-8191) as palette
  coordinates: column (`pen % 128`) in red, row (`pen / 128`) in green; alpha 0 for an
  empty pixel, which the shader discards (`system24_index_texel`,
  `render/system24_upload.h`).
* **One palette texture holds every colour**: 128x64, one texel per pen.
* **The shader does the lookup**, point sampled on texture unit 2.

  ```
  layer texture (512x512)          palette texture (128x64)
  texel = pen 1234  ──────────────▶ texel (1234 % 128, 1234 / 128) = colour ──▶ pixel
  ```
* **Fades cost almost nothing.** A fade rewrites palette RAM only: the renderer copies
  the 8192 colours (32 KB) when one changed. With RGB textures each fade step meant
  rewriting all 16384 tiles (8 MB, ~67 ms on the Vita).
* **Only changed tiles are rewritten.** Each 8x8 tile has a generation number;
  `upload_system24_layer_indices` rewrites changed tiles by runs, each texel line in
  one go (NEON, 8 texels per step).

Model 2 polygon textures work the same way, except that their 4-bit texels are
**filtered** by the GPU before the lookup, as the real board did: each polygon gets a
128-entry row of a palette texture (per luma table, colour and face brightness), built
by the CPU the first time a combination appears, then cached.

### Performance (vitaGL build)

Measured on a Vita at CPU 444 MHz / GPU 166 MHz, pinned cores, `--fast-inaccuracy`,
`perf.log` (41 windows of 5 s, about 3.5 minutes of Daytona racing):

| | Result |
| --- | --- |
| Speed | median **57.3 fps** for 57.52 on the arcade (99.6%); 40 of 41 windows at 55 fps or more |
| Polygons | about 1,200 per frame on average, up to 1,700 |
| Core 0 (game + drawing) | 74% busy on average, 95% at worst: the limit |
| Core 1 (geometry) | 53% on average, 71% at worst |
| Core 2 (sound + 2D) | 78% on average, 87% at worst |
| Sound | 8 short gaps in 3.5 minutes, playback speed 99-100% |

More in [docs/PERFORMANCE.md](docs/PERFORMANCE.md), [docs/GEO_ASYNC.md](docs/GEO_ASYNC.md)
and [docs/PERF_BREAKDOWN.md](docs/PERF_BREAKDOWN.md).

### vitaGL: pinned build and flags

vitaGL, vitaShaRK and the Vita port of math-neon are the work of **Rinnegatamante**
(math-neon originally by Lachlan Tychsen-Smith), SceShaccCgExt of **Bythos**: thank
you! This renderer would not exist without them.

`scripts/setup_vitagl.py` clones vitaGL and the libraries it is linked with at fixed
commits and builds them into `extern/vitagl/` (git-ignored): a new vitaGL commit
upstream never changes this build; moving to one means editing `PINS` in the script
and testing on the console.

| Library | Why | Pinned commit |
| --- | --- | --- |
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) | the OpenGL ES layer over sceGxm | `dca4b9d143290d78ec043131a19be36c78cdc7b5` (2026-10-07) |
| [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) | runtime shader compiler used by vitaGL (`libshacccg.suprx`) | `df24065e65098b2d1ac533760109ad4367573f28` (2026-08-22) |
| [math-neon](https://github.com/Rinnegatamante/math-neon) | NEON maths used by vitaGL | `0faab814782c071ff4015527f1ca955ab1ccc470` (2020-07-05) |
| [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) | needed by vitaShaRK | `fb0e9d338525b067f3679ab33571323336493cca` (2026-07-29) |

Nothing is installed into `$VITASDK`, and a vitaGL already there is neither used nor
changed: the script puts its own headers first (`CPATH`) and `CMakeLists.txt` links
the libraries of `extern/vitagl/install` by full path. The configure step and the
`build_vita.py` summary print the vitaGL used (`GPU GL: pinned vitaGL <commit>
(release: ...)`). Without `extern/vitagl/install` the build falls back to the SDK's
vitaGL with a warning. A rerun with the same pins, flags and toolchain does nothing;
`--force` rebuilds, `--print-flags` shows the make flags.

**Release flags** (default, `--profile release`), chosen from what the renderer does:
GLSL shaders only, `GL_TRIANGLES` only, 16-bit indices, no instancing, no framebuffer
objects, no `glTexSubImage2D` (texture memory written in place after `sceGxmFinish`),
one VBO on our own GPU memory (`vglBufferData`), every GL call on the main thread.

| Flag | Effect for this renderer |
| --- | --- |
| `NO_DEBUG=1` | No argument validation in every GL call (CPU time on the busiest core). The renderer checks shader compile/link status itself. |
| `HAVE_VERTEX_LAYOUT_CACHE=1` | The attribute layout and patched vertex program are cached per (program, VAO, draw type); our layout changes once per frame, our draws are many. |
| `HAVE_SHADER_CACHE=1` | The 7 programs' compiled shaders are cached per title ID: only the first boot runs the runtime compiler. |
| `TEXTURES_SPEEDHACK=1` | No texture use tracking in every draw; what it gives up (the copy on `glTexSubImage2D` of a texture in use) is never needed. |
| `INDICES_SPEEDHACK=1` | Skips per-stream index source setup; only breaks instanced draws and 32-bit indices, not used. |
| `PRIMITIVES_SPEEDHACK=1` | Skips the polygon mode restore after each draw; only breaks `GL_LINES` / `GL_POINTS`, not used. |

`DRAW_SPEEDHACK` is refused: `glDrawArrays` would ignore its first vertex, which the
indexed quads need (`render/draw_list.cpp`).

**Debug flags** (`--profile debug`): `LOG_ERRORS=1` (GL errors through
`sceClibPrintf`) and `HAVE_SHARK_LOG=1` (shader compiler messages), every speed hack
off: the first thing to try on a rendering glitch. Read the output with a log plugin
(PrincessLog, for example).

To prove the script never reads the SDK's vitaGL, run it with a copy of the SDK that
has none, from a fresh clone:

```sh
cp -al "$VITASDK" /tmp/vitasdk-nogl            # hard links (macOS APFS: cp -Rc)
cd /tmp/vitasdk-nogl/arm-vita-eabi
rm -f include/vitaGL.h include/vitashark.h include/math_neon.h include/shacccg_ext.h \
      lib/libvitaGL.a lib/libvitashark.a lib/libmathneon.a lib/libSceShaccCgExt.a
git clone <this repository> /tmp/daytona-clean && cd /tmp/daytona-clean
VITASDK=/tmp/vitasdk-nogl python3 scripts/setup_vitagl.py
VITASDK=/tmp/vitasdk-nogl python3 scripts/setup.py --no-build
VITASDK=/tmp/vitasdk-nogl python3 scripts/build_vita.py --compile-check
rm -rf /tmp/vitasdk-nogl /tmp/daytona-clean      # the real SDK is untouched
```

## Tests

Host tests (no ROM or SDK): configure the root build with `-DM2_VITA_TESTS=ON`, then
`ctest -R vita`; `python3 scripts/test_vita_renderer.py` (the optimized rasterizer and
tile cache against their reference); `python3 -m unittest discover -s tests/scripts -p
'test_*vita*.py'`. `build_vita.py --compile-check` builds the ARM objects without a
ROM: it does not link game code, produce a VPK, or test anything on the console.

See [HANDOFF.md](HANDOFF.md) for the port's status and
[THIRD_PARTY.md](THIRD_PARTY.md) for the SDK dependencies.
