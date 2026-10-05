#pragma once

// vitaGL backend for the Vita GPU path (build option DAYTONA_VITA_GPU_GL).
// Alternative to gpu_fast.cpp (libvita2d); both initialise sceGxm, so only one can be linked.
// GpuGlRenderer mirrors GpuFastRenderer's public interface so main_gpu.cpp needs few #ifdefs.

#include "runtime/video.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

// vitaGL.h does not include the sceGxm headers it needs: they must come first.
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

// Same packing as libvita2d's RGBA8: bytes in memory are r, g, b, a.
#ifndef RGBA8
#define RGBA8(r, g, b, a) ((((a) & 0xFF) << 24) | (((b) & 0xFF) << 16) | (((g) & 0xFF) << 8) | (((r) & 0xFF) << 0))
#endif

namespace vita {

// ---- Frame level API (replaces the vita2d_* calls in main_gpu.cpp) ----------
bool gl_init();              // init vitaGL + shaders; false on failure (see gl_error())
const char *gl_error();      // last init error, empty when none
void gl_begin_frame();       // clear the screen and the recorded draw list
void gl_end_frame();         // draw everything recorded since gl_begin_frame() and swap
void gl_fini();              // release shaders/buffers
void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba); // display pixels (960x544)

// What the main loop did during one iteration (one presented frame), in microseconds.
// main_gpu.cpp fills it and calls gl_profile_loop() once per iteration; gl.log prints
// averages next to the renderer's own timings, so a whole frame is accounted for.
struct GlLoopProfile {
    bool menu = false;
    unsigned board_frames = 0;   // emulated Model 2 frames (normally 0 or 1 per present)
    uint64_t board = 0;          // whole emulated frame (GameLoop::run_frame_sound_packet)
    uint64_t board_core = 0;     //   i960 main CPU + synchronous TGP + scheduling
    uint64_t board_geometry = 0; //   vblank start: geometry, polygon list for the GPU
    uint64_t board_video = 0;    //   vblank end: software video (System 24 layers, CPU raster)
    uint64_t sound_wait = 0;     // main thread blocked waiting for the sound worker
    uint64_t sound_worker = 0;   // sound board time on the worker thread (runs in parallel)
    uint64_t prepare = 0;        // GpuGlRenderer::prepare_frame (texture cache resets)
};
void gl_profile_loop(const GlLoopProfile &profile);

class GpuGlRenderer {
public:
    GpuGlRenderer();
    ~GpuGlRenderer() = default; // main_gpu calls shutdown() before gl_fini()
    GpuGlRenderer(const GpuGlRenderer &) = delete;
    GpuGlRenderer &operator=(const GpuGlRenderer &) = delete;

    bool ok() const { return ok_; }
    void reset_materials();      // drop every cached texture and palette (mode change, new game)
    void prepare_frame();        // call before gl_begin_frame(), also for menus
    void shutdown();
    void draw(rt::Video &video);       // System 24 layers + Model 2 polygons (the only Vita GPU case)

    // ---- Diagnostics read by main_gpu.cpp (same names as GpuFastRenderer) ----
    double last_gpu_ms() const { return last_gpu_ms_; }
    uint64_t last_sort_us() const { return last_sort_us_; } // priority sort feeding the depth ranks
    uint64_t last_texture_us() const { return last_texture_us_; } // index texture builds (inside polygons)
    uint64_t last_polygon_us() const { return last_polygon_us_; }
    uint64_t last_tile_us() const { return last_tile_us_; }
    uint64_t last_upload_us() const { return last_upload_us_; }
    std::size_t cached_bytes() const { return cached_bytes_; }
    std::size_t cached_materials() const { return palette_index_.size(); } // palette rows
    std::size_t cached_sources() const { return sources_.size(); }         // index textures
    std::size_t reserved_bytes() const { return 0; }
    unsigned cache_resets() const { return cache_resets_; }
    unsigned material_drops() const { return material_drops_; }
    unsigned material_builds() const { return material_builds_; }
    unsigned material_defers() const { return material_defers_; }
    std::size_t submitted_vertices() const { return submitted_vertices_; }
    unsigned system24_quads() const { return system24_quads_; }
    unsigned system24_uploaded_tiles() const { return system24_uploaded_tiles_; }
    unsigned textured_polys() const { return textured_polys_; }
    unsigned solid_polys() const { return solid_polys_; }
    unsigned checker_polys() const { return checker_polys_; }
    unsigned textured_checker_polys() const { return textured_checker_polys_; }
    unsigned clip_changes() const { return clip_changes_; }
    // Not tracked by this backend.
    unsigned pool_drops() const { return 0; }
    unsigned subdivided_polys() const { return 0; }
    unsigned min_pool_free() const { return 0; }
    unsigned textured_draws() const { return 0; }
    unsigned solid_draws() const { return 0; }
    unsigned shader_setups() const { return 0; }
    unsigned state_reuses() const { return 0; }
    unsigned draw_errors() const { return 0; }

private:
    // Model 2 textures are 4-bit texel indices; the colour comes from a palette chosen per
    // polygon. As on the real board, the GPU filters the INDEX, then looks the colour up:
    //   * Source  = the indices of one texture region, as a GL_LUMINANCE_ALPHA texture
    //               (L = index * 16, A = 0 for a transparent texel). Shared by all palettes.
    //   * Palette = one 128-entry row of the palette texture per (luma base, colour, luma):
    //               entry k = colour of luminance step k (index * 8 + filtered fraction).
    struct SourceKey {
        uint16_t h0 = 0, h2 = 0; // texheader[0] size/mirror/transparent, texheader[2] position
        bool operator==(const SourceKey &o) const { return h0 == o.h0 && h2 == o.h2; }
    };
    struct SourceKeyHash {
        std::size_t operator()(const SourceKey &k) const { return std::hash<uint32_t>()(k.h0 | uint32_t(k.h2) << 16); }
    };
    struct Source {
        uint32_t texture = 0;                // GLuint
        uint32_t source_w = 1, source_h = 1; // size in Model 2 texels
        bool transparent = false;            // texel 15 is see-through: alpha-test shader
        std::size_t bytes = 0;
    };

    static constexpr std::size_t kSourceCacheBytes = 24u * 1024u * 1024u;
    static constexpr unsigned kSourceBuildBudget = 32; // new index textures per frame
    static constexpr uint32_t kTextureLimit = 512;     // max GL texture side
    static constexpr uint32_t kPaletteWidth = 128;     // luminance steps per palette
    static constexpr uint32_t kPaletteRows = 2048;     // palettes in the palette texture
    static constexpr uint32_t kPaletteFrameRows = 512; // rows kept free for one frame

    // Textures
    uint32_t make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t, uint32_t format,
                          const void *pixels);
    static void *texture_memory(uint32_t texture); // CPU pointer to a texture's pixels (linear rows)
    const Source *source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    int palette_row(const rt::GeoPoly &poly, const rt::VideoMem &mem);
    uint32_t solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const;
    void clear_cache();
    // 2D layers
    void update_system24_textures(const rt::Video &video);
    void draw_system24(const rt::Video &video, bool foreground);
    // Polygons
    void draw_polygons(rt::Video &video);

    bool ok_ = false, shutdown_ = false;
    std::array<uint32_t, 8> system24_textures_{}; // 0-3 background layers, 4-7 foreground layers
    uint64_t system24_generation_ = UINT64_MAX;
    std::unordered_map<SourceKey, Source, SourceKeyHash> sources_;
    std::vector<uint8_t> index_scratch_;
    std::size_t cached_bytes_ = 0;
    bool cache_reset_pending_ = false;
    // Palettes: rows of the palette texture, filled in order of first use.
    uint32_t palette_texture_ = 0;     // GLuint, kPaletteWidth x kPaletteRows RGBA
    uint32_t *palette_data_ = nullptr; // its pixels, written directly
    std::unordered_map<uint32_t, uint32_t> palette_index_; // palette key -> row
    uint32_t palette_used_ = 0;        // rows
    uint8_t gamma_[256]{};

    // Diagnostics
    double last_gpu_ms_ = 0.0;
    uint64_t last_polygon_us_ = 0, last_tile_us_ = 0, last_upload_us_ = 0, last_sort_us_ = 0, last_texture_us_ = 0;
    unsigned cache_resets_ = 0, material_drops_ = 0, material_builds_ = 0, material_defers_ = 0,
             system24_quads_ = 0, system24_uploaded_tiles_ = 0, clip_changes_ = 0, textured_polys_ = 0,
             solid_polys_ = 0, checker_polys_ = 0, textured_checker_polys_ = 0;
    std::size_t submitted_vertices_ = 0;
};

} // namespace vita