// vitaGL renderer: Model 2 index textures and palettes (render/renderer.cpp).

#include "render/gl_internal.h"
#include "render/texel_index.h"
#include "runtime/raster_texel.h"

namespace vita {
using namespace gldraw;

// ---- Renderer: Model 2 textures and palettes -------------------------------------------
// Index texture of one texture region (texheader[0] size/mirror/transparent, texheader[2]
// position). L = index * 16 so that the GPU's bilinear filter interpolates the index.
// A = 0 marks a transparent texel (index 15 when the texture is transparent); its L is
// copied from an opaque neighbour so the filter does not pull index 15 into the edges.
const GpuGlRenderer::Source *GpuGlRenderer::source_for(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const SourceKey key{uint16_t(poly.texheader[0] & 0x23ff), uint16_t(poly.texheader[2] & 0x1fff)};
    const uint32_t key32 = uint32_t(key.h0) | uint32_t(key.h2) << 16;
    Source *known = nullptr;
    if (Source *const *cached = source_cache_.find(key32)) known = *cached;
    else if (auto found = sources_.find(key); found != sources_.end()) {
        known = &found->second;
        source_cache_.insert(key32, known); // full: stays in sources_ only
    }
    const uint32_t bx = 32u * (poly.texheader[2] & 0x3f);
    const uint32_t by = 32u * ((poly.texheader[2] >> 6) & 0x1f);
    const uint32_t *sheet = (poly.texheader[2] & 0x1000) ? mem.tex1 : mem.tex0;
    if (known && known->check) {
        // Its texture RAM was written: the same words keep it, others rebuild it.
        const uint64_t check_begin = kGlLog ? now_us() : 0;
        const bool same = vita::index_texture_hash(bx, by, known->source_w, known->source_h, sheet) == known->raw_hash;
        if constexpr (kGlLog) { // the probes: diagnostics builds only
            const uint64_t check_us = now_us() - check_begin;
            texture_probe_.check_us += check_us;
            last_texture_us_ += check_us;
            g.timing.rec_textures += check_us;
        }
        if (same) {
            known->check = false;
            known->generation = mem.tex_generation;
            if constexpr (kGlLog) ++texture_probe_.kept;
        } else {
            if constexpr (kGlLog) ++texture_probe_.changed;
            GLuint t = known->texture;
            if (t) glDeleteTextures(1, &t);
            cached_bytes_ -= std::min(cached_bytes_, known->bytes);
            sources_.erase(key);
            source_cache_.clear(); // it points into sources_; refilled on use
            known = nullptr;
        }
    }
    if (known) return known;
    if (cached_bytes_ >= kSourceCacheBytes) {
        cache_reset_pending_ = true; // emptied by the next prepare_frame()
        ++material_drops_;
        return nullptr;
    }
    Source s;
    s.source_w = 32u << (poly.texheader[0] & 7);
    s.source_h = 32u << ((poly.texheader[0] >> 3) & 7);
    // Spread a cold cache over several frames, by texels decoded (cost grows with the
    // size; a count let one frame take 32 large textures, ~88 ms). The first build of a
    // frame is always allowed, so even the largest texture is never deferred forever.
    const uint32_t texels = std::min(s.source_w, kTextureLimit) * std::min(s.source_h, kTextureLimit);
    if (g.stats.new_sources > 0 &&
        (g.stats.new_sources >= kSourceBuildBudget || g.stats.new_texels + texels > kSourceTexelBudget)) {
        ++material_defers_;
        ++g.stats.deferred_sources;
        return nullptr;
    }

    const uint64_t build_begin = now_us();
    s.transparent = (poly.texheader[0] >> 13) & 1;
    // Above kTextureLimit, keep one texel every 'step' (GL texture side limit).
    const uint32_t step_x = std::max(1u, (s.source_w + kTextureLimit - 1) / kTextureLimit);
    const uint32_t step_y = std::max(1u, (s.source_h + kTextureLimit - 1) / kTextureLimit);
    const uint32_t w = (s.source_w + step_x - 1) / step_x;
    const uint32_t h = (s.source_h + step_y - 1) / step_y;
    std::vector<uint8_t> &la = index_scratch_; // L, A pairs
    const uint64_t decode_begin = kGlLog ? now_us() : 0;
    la.resize(size_t(w) * h * 2u);
    const uint8_t transparent_alpha = s.transparent ? 0 : 255; // alpha of index 15
    if (step_x == 1 && step_y == 1) {
        // A texture RAM word at a time (8 texels), its hash on the way.
        s.raw_hash = vita::decode_index_texture(bx, by, w, h, sheet, transparent_alpha, la.data());
    } else {
        // Above kTextureLimit: one texel every step, a texel at a time.
        s.raw_hash = vita::index_texture_hash(bx, by, s.source_w, s.source_h, sheet);
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t ty = std::min(y * step_y, s.source_h - 1);
            uint8_t *row = &la[size_t(y) * w * 2u];
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t tx = x * step_x; // < source_w: w = ceil(source_w / step_x)
                const uint32_t index = texel_index(bx, by, tx, ty, sheet);
                row[x * 2u] = uint8_t(index * 16);
                row[x * 2u + 1] = index == 15 ? transparent_alpha : 255;
            }
        }
    }
    if (s.transparent) {
        // An elegant replacement for the modulo: a texture's sides are powers of two
        // (32 << n, also after the step), so the wrap x % w is x & (w - 1), the same
        // value with one AND instead of a software division (the Vita's Cortex-A9 has
        // no divide instruction), four times per transparent texel.
        const uint32_t wm = w - 1, hm = h - 1;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = (size_t(y) * w + x) * 2u;
                if (la[i + 1]) continue;
                const size_t n[4] = {(size_t(y) * w + ((x + 1) & wm)) * 2u, (size_t(y) * w + ((x + wm) & wm)) * 2u,
                                     (size_t((y + 1) & hm) * w + x) * 2u, (size_t((y + hm) & hm) * w + x) * 2u};
                for (size_t j : n)
                    if (la[j + 1]) { la[i] = la[j]; break; }
            }
    }
    const uint64_t decode_end = kGlLog ? now_us() : 0;
    // The texture RAM pages it was decoded from (texel_index's addressing: each row pair
    // of texels is one run of words; the x >= 1024 half is folded), for mark_rewritten_sources.
    s.generation = mem.tex_generation;
    s.sheet = (poly.texheader[2] & 0x1000) ? 1 : 0;
    for (uint32_t half = 0; half < 2; ++half) {
        const uint32_t x0 = std::max(bx, half ? 1024u : 0u), x1 = std::min(bx + s.source_w, half ? 2048u : 1024u);
        if (x0 >= x1) continue;
        const uint32_t fold = half ? 512 : 0, xa = (x0 - (half ? 1024 : 0)) >> 1, xb = (x1 - 1 - (half ? 1024 : 0)) >> 1;
        for (uint32_t r = by >> 1; r <= (by + s.source_h - 1) >> 1; ++r) {
            const uint32_t row = (r ^ fold) << 9;
            const uint32_t first = ((((row + xa) >> 1) & 0x7ffff) * 4) >> 12, last = ((((row + xb) >> 1) & 0x7ffff) * 4) >> 12;
            for (uint32_t p = first; p <= last && p < rt::VideoMem::kTexPages; ++p) s.pages[p >> 6] |= uint64_t(1) << (p & 63);
        }
    }
    // Mirror flags map to GL mirrored repeat, otherwise plain repeat (road textures repeat).
    const uint64_t upload_begin = kGlLog ? now_us() : 0;
    s.texture = make_texture(w, h, true, ((poly.texheader[0] >> 8) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT,
                             ((poly.texheader[0] >> 9) & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT, GL_LUMINANCE_ALPHA,
                             la.data());
    if (!s.texture) { ++material_drops_; return nullptr; }
    s.bytes = la.size();
    cached_bytes_ += s.bytes;
    ++g.stats.new_sources;
    g.stats.new_texels += w * h;
    const uint64_t build_end = now_us();
    g.timing.rec_textures += build_end - build_begin;
    last_texture_us_ += build_end - build_begin;
    if constexpr (kGlLog) { // the probes: diagnostics builds only
        texture_probe_.decode_us += decode_end - decode_begin;
        texture_probe_.upload_us += build_end - upload_begin;
        ++texture_probe_.builds;
        texture_probe_.texels += w * h;
    }
    Source *built = &sources_.emplace(key, s).first->second;
    source_cache_.insert(key32, built);
    return built;
}

// Row of the palette texture for this polygon (created on first use), -1 when full.
// Entry k = colour of luminance step k, same maths as the CPU renderer:
//   luma = lumaram[luma base + k] * polygon luma / 256, then colour table + gamma.
int GpuGlRenderer::palette_row(const rt::GeoPoly &poly, const rt::VideoMem &mem) {
    const uint32_t lumabase = poly.texheader[1] & 0xff;
    const uint32_t color_index = (poly.texheader[3] >> 6) & 0x3ff;
    const uint32_t luma_scale = kLumaMask ? (poly.luma & kLumaMask) : 0xff;
    const uint32_t key = lumabase | color_index << 8 | luma_scale << 18;
    if (const uint32_t *found = palette_index_.find(key)) return int(*found);
    if (palette_used_ >= kPaletteRows) { ++material_drops_; return -1; }

    const uint32_t row = palette_used_++;
    const uint32_t color = le16(mem.palram, color_index + 0x1000) & 0x7fff;
    uint32_t *out = &palette_data_[size_t(row) * kPaletteWidth]; // texture memory, 128-pixel rows
    for (uint32_t k = 0; k < kPaletteWidth; ++k) {
        const uint32_t luma = std::min(uint32_t(mem.lumaram[((lumabase << 7) + k) * 4]) * luma_scale / 256, 0x3fu);
        const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t gg = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
        const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
        out[k] = RGBA8(r, gg, b, 255);
    }
    palette_index_.insert(key, row); // kPaletteRows < capacity: always room
    ++material_builds_;
    ++g.stats.new_palettes;
    return int(row);
}

void GpuGlRenderer::mark_rewritten_sources(const rt::VideoMem &mem) {
    if (mem.tex_generation == tex_generation_ || !mem.tex_page_generation) return;
    tex_generation_ = mem.tex_generation;
    for (auto &entry : sources_) {
        Source &s = entry.second;
        if (s.check) continue;
        const uint64_t *gen = mem.tex_page_generation + size_t(s.sheet) * rt::VideoMem::kTexPages;
        bool stale = false;
        for (uint32_t w = 0; w < rt::VideoMem::kTexPages / 64 && !stale; ++w)
            for (uint64_t bits = s.pages[w]; bits && !stale; bits &= bits - 1)
                stale = gen[w * 64 + uint32_t(__builtin_ctzll(bits))] > s.generation;
        if (!stale) continue;
        s.check = true; // source_for compares its words before the next use
        if constexpr (kGlLog) ++texture_probe_.rewritten;
    }
}

uint32_t GpuGlRenderer::solid_color(const rt::GeoPoly &poly, const rt::VideoMem &mem) const {
    const uint32_t color = le16(mem.palram, ((poly.texheader[3] >> 6) & 0x3ff) + 0x1000);
    const uint32_t luma = poly.luma >> 2;
    const uint8_t r = gamma_[le16(mem.colorxlat, (((color >> 0) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t gg = gamma_[le16(mem.colorxlat, 0x4000 / 2 + (((color >> 5) & 0x1f) << 8) + luma) & 0xff];
    const uint8_t b = gamma_[le16(mem.colorxlat, 0x8000 / 2 + (((color >> 10) & 0x1f) << 8) + luma) & 0xff];
    return RGBA8(r, gg, b, 255);
}

} // namespace vita
