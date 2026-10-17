// vitaGL renderer: the System 24 layers and their worker thread (render/renderer.cpp).

#include "render/gl_internal.h"
#include "render/system24_upload.h"
#include "system/core_policy.h"
#include <psp2/kernel/threadmgr.h>

namespace vita {
using namespace gldraw;

// ---- Renderer: 2D layers -----------------------------------------------------------------

// Worker job (core 2, or inline without the thread): no GL call, reads only Video's
// System 24 state, writes only the slot (its texture pixels: the GPU is done with them).
void GpuGlRenderer::s24_prepare(S24Slot &slot, const rt::Video &video) {
    slot.uploaded_tiles = 0;
    // Palette: 8192 pens, rewritten when a pen changed since this slot's last frame.
    if (slot.palette_generation == UINT64_MAX || slot.palette_generation != video.system24_palette_generation()) {
        upload_system24_palette(video, slot.palette, kSystem24PaletteWidth);
        slot.palette_generation = video.system24_palette_generation();
    }
    // Tiles whose pixels or categories changed since this slot's last frame (two frames
    // ago: tile generations only grow), straight into the textures, as pen numbers.
    if (slot.generation != video.system24_texture_generation()) {
        constexpr size_t kStride = 512u * sizeof(uint32_t);
        for (int layer = 0; layer < 4; ++layer)
            slot.uploaded_tiles += upload_system24_layer_indices(video, layer, slot.generation,
                                                                 slot.texels[size_t(layer)], kStride,
                                                                 slot.texels[size_t(layer + 4)], kStride);
        slot.generation = video.system24_texture_generation();
    }
    slot.backdrop = argb_to_rgba(video.system24_pen(0));

    // Layer rectangles, the same for the background and foreground passes (the
    // layer/window/split logic of the removed GpuFastRenderer::draw_system24).
    using Rect = S24Rect;
    auto &rects = slot.rects;
    auto &runs = slot.runs;
    rects.clear();
    runs.clear();
    auto add_run = [&](int layer, const std::vector<Rect> &list) {
        if (list.empty()) return;
        runs.push_back({layer, uint32_t(rects.size()), uint32_t(list.size())});
        rects.insert(rects.end(), list.begin(), list.end());
    };
    for (int layer = 3; layer >= 0; --layer) {
        const uint16_t hreg = video.system24_word(0x5000u + unsigned(layer));
        const uint16_t vreg = video.system24_word(0x5004u + unsigned(layer));
        if (vreg & 0x8000) continue; // layer disabled
        const uint16_t ctrl = video.system24_word(0x5004u + unsigned(layer & 2));
        const int split_mode = (ctrl >> 13) & 3;
        const bool line_scroll = (hreg & 0x8000) != 0;
        const uint32_t line_base = 0x4000u + 0x200u * unsigned(layer);

        if (split_mode) {
            // The even layer draws itself and the following odd layer; the odd one is skipped.
            if (layer & 1) continue;
            auto &split_rects = slot.split;
            split_rects[0].clear();
            split_rects[1].clear();
            const int source_y = vreg & 511;
            auto add = [&](int source_layer, int x0, int x1, int y0, int y1, int scroll) {
                x0 = std::clamp(x0, 0, rt::Video::W);
                x1 = std::clamp(x1, 0, rt::Video::W);
                y0 = std::clamp(y0, 0, rt::Video::H);
                y1 = std::clamp(y1, 0, rt::Video::H);
                if (x0 < x1 && y0 < y1) split_rects[size_t(source_layer - layer)].push_back({x0, x1, y0, y1, scroll, source_y + y0});
            };
            if (split_mode == 1) { // horizontal split line
                const int neg_v = (-int(vreg)) & 0x3ff;
                const int cut = neg_v & 511;
                int first_layer = layer;
                if (!(neg_v & 0x200)) first_layer ^= 1;
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H;) {
                        const int scroll = (-int(video.system24_word(line_base + unsigned(y)))) & 511;
                        const int source_layer = y >= cut ? first_layer ^ 1 : first_layer;
                        int y1 = y + 1;
                        while (y1 < rt::Video::H &&
                               ((-int(video.system24_word(line_base + unsigned(y1)))) & 511) == scroll &&
                               (y1 >= cut ? first_layer ^ 1 : first_layer) == source_layer)
                            ++y1;
                        add(source_layer, 0, rt::Video::W, y, y1, scroll);
                        y = y1;
                    }
                } else {
                    const int scroll = (-int(hreg)) & 511;
                    add(first_layer, 0, rt::Video::W, 0, cut, scroll);
                    add(first_layer ^ 1, 0, rt::Video::W, cut, rt::Video::H, scroll);
                }
            } else { // vertical split column
                if (line_scroll) {
                    for (int y = 0; y < rt::Video::H; ++y) {
                        const int raw = video.system24_word(line_base + unsigned(y));
                        const int cut = raw & 511;
                        const int first_layer = (raw & 0x200) ? layer : layer ^ 1;
                        const int scroll = (-cut) & 511;
                        add(first_layer, 0, cut, y, y + 1, scroll);
                        add(first_layer ^ 1, cut, rt::Video::W, y, y + 1, scroll);
                    }
                } else {
                    const int cut = hreg & 511;
                    const int first_layer = (hreg & 0x200) ? layer : layer ^ 1;
                    const int scroll = (-cut) & 511;
                    add(first_layer, 0, cut, 0, rt::Video::H, scroll);
                    add(first_layer ^ 1, cut, rt::Video::W, 0, rt::Video::H, scroll);
                }
            }
            add_run(layer, split_rects[0]);
            add_run(layer + 1, split_rects[1]);
            continue;
        }

        // Normal mode: each line has a 4x16-bit mask of visible 8-pixel blocks ("windows").
        // Lines with the same scroll and mask are merged into one rectangle per visible run.
        const bool win = (layer & 1) != 0;
        const uint32_t mask_base = layer >= 2 ? 0x6800u : 0x6000u;
        auto line_scroll_of = [&](int y) {
            return line_scroll ? ((-int(video.system24_word(line_base + unsigned(y)))) & 511) : ((-int(hreg)) & 511);
        };
        const uint32_t run_first = uint32_t(rects.size());
        for (int y = 0; y < rt::Video::H;) {
            const int h = line_scroll_of(y);
            std::array<uint16_t, 4> masks{};
            for (int w = 0; w < 4; ++w) masks[size_t(w)] = video.system24_word(mask_base + unsigned(y * 4 + w));
            int y1 = y + 1;
            for (; y1 < rt::Video::H && line_scroll_of(y1) == h; ++y1) {
                bool same = true;
                for (int w = 0; same && w < 4; ++w) same = video.system24_word(mask_base + unsigned(y1 * 4 + w)) == masks[size_t(w)];
                if (!same) break;
            }
            auto visible = [&](int block) { return ((masks[size_t(block / 16)] & (0x8000u >> (block & 15))) != 0) == win; };
            for (int block = 0; block * 8 < rt::Video::W;) {
                while (block * 8 < rt::Video::W && !visible(block)) ++block;
                const int first = block;
                while (block * 8 < rt::Video::W && visible(block)) ++block;
                if (first < block) rects.push_back({first * 8, std::min(block * 8, rt::Video::W), y, y1, h, (vreg & 511) + y});
            }
            y = y1;
        }
        if (rects.size() > run_first) runs.push_back({layer, run_first, uint32_t(rects.size()) - run_first});
    }
    slot.valid = true;
}

int GpuGlRenderer::s24_thread_entry(SceSize, void *argp) {
    GpuGlRenderer *self = *static_cast<GpuGlRenderer **>(argp);
    sceKernelSignalSema(self->s24_done_, 1); // started
    int applied_core_mask = 0;
    for (;;) {
        sceKernelWaitSema(self->s24_start_, 1, nullptr);
        if (self->s24_stop_.load()) break;
        if (kWorker2DCore < 0) apply_core_policy(applied_core_mask); // --free-core: the fourth-core policy
        const uint64_t begin = now_us();
        S24Slot &slot = *self->s24_job_;
        try {
            s24_prepare(slot, *self->s24_video_);
        } catch (...) { // out of memory: this frame shows no 2D, the next one uploads everything
            slot.valid = false;
            slot.generation = slot.palette_generation = UINT64_MAX;
        }
        self->s24_job_us_ = now_us() - begin;
#if defined(__arm__)
        // The texels go to GPU-visible memory: drain this core's writes before the main core submits.
        __asm__ volatile("dsb" ::: "memory");
#endif
        sceKernelSignalSema(self->s24_done_, 1);
    }
    return 0;
}

bool GpuGlRenderer::s24_start_thread() {
    s24_stop_.store(false);
    s24_start_ = sceKernelCreateSema("daytona_2d_start", 0, 0, 1, nullptr);
    s24_done_ = sceKernelCreateSema("daytona_2d_done", 0, 0, 1, nullptr);
    if (s24_start_ < 0 || s24_done_ < 0) {
        s24_create_result_ = s24_start_ < 0 ? s24_start_ : s24_done_;
        return false;
    }
    // One step above the main and sound threads (both at the default user priority): the
    // job is short (~2-3 ms) and the main core waits for it at the end of draw(), so it
    // preempts the sound board, which has room to spare in its frame.
    int priority = sceKernelGetThreadCurrentPriority();
    priority = priority > 64 && priority <= 191 ? priority - 1 : 159;
    const int mask = kWorker2DCore >= 0 && kWorker2DCore <= 2 ? SCE_KERNEL_CPU_MASK_USER_0 << kWorker2DCore
                                                              : SCE_KERNEL_CPU_MASK_USER_ALL;
    s24_thread_ = sceKernelCreateThread("daytona_2d", reinterpret_cast<SceKernelThreadEntry>(&s24_thread_entry),
                                        priority, 128 * 1024, 0, mask, nullptr);
    s24_create_result_ = s24_thread_;
    if (s24_thread_ < 0) { s24_thread_ = -1; return false; }
    GpuGlRenderer *self = this;
    const int started = sceKernelStartThread(s24_thread_, sizeof(self), &self);
    if (started < 0) {
        s24_create_result_ = started;
        sceKernelDeleteThread(s24_thread_);
        s24_thread_ = -1;
        return false;
    }
    sceKernelWaitSema(s24_done_, 1, nullptr);
    return true;
}

void GpuGlRenderer::s24_stop_thread() {
    s24_join();
    if (s24_thread_ >= 0) {
        s24_stop_.store(true);
        sceKernelSignalSema(s24_start_, 1);
        sceKernelWaitThreadEnd(s24_thread_, nullptr, nullptr);
        sceKernelDeleteThread(s24_thread_);
        s24_thread_ = -1;
    }
    if (s24_start_ >= 0) sceKernelDeleteSema(s24_start_);
    if (s24_done_ >= 0) sceKernelDeleteSema(s24_done_);
    s24_start_ = s24_done_ = -1;
}

void GpuGlRenderer::s24_kick(const rt::Video &video) {
    S24Slot &slot = s24_[s24_back_];
    if (s24_thread_ < 0) {
        const uint64_t begin = now_us();
        try {
            s24_prepare(slot, video);
        } catch (...) {
            slot.valid = false;
            slot.generation = slot.palette_generation = UINT64_MAX;
        }
        s24_job_us_ = now_us() - begin;
        return;
    }
    s24_video_ = &video;
    s24_job_ = &slot;
    s24_busy_ = true;
    sceKernelSignalSema(s24_start_, 1);
}

void GpuGlRenderer::s24_join() {
    if (!s24_busy_) return;
    sceKernelWaitSema(s24_done_, 1, nullptr);
    s24_busy_ = false;
}

// Quads of a prepared slot, written once, straight into the frame's GPU vertex memory.
// With k2DLayersByDepth the foreground is drawn before the polygons and the background
// after them; the depth buffer puts each one in its place (see k2DLayersByDepth).
void GpuGlRenderer::s24_emit(const S24Slot &slot, bool foreground) {
    const Depth depth = !k2DLayersByDepth ? Depth::Off : foreground ? Depth::TestWrite : Depth::Test;
    const float d = !k2DLayersByDepth ? 0.0f : foreground ? kForegroundDepth : kBackgroundDepth;
    if (!foreground) {
        // Backdrop (pen 0) under the background layers. Opaque when sorted by depth: alpha
        // is 255 once the palette is written, and black is black either way before that.
        push_quad(Prog::Flat, 0, kOffsetX, 0.0f, kOffsetX + kSourceW * kScale, kDisplayH, 0.0f, 0.0f, 0.0f, 0.0f,
                  slot.backdrop, depth, d);
    }
    for (const S24Run &run : slot.runs) {
        const GLuint texture = slot.textures[size_t((foreground ? 4 : 0) + run.layer)];
        // Indexed quads (4 vertices per rectangle) when they fit, else two triangles (6).
        Vertex *dst = append_quads(run.count);
        const bool indexed = dst != nullptr;
        const size_t count = size_t(run.count) * (indexed ? 4u : 6u);
        if (!indexed) dst = g.verts.append(count);
        if (!dst) { g.stats.dropped_vertices += unsigned(count); continue; }
        const uint32_t first = uint32_t(dst - g.verts.data);
        for (uint32_t i = run.first; i < run.first + run.count; ++i) {
            const S24Rect &r = slot.rects[i];
            const float u0 = float(r.x0 + r.h) / 512.0f, u1 = float(r.x1 + r.h) / 512.0f;
            const float v0 = float(r.v) / 512.0f, v1 = float(r.v + (r.y1 - r.y0)) / 512.0f;
            const float x0 = sx(float(r.x0)), x1 = sx(float(r.x1)), y0 = sy(float(r.y0)), y1 = sy(float(r.y1));
            // Triangles (x0y0, x1y0, x1y1) (x0y0, x1y1, x0y1) in both forms.
            *dst++ = {x0, y0, 1, u0, v0, 0, d, 0xffffffffu};
            *dst++ = {x1, y0, 1, u1, v0, 0, d, 0xffffffffu};
            *dst++ = {x1, y1, 1, u1, v1, 0, d, 0xffffffffu};
            if (!indexed) {
                *dst++ = {x0, y0, 1, u0, v0, 0, d, 0xffffffffu};
                *dst++ = {x1, y1, 1, u1, v1, 0, d, 0xffffffffu};
            }
            *dst++ = {x0, y1, 1, u0, v1, 0, d, 0xffffffffu};
        }
        // One command per layer texture (consecutive runs never share a texture).
        g.cmds.push_back({Prog::Layer, texture, first, uint32_t(count), depth, g.clip, indexed});
        system24_quads_ += run.count;
    }
}

} // namespace vita
