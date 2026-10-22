// vitaGL renderer: Model 2 polygons, in priority order (render/renderer.cpp).

#include "render/gl_internal.h"
#include "games/game.h"
#include <cmath>

namespace vita {
using namespace gldraw;

// ---- Renderer: Model 2 polygons --------------------------------------------------------
void GpuGlRenderer::draw_polygons(rt::Video &video) {
    const auto &polys = video.gpu_polys();
    const rt::VideoMem &mem = video.gpu_mem();
    FrameStats &st = g.stats;
    st.polys_in = unsigned(polys.size());
    material_drops_ = material_builds_ = material_defers_ = 0;
    textured_polys_ = solid_polys_ = checker_polys_ = textured_checker_polys_ = 0;
    submitted_vertices_ = 0;
    g.batch_count = 0;
    g.batch_index.clear();
    // Palette texture almost full: start it again (gl_begin_frame waited for the GPU, so no
    // frame in flight still reads it). Rows are cheap to rebuild.
    if (palette_used_ > kPaletteRows - kPaletteFrameRows) {
        palette_index_.clear();
        palette_used_ = 0;
        ++cache_resets_;
    }
    // The rows' colours come from palette, colour translation and luma RAM: when the game
    // rewrites them (fades), every row is stale. Rebuilt on use, like after a reset.
    if (const uint64_t generation = video.colour_generation(); generation != colour_generation_) {
        colour_generation_ = generation;
        palette_index_.clear();
        palette_used_ = 0;
    }
    // Texture RAM rewritten (Virtua Fighter 2 loads each fight's characters there): the
    // index textures decoded from those pages are checked at their next use.
    mark_rewritten_sources(mem);

    // Per-polygon state, cached on two levels and reused while the following polygons match:
    //   * Material: everything that depends on the header (texheader, viewport, window):
    //     clip, shader, texture, batch, UV scale. Neighbouring polygons nearly always belong
    //     to the same object and material.
    //   * Light: what depends on the polygon luma: the palette row (textured) or the flat
    //     colour. The geometrizer lights each face (normal . light), so the luma changes
    //     from one polygon to the next; only this part is redone when it is the only change.
    enum class Skip : uint8_t { None, Window, Clip, Texture, Renderer };

    struct Material {
        bool cached = false;
        // key
        uint16_t th[4] = {};
        int viewport[4] = {}, window = 0;
        // result
        Skip skip = Skip::None;
        bool textured = false, checker = false;
        int clip = -1;
        Prog prog = Prog::Flat;
        GLuint texture = 0;
        bool has_batch = false; // batch created when the first polygon gets a palette row
        uint32_t batch = 0;
        float u_scale = 0, v_scale = 0;
    } mat;
    struct Light {
        bool cached = false;
        uint8_t key = 0;     // the luma bits the result depends on (see luma_key)
        bool ok = false;     // false: palette texture full, polygon skipped
        float palette = 0;
        uint32_t color = 0xffffffffu;
    } light;

    const int windows = video.gpu_windows(), rx = video.render_x(), ry = video.render_y();
    const int crtc_x = video.crtc_x(), crtc_y = video.crtc_y();

    auto same_material = [&](const rt::GeoPoly &p) {
        return mat.cached && p.texheader[0] == mat.th[0] && p.texheader[1] == mat.th[1] &&
               p.texheader[2] == mat.th[2] && p.texheader[3] == mat.th[3] && p.window == mat.window &&
               p.viewport[0] == mat.viewport[0] && p.viewport[1] == mat.viewport[1] &&
               p.viewport[2] == mat.viewport[2] && p.viewport[3] == mat.viewport[3];
    };
    auto set_material = [&](const rt::GeoPoly &p) {
        mat = Material{};
        light.cached = false; // the palette also depends on texheader (luma base, colour)
        mat.cached = true;
        for (int i = 0; i < 4; ++i) { mat.th[i] = p.texheader[i]; mat.viewport[i] = p.viewport[i]; }
        mat.window = p.window;
        if (p.window > windows) { mat.skip = Skip::Window; return; }

        // Clip rectangle (the polygon's viewport), as a scissor in display pixels.
        const int l = std::max<int>(p.viewport[0] + rx, 0);
        const int r = std::min<int>(p.viewport[2] + rx, rt::Video::W - 1);
        const int t = std::max<int>(384 - p.viewport[3] + ry, 0);
        const int b = std::min<int>(384 - p.viewport[1] + ry, rt::Video::H - 1);
        if (l > r || t > b) { mat.skip = Skip::Clip; return; }
        mat.clip = add_clip({int(sx(float(l))), int(sy(float(t))), int(sx(float(r + 1))), int(sy(float(b + 1)))});

        // Shader and texture.
        const int renderer = (p.texheader[0] >> 13) & 3; // 0 flat, 1 unsupported, 2-3 textured
        mat.checker = (p.texheader[0] & 0x8000) != 0;
        if (renderer & 2) {
            const Source *src = source_for(p, mem);
            if (!src) { mat.skip = Skip::Texture; return; }
            mat.prog = mat.checker ? Prog::ModelChecker : src->transparent ? Prog::ModelAlpha : Prog::Model;
            mat.texture = src->texture;
            mat.textured = true;
            mat.u_scale = 1.0f / (8.0f * float(src->source_w)); // Model 2 texel units are x8
            mat.v_scale = 1.0f / (8.0f * float(src->source_h));
        } else if (renderer == 0) {
            mat.prog = mat.checker ? Prog::FlatChecker : Prog::Flat;
        } else {
            mat.skip = Skip::Renderer;
        }
    };
    // Luma bits that change the light result: the palette key keeps luma & kLumaMask
    // (palette_row), the flat colour uses luma >> 2 (solid_color).
    auto luma_key = [&](const rt::GeoPoly &p) -> uint8_t {
        if (mat.textured) return kLumaMask ? uint8_t(p.luma & kLumaMask) : uint8_t(0);
        return uint8_t(p.luma & 0xfc);
    };
    auto set_light = [&](const rt::GeoPoly &p) {
        light = Light{};
        light.cached = true;
        light.key = luma_key(p);
        if (mat.textured) {
            const int row = palette_row(p, mem);
            if (row < 0) return; // light.ok stays false
            light.palette = (float(row) + 0.5f) / float(kPaletteRows);
        } else {
            light.color = solid_color(p, mem);
        }
        light.ok = true;
    };

    // Model 2 draw priority -> one depth per polygon: the geometry thread's, computed
    // with the list (render/polygon_order.h polygon_depth), else here.
    const uint64_t sort_begin = now_us();
    const float *depth = rt::Geo::depth_of(polys, g.upgrade_z_depth);
    if (!depth) {
        polygon_depth(polys, g.upgrade_z_depth, g.order, g.depth);
        depth = g.depth.data();
    }
    last_sort_us_ = now_us() - sort_begin;

    for (size_t pi = 0; pi < polys.size(); ++pi) {
        const rt::GeoPoly &poly = polys[pi];
        const int n = poly.num_vertices;
        if (n < 3 || n > 8) { ++st.skip_vcount; continue; }

        // 1. Material: clip, shader, texture (reused from the previous polygon if possible).
        if (!same_material(poly)) set_material(poly);
        switch (mat.skip) {
        case Skip::None: break;
        case Skip::Window: ++st.skip_window; continue;
        case Skip::Clip: ++st.skip_clip; continue;
        case Skip::Texture: ++st.skip_texture; continue;
        case Skip::Renderer: ++st.skip_renderer; continue;
        }
        // 2. Light: palette row or flat colour, redone only when the useful luma bits change.
        if (!light.cached || luma_key(poly) != light.key) set_light(poly);
        if (!light.ok) { ++st.skip_palette; continue; }
        // 3. Batch: created at the first polygon that gets this far. Batches mix Model 2
        //    windows: the window is part of the priority rank, hence of the depth.
        if (!mat.has_batch) {
            mat.batch = batch_for(mat.clip, mat.prog, mat.texture);
            mat.has_batch = true;
        }

        // 4. Reference only: the vertices are built by flush_batches(), straight into the
        //    frame's GPU vertex memory. x * w = (crtc_x + center_x) * w + vx (GPU divides by w).
        const float base_x = sx(float(crtc_x + poly.center[0]));
        const float base_y = sy(float(384 - poly.center[1] + crtc_y));
        g.batches[mat.batch].poly_refs.push_back({
            &poly, base_x, base_y, mat.u_scale, mat.v_scale, light.palette, depth[pi], light.color
        });
    }
    clip_changes_ = unsigned(g.clips.size());
    const uint64_t vertex_begin = now_us();
    flush_batches();
    last_vertex_us_ = now_us() - vertex_begin;

    // Polygon counters, from what flush_batches() actually wrote (it drops invalid polygons).
    for (size_t i = 0; i < g.batch_count; ++i) {
        const Batch &b = g.batches[i];
        st.polys_out += b.valid_polys;
        submitted_vertices_ += b.valid_vertices;
        const bool checker = b.prog == Prog::FlatChecker || b.prog == Prog::ModelChecker;
        if (textured(b.prog)) {
            textured_polys_ += b.valid_polys;
            if (checker) textured_checker_polys_ += b.valid_polys;
        } else {
            solid_polys_ += b.valid_polys;
            if (checker) checker_polys_ += b.valid_polys;
        }
    }
}

} // namespace vita
