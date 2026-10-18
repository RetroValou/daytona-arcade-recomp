#pragma once

// What the vitaGL renderer's files share (render/*.cpp, not the frontend): the screen
// mapping, the draw list, the frame's state and the small helpers that record into it.
// The design of the renderer is described at the top of render/renderer.cpp.

#include "render/renderer.h"
#include "render/polygon_order.h"
#include "render/flat_index.h"

#include <psp2/kernel/processmgr.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef DAYTONA_VITA_DIAGNOSTICS
#define DAYTONA_VITA_DIAGNOSTICS 0
#endif

namespace vita::gldraw {

// ---- Screen ------------------------------------------------------------------------
// The 496x384 Model 2 picture is scaled to the 544 lines of the Vita and centred.
constexpr float kDisplayW = 960.0f;
constexpr float kDisplayH = 544.0f;
constexpr float kSourceW = float(rt::Video::W);
constexpr float kSourceH = float(rt::Video::H);
constexpr float kScale = kDisplayH / kSourceH;
constexpr float kOffsetX = (kDisplayW - kSourceW * kScale) * 0.5f;

inline float sx(float x) { return kOffsetX + x * kScale; } // native -> display pixels
inline float sy(float y) { return y * kScale; }

// Depth: the Model 2 has no depth buffer, it draws its polygons in priority order. The
// depth buffer only reproduces that order: one value per polygon (Vertex::d, the same on
// every vertex), d = 1 - (rank + 1) / (polygons + 1), rank in the Model 2 draw order above.
// Larger = in front (test GL_GEQUAL, cleared to 0, never cleared during the frame). The
// vertex shader outputs z = d * w, so it is d again after the division. A polygon is
// entirely in front of or behind another one: intersecting polygons do not cut, as on the
// arcade board.
//
// 2D layers sorted by the same depth buffer instead of by drawing order (every polygon
// depth is strictly inside (0, 1)). Drawing order:
//   1. foreground layers  depth test + write at kForegroundDepth (in front of every
//                         polygon): polygon pixels hidden by the HUD are rejected by the
//                         depth test before their fragment shader runs.
//   2. polygons           unchanged.
//   3. background         backdrop + layers, depth test only at kBackgroundDepth (behind
//                         every polygon, equal to the cleared depth): only the pixels no
//                         polygon covered are shaded, the rest is rejected before the shader.
// The image is the same as painting background, polygons, foreground in that order (no
// blending anywhere in the game image). false: plain painter order (background, polygons,
// foreground, 2D without depth test), same image, kept to compare on the console.
constexpr bool k2DLayersByDepth = true;

// Core of the 2D worker thread (System 24 uploads + layer rectangles): 2, the sound
// core, the least loaded one in the race (-1 = any application core).
#ifndef DAYTONA_VITA_2D_CORE
#define DAYTONA_VITA_2D_CORE 2
#endif
constexpr int kWorker2DCore = DAYTONA_VITA_2D_CORE;
constexpr float kForegroundDepth = 0.99999994f; // largest float below 1: z stays inside w
constexpr float kBackgroundDepth = 0.0f;        // glClear writes 0 (glClearDepthf(0))

// vitaGL's glDrawArrays reads a fixed index table of 0xC000 entries (MAX_IDX_NUMBER):
// never draw more vertices in one call. Multiple of 3.
constexpr size_t kMaxVerticesPerDraw = 49152;

// Indexed quads (polygons and System 24 rectangles). A static index buffer turns each group
// of 4 vertices {a, b, c, d} into the triangles (a, b, c) (a, c, d): a Model 2 quad needs 4
// vertices instead of 6 (95% of the polygons), and a clipped n-gon ceil((n-2)/2) quads
// instead of n-2 triangles (an odd fan ends with a degenerate quad, d = c, which draws
// nothing). Same triangles as the fan, so the same image, with a third less vertex data
// written to uncached GPU memory (perf.log: ~14,000 vertices = 435 KB per frame in a
// race, ~2.3 ms of the main core). Indices are absolute 16-bit vertex numbers, the vertex
// attributes stay at offset 0 (see set_attributes): batches start on a multiple of 4 and
// only the first kIndexedVertexLimit vertices of a frame are indexed; anything beyond, or
// a failed index buffer, falls back to plain triangles.
constexpr size_t kIndexedVertexLimit = 65536;              // 16-bit indices
constexpr size_t kQuadIndices = kIndexedVertexLimit / 4 * 6; // 98,304 indices, 192 KB

// Polygon luma kept in the palette key. 0xfc = 64 brightness levels per palette;
// 0 = always full brightness (fewest palettes).
constexpr uint8_t kLumaMask = 0xfc;

// ---- Log: ux0:data/<set>/gl.log, rewritten at every start -----------------------------
// Each line is appended then the file is closed, so the last line survives a crash.
#ifndef M2_ROMSET
#define M2_ROMSET "daytona93"
#endif
constexpr const char *kLogPath = "ux0:data/" M2_ROMSET "/gl.log";
constexpr unsigned kLogEvery = 300; // frames per report (averages over that period)
// No GPU time sampling: waiting for the GPU before and after a frame stalled the main core
// 30-50 ms on purpose, the only frame time spikes perf.log showed in the race. The gl.log
// write is reported to perf.log as logging, not as frame end (gl_last_diagnostic_us).

// gl.log exists only in diagnostic builds (scripts/build_vita.py --diagnostics).
constexpr bool kGlLog = DAYTONA_VITA_DIAGNOSTICS != 0;

void gl_log(const char *fmt, ...); // gl.log, diagnostics builds only (gl_context.cpp)
inline uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
void wait_gpu();                   // until the GPU has finished every submitted frame
void set_error(const char *what, const char *log = nullptr);

// Opaque textures must not use an alpha-test variant: 'discard' disables the PowerVR
// hidden surface removal that makes the depth buffer cheap.
enum class Prog : uint8_t { Flat, Layer, Model, ModelAlpha, FlatChecker, ModelChecker, Ui, Count };
struct ProgramDef { const char *name; const char *defines; };
extern const ProgramDef kPrograms[int(Prog::Count)]; // shaders.cpp
inline bool textured(Prog p) { return p != Prog::Flat && p != Prog::FlatChecker; }

// shaders.cpp; 0 on failure (set_error). upgrade_z_depth: the hybrid depth variant
// of the vertex shader (kVertexShader's #ifdef upgrade_z_depth).
GLuint build_program(const ProgramDef &def, bool upgrade_z_depth = false);

// ---- Draw list -----------------------------------------------------------------------
struct Vertex {
    float x, y, w;  // x * w, y * w in display pixels, w (1 for 2D quads)
    float u, v;     // texture coordinates
    float p;        // palette row coordinate (Model 2 textures only)
    float d;        // depth in [0, 1], larger = in front (Model 2 priority, see above)
    uint32_t color; // bytes r, g, b, a
};
static_assert(sizeof(Vertex) == 32, "attribute offsets below rely on this layout");

// Depth state of a command.
enum class Depth : uint8_t {
    Off,       // no depth test (menus, and the 2D layers when !k2DLayersByDepth)
    TestWrite, // test GL_GEQUAL + write (polygons, foreground layers)
    Test,      // test GL_GEQUAL, no write (background layers)
};

struct Cmd {
    Prog prog;
    GLuint texture;
    uint32_t first, count;    // vertex range in g.verts.data
    Depth depth;
    int clip;                 // scissor rectangle index in g.clips, -1 = none
    bool quads = false;       // indexed quads: first and count are multiples of 4
};

// A polygon waiting in its batch: everything draw_polygons() resolved for it (material and
// light); flush_batches() builds its vertices straight into the frame's GPU vertex memory.
struct PolyRef {
    const rt::GeoPoly *poly; // in Video::gpu_polys(), valid for the whole frame
    float base_x, base_y;    // projection centre in display pixels
    float u_scale, v_scale, palette;
    float depth;             // Model 2 priority depth
    uint32_t color;
};

// Polygons waiting to be drawn, grouped by (clip, program, texture). Model 2 windows need no
// separate pass: the window is part of the priority rank, hence of the depth.
struct Batch {
    int clip;
    Prog prog;
    GLuint texture;
    std::vector<PolyRef> poly_refs; // kept between frames to reuse the allocation
    // Filled by flush_batches(): what was actually written (invalid polygons are dropped).
    unsigned valid_polys = 0;
    size_t valid_vertices = 0;
};

struct FrameStats {
    unsigned polys_in = 0, polys_out = 0;
    unsigned skip_vcount = 0, skip_window = 0, skip_clip = 0, skip_invalid = 0, skip_texture = 0,
             skip_palette = 0, skip_renderer = 0;
    unsigned draw_calls = 0, batches = 0;
    unsigned new_sources = 0, new_palettes = 0, deferred_sources = 0;
    uint32_t new_texels = 0;       // texels decoded into new index textures this frame
    unsigned dropped_vertices = 0; // vertex memory exhausted (should stay 0)
};

// Times (microseconds) summed over one log period (kLogEvery frames).
struct Timing {
    // Renderer, measured here.
    unsigned frames = 0;
    uint64_t interval = 0, interval_max = 0; // gl_end_frame to gl_end_frame = one presented frame
    uint64_t record = 0;                     // GpuGlRenderer::draw (CPU)
    uint64_t rec_upload = 0;                 //   System 24 / layer texture uploads
    uint64_t rec_layers = 0;                 //   System 24 rectangles
    uint64_t rec_polygons = 0;               //   polygons (texture builds included)
    uint64_t rec_textures = 0;               //     of which: building index textures
    uint64_t gpu_wait = 0;                   // gl_begin_frame: GPU still busy with the previous frame
    uint64_t submit = 0;                     // execute(): GL calls
    uint64_t swap = 0;                       // vglSwapBuffers: waits for a free buffer / vsync
    // Main loop, reported by frontend/main.cpp through gl_profile_loop().
    unsigned loops = 0, menu_loops = 0, board_frames = 0;
    uint64_t board = 0, board_core = 0, board_geometry = 0, board_video = 0, board_max = 0;
    uint64_t sound_wait = 0, sound_worker = 0, prepare = 0;
};

// All the vertices of a frame, in GPU-visible memory. The CPU only writes it (never reads
// it back); the GPU draws it in place through the VBO (vglBufferData: no copy). Since
// gl_begin_frame waits for the GPU, the same memory is refilled every frame.
struct VertexArena {
    Vertex *data = nullptr;
    size_t size = 0, capacity = 0;

    Vertex *append(size_t n) {
        if (size + n > capacity && !grow(size + n)) return nullptr;
        Vertex *p = data + size;
        size += n;
        return p;
    }
    bool grow(size_t needed) {
        const size_t cap = std::max<size_t>(capacity ? capacity * 2 : size_t(1) << 16, needed);
        auto *bigger = static_cast<Vertex *>(vglForceAlloc(uint32_t(cap * sizeof(Vertex))));
        if (!bigger) return false;
        if (size) std::memcpy(bigger, data, size * sizeof(Vertex)); // rare
        if (data) vglFree(data); // the GPU is idle (gl_begin_frame) and the VBO is re-pointed in execute()
        data = bigger;
        capacity = cap;
        return true;
    }
    void release() {
        if (data) vglFree(data);
        data = nullptr;
        size = capacity = 0;
    }
};

struct State {
    bool ready = false;
    unsigned frame = 0;
    GLuint program[int(Prog::Count)] = {};
    // The same programs with the hybrid depth (upgrade_z_depth), and whether the
    // frame draws with them (the Vita option, gl_set_upgrade_z_depth).
    GLuint program_z[int(Prog::Count)] = {};
    bool upgrade_z_depth = false;
    GLuint vbo = 0;
    GLuint quad_ibo = 0;        // static index buffer of the indexed quads (0: plain triangles)
    GLuint palette_texture = 0; // bound on unit 1 for the MODEL shaders
    GLuint s24_palette_texture = 0; // bound on unit 2 for the Layer shader (System 24 pens)
    VertexArena verts;
    std::vector<Cmd> cmds;
    std::vector<std::array<int, 4>> clips; // x0, y0, x1, y1 display pixels, top-left origin, exclusive
    int clip = -1;                         // clip of the next pushed vertices
    std::vector<Batch> batches;
    size_t batch_count = 0;
    FlatIndex<uint64_t, uint32_t, 12> batch_index; // (clip, program, texture) -> batch, cleared per frame
    PolygonOrder order;        // Model 2 draw priority (same order as the CPU renderer)
    std::vector<float> depth;  // per polygon depth from that order
    FrameStats stats;
    Timing timing;
    uint64_t record_us = 0, last_end = 0;
    uint64_t diagnostic_us = 0; // last gl_end_frame: gl.log write
    bool skip_worst = false; // the next interval includes the log write
    char error[512] = {};
};
extern State g; // gl_context.cpp

// Append vertices; merged into the previous command when it uses the same state.
inline void push(Prog prog, GLuint texture, const Vertex *vertices, size_t count, Depth depth = Depth::Off) {
    if (!count) return;
    const uint32_t first = uint32_t(g.verts.size);
    Vertex *dst = g.verts.append(count);
    if (!dst) { g.stats.dropped_vertices += unsigned(count); return; }
    std::memcpy(dst, vertices, count * sizeof(Vertex));
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == prog && last.texture == texture && last.depth == depth && last.clip == g.clip) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({prog, texture, first, uint32_t(count), depth, g.clip});
}

// Screen-space quad (w = 1), display pixels, at depth d (only used when depth != Off).
inline void push_quad(Prog prog, GLuint texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
               float v1, uint32_t color, Depth depth = Depth::Off, float d = 0.0f) {
    const Vertex q[6] = {
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y0, 1, u1, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color},
        {x0, y0, 1, u0, v0, 0, d, color}, {x1, y1, 1, u1, v1, 0, d, color}, {x0, y1, 1, u0, v1, 0, d, color},
    };
    push(prog, texture, q, 6, depth);
}

// Index of a scissor rectangle in g.clips (added if new; there are only a few per frame).
inline int add_clip(const std::array<int, 4> &rect) {
    for (size_t i = 0; i < g.clips.size(); ++i)
        if (g.clips[i] == rect) return int(i);
    g.clips.push_back(rect);
    return int(g.clips.size() - 1);
}

// Index in g.batches of the polygon batch for this state, created on first use this frame.
inline uint32_t batch_for(int clip, Prog prog, GLuint texture) {
    const uint64_t key = uint64_t(clip & 0xffff) << 40 | uint64_t(prog) << 32 | texture;
    if (const uint32_t *found = g.batch_index.find(key)) return *found;
    if (g.batch_count == g.batches.size()) g.batches.emplace_back();
    Batch &b = g.batches[g.batch_count];
    b.clip = clip; b.prog = prog; b.texture = texture;
    b.poly_refs.clear();
    g.batch_index.insert(key, uint32_t(g.batch_count)); // full table: a second batch, same image
    return uint32_t(g.batch_count++);
}

// Vertex slots for `quads` indexed quads, starting on a multiple of 4 (the index buffer is
// built for groups of 4); nullptr when they would pass the 16-bit index limit (the caller
// then writes plain triangles) or memory is exhausted. The padding vertices are never drawn.
inline Vertex *append_quads(size_t quads) {
    if (!g.quad_ibo || !quads) return nullptr;
    const size_t pad = (4 - g.verts.size % 4) % 4;
    if (g.verts.size + pad + quads * 4 > kIndexedVertexLimit) return nullptr;
    if (!g.verts.append(pad + quads * 4)) return nullptr;
    return g.verts.data + (g.verts.size - quads * 4);
}

// Fan (v0, vi, vi+1) of a convex n-gon as quads (v0, v1+2k, v2+2k, v3+2k): the two fan
// triangles 1+2k and 2+2k. Odd fans end with (v0, vn-2, vn-1, vn-1), whose second triangle
// is degenerate. Returns the vertices written (4 per quad).
inline size_t write_fan_quads(Vertex *dst, const Vertex *v, int n) {
    size_t written = 0;
    for (int a = 1; a + 1 < n; a += 2) {
        *dst++ = v[0];
        *dst++ = v[a];
        *dst++ = v[a + 1];
        *dst++ = v[std::min(a + 2, n - 1)];
        written += 4;
    }
    return written;
}

// Draw list execution (draw_list.cpp).
void flush_batches(); // the polygon batches into the draw list
void execute();       // runs the draw list

inline uint16_t le16(const uint8_t *base, uint32_t index) { return uint16_t(base[index * 2] | base[index * 2 + 1] << 8); }
inline uint32_t argb_to_rgba(uint32_t argb) { return (argb & 0xff00ff00u) | ((argb >> 16) & 0xffu) | ((argb & 0xffu) << 16); }

} // namespace vita::gldraw
