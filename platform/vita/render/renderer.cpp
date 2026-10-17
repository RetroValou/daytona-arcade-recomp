// vitaGL renderer for the Vita GPU path.
//
// One frame:
//   gl_begin_frame()       clear the screen and the depth buffer, empty the draw list
//   GpuGlRenderer::draw()  CPU only: fills the draw list (vertices + commands)
//   gl_end_frame()         uploads all the vertices to one VBO, runs the commands, swaps
//
// Polygons
//   * Visibility follows the Model 2 draw priority, as in the CPU reference renderer:
//     higher window first, then smaller z sort key (GeoPoly::z, float_to_zval of the
//     geometrizer), then newest polygon first; the first polygon to cover a pixel wins.
//     Each polygon gets a unique depth from its rank in that order, so the depth buffer
//     reproduces it whatever the submission order: polygons are grouped by (clip
//     rectangle, shader, texture) into one draw call per group.
//   * Projection is done by the GPU. Each vertex is sent in homogeneous form
//     (x * w, y * w, w), w being the vertex distance (pz). The GPU divides by w and
//     interpolates the texture coordinates with perspective correction on its own.
//   * Textures work like the real board: the GPU filters the 4-bit texel INDEX, then the
//     fragment shader looks its colour up in the polygon's palette (luma table + colour +
//     polygon luma). Texel indices are uploaded once per texture region; a palette is one
//     128-texel row of a shared palette texture, selected per vertex.
//   * Transparency: no blending, like the real board. Texel 15 can be transparent
//     (alpha test) and the Model 2 "checker" flag keeps one native pixel out of two.
//
// 2D layers (System 24 tilemaps) are alpha-tested textured quads placed by the depth buffer:
// foreground before the polygons, background after them (see k2DLayersByDepth). Their
// textures hold pen NUMBERS; the Layer shader looks the colour up in a 128x64 palette
// texture (unit 2). A palette change (fades, flashes) rewrites that 32 KB texture only,
// where colour textures had to rewrite all 16384 tiles (~67 ms on the Vita).
// The 2D is shown one frame late, like the 3D of the pipelined geometrizer, so both are
// in phase: two sets of layer textures; while the frame shows the set prepared during the
// previous frame, a worker thread on core 2 (the sound core, the least loaded) uploads
// this frame's tiles and palette into the other set and computes its layer rectangles.
//
// GPU memory written in place: gl_begin_frame() waits for the GPU (cheap: the previous
// frame finished long ago while the CPU emulated). After that, the frame's vertices and the
// changed texture pixels are written STRAIGHT into GPU-visible memory. glTexSubImage2D is avoided on purpose: on a texture
// used in the last frames, vitaGL first allocates a new copy and copies the old pixels
// back from GPU memory, which the CPU reads very slowly (~30 ms/frame for the System 24
// layers in the race).

// Files: renderer.cpp (setup, frame entry), model2_textures.cpp, model2_polygons.cpp,
// layers_2d.cpp (the renderer class); gl_context.cpp (vitaGL, the frame API, gl.log),
// shaders.cpp, draw_list.cpp, ui_draw.cpp (the menu's triangles); gl_internal.h (shared).

#include "render/gl_internal.h"
#include "render/system24_upload.h"

#include <vector>

namespace vita {
using namespace gldraw;

GpuGlRenderer::GpuGlRenderer() {
    for (int i = 0; i < 256; ++i) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
    bool ok = true;
    for (S24Slot &slot : s24_) {
        for (size_t i = 0; i < slot.textures.size(); ++i) {
            // Point sampled and repeating: scroll offsets add up to 511 texels to the coordinates.
            slot.textures[i] = make_texture(512, 512, false, GL_REPEAT, GL_REPEAT, GL_RGBA, nullptr);
            slot.texels[i] = texture_memory(slot.textures[i]);
            ok = ok && slot.textures[i] && slot.texels[i];
        }
        // System 24 palette: 8192 pens, point sampled (system24_upload.h).
        slot.palette_texture = make_texture(kSystem24PaletteWidth, kSystem24PaletteHeight, false, GL_CLAMP_TO_EDGE,
                                            GL_CLAMP_TO_EDGE, GL_RGBA, nullptr);
        slot.palette = static_cast<uint32_t *>(texture_memory(slot.palette_texture));
        ok = ok && slot.palette_texture && slot.palette;
        // The worker should not allocate: room for the usual frames (more is allocated if needed).
        slot.rects.reserve(4096);
        slot.runs.reserve(64);
        for (auto &split : slot.split) split.reserve(1024);
    }
    // Palette texture: point sampled, one row per palette.
    palette_texture_ = make_texture(kPaletteWidth, kPaletteRows, false, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_RGBA, nullptr);
    palette_data_ = static_cast<uint32_t *>(texture_memory(palette_texture_));
    g.palette_texture = palette_texture_;
    ok = ok && palette_texture_ && palette_data_;
    sources_.reserve(1024);
    ok_ = g.ready && ok;
    if (ok_ && !s24_start_thread()) s24_stop_thread(); // 2D prepared inline on the main core
}

GpuGlRenderer::~GpuGlRenderer() { s24_stop_thread(); }

int GpuGlRenderer::worker_2d_core() { return kWorker2DCore; }

uint32_t GpuGlRenderer::make_texture(uint32_t w, uint32_t h, bool linear, uint32_t wrap_s, uint32_t wrap_t,
                                     uint32_t format, const void *pixels) {
    std::vector<uint8_t> zero;
    if (!pixels) {
        zero.assign(size_t(w) * h * 4u, 0);
        pixels = zero.data();
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(wrap_s));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(wrap_t));
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(format), GLsizei(w), GLsizei(h), 0, GLenum(format), GL_UNSIGNED_BYTE, pixels);
    return texture;
}

// vitaGL textures are linear: rows of VGL_ALIGN(width, 8) pixels. The pointer stays valid
// as long as glTexImage2D / glTexSubImage2D are not called again on the texture.
void *GpuGlRenderer::texture_memory(uint32_t texture) {
    if (!texture) return nullptr;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    return vglGetTexDataPointer(GL_TEXTURE_2D);
}

void GpuGlRenderer::clear_cache() {
    for (auto &entry : sources_) {
        GLuint texture = entry.second.texture;
        if (texture) glDeleteTextures(1, &texture);
    }
    sources_.clear();
    source_cache_.clear();
    cached_bytes_ = 0;
    cache_reset_pending_ = false;
    palette_index_.clear();
    palette_used_ = 0;
}

void GpuGlRenderer::reset_materials() {
    s24_join();
    glFinish();
    clear_cache();
    for (S24Slot &slot : s24_) {
        slot.valid = false; // nothing shown until a frame of the new state was prepared
        slot.generation = slot.palette_generation = UINT64_MAX;
    }
}

void GpuGlRenderer::prepare_frame() {
    // (gl_begin_frame waits for the GPU before anything is rewritten in GPU memory.)
    if (cache_reset_pending_) {
        clear_cache();
        ++cache_resets_;
    }
}

void GpuGlRenderer::shutdown() {
    if (shutdown_) return;
    glFinish();
    clear_cache();
    if (palette_texture_) {
        GLuint texture = palette_texture_;
        glDeleteTextures(1, &texture);
    }
    palette_texture_ = 0;
    palette_data_ = nullptr;
    g.palette_texture = 0;
    s24_stop_thread();
    g.s24_palette_texture = 0;
    for (S24Slot &slot : s24_) {
        for (uint32_t &texture : slot.textures) {
            GLuint t = texture;
            if (t) glDeleteTextures(1, &t);
            texture = 0;
        }
        GLuint t = slot.palette_texture;
        if (t) glDeleteTextures(1, &t);
        slot.palette_texture = 0;
        slot.palette = nullptr;
        slot.texels.fill(nullptr);
        slot.valid = false;
    }
    shutdown_ = true;
    ok_ = false;
}

// ---- Renderer: frame entry points ------------------------------------------------------
void GpuGlRenderer::draw(rt::Video &video) {
    const uint64_t begin = now_us();
    last_polygon_us_ = last_tile_us_ = last_upload_us_ = last_sort_us_ = last_texture_us_ = 0;
    // The Vita GPU always draws the System 24 layers itself (Video::system24_gpu_compatible()
    // is unconditionally true there): background tiles, Model 2 polygons, foreground tiles.
    // With k2DLayersByDepth the foreground goes first and the background last; the depth
    // buffer keeps the same image (see k2DLayersByDepth).
    // The polygons drawn here are the previous frame's (pipelined geometrizer), so the 2D
    // shown is the previous frame's too (the front slot) while the worker prepares this
    // frame's into the back slot, in parallel with the recording below.
    const S24Slot &front = s24_[s24_back_ ^ 1u];
    s24_kick(video);
    const uint64_t t0 = now_us();
    system24_quads_ = 0;
    g.s24_palette_texture = front.palette_texture;
    if (front.valid) s24_emit(front, k2DLayersByDepth); // first pass: foreground (by depth) or background
    const uint64_t t1 = now_us();
    draw_polygons(video);
    const uint64_t t2 = now_us();
    if (front.valid) s24_emit(front, !k2DLayersByDepth); // last pass: background (by depth) or foreground
    const uint64_t t3 = now_us();
    s24_join(); // the board must not run while the worker reads Video
    const uint64_t t4 = now_us();
    system24_uploaded_tiles_ = s24_[s24_back_].uploaded_tiles;
    s24_back_ ^= 1u;
    last_2d_worker_us_ = s24_job_us_;
    last_upload_us_ = (t0 - begin) + (t4 - t3); // inline job (no thread) or waiting for the worker
    last_polygon_us_ = t2 - t1;
    last_tile_us_ = (t1 - t0) + (t3 - t2);
    g.record_us += t4 - begin;
    g.timing.rec_upload += last_upload_us_;
    g.timing.rec_layers += last_tile_us_;
    g.timing.rec_polygons += last_polygon_us_;
    last_gpu_ms_ = double(t4 - begin) / 1000.0;
}

} // namespace vita
