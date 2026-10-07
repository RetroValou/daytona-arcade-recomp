#pragma once

#include "runtime/video.h"

#include <cstddef>
#include <cstdint>

namespace vita {

// System 24 tile textures, two forms:
//   * colour (upload_system24_layer, libvita2d path): each texel is the pen's
//     RGBA colour. A palette change recolours every tile: all 4096 tiles of
//     the 4 layers are rewritten (16384 tiles, 8 MB; ~67 ms on the Vita).
//   * index (upload_system24_layer_indices, vitaGL path): each texel is the
//     pen NUMBER; the fragment shader looks its colour up in a 128x64 palette
//     texture (upload_system24_palette, 8192 pens, 32 KB). A palette change
//     rewrites only that texture; tiles are rewritten only when their pixels
//     or categories change.
// Both write only tiles changed since the last presented source generation.
// The caller must finish any previous GPU readers before writing them. Kept
// independent of GXM so tests exercise the exact Vita upload conversion.

// Index texel (RGBA8, bytes r g b a) of 13-bit pen p, already in palette texture
// units: r = (p % 128) * 2 (column), g = (p / 128) * 4 (row), a = 255 when visible;
// 0 (alpha 0, discarded by the alpha test) when not. The shader reads the column
// with floor(r * 127.5 + 0.01) and the row with floor(g * 63.75 + 0.01): the same
// form as the Model 2 palette lookup the console's shader compiler already takes.
inline uint32_t system24_index_texel(uint16_t pen, bool visible) {
    const uint32_t p = uint32_t(pen) & 0x1fffu;
    return visible ? 0xff000000u | ((p & 127u) << 1) | ((p >> 7) << 10) : 0u;
}

inline uint32_t system24_argb_to_rgba(uint32_t argb) {
    return (argb & 0xff00ff00u) | ((argb & 0xffu) << 16u) | ((argb >> 16u) & 0xffu);
}

template <bool kIndices>
inline unsigned upload_system24_layer_impl(const rt::Video &video, int layer, uint64_t previous,
                                           void *background, size_t background_stride,
                                           void *foreground, size_t foreground_stride) {
    constexpr size_t row_bytes = 512u * sizeof(uint32_t);
    if (!background || !foreground || background_stride < row_bytes || foreground_stride < row_bytes ||
        (background_stride % alignof(uint32_t)) || (foreground_stride % alignof(uint32_t))) return 0;
    // Index form: colours are not in the tiles, a palette change rewrites nothing here.
    const bool full = previous == UINT64_MAX || (!kIndices && video.system24_palette_generation() > previous);
    const bool opaque_background = layer >= 2;
    const bool split_background = opaque_background && (video.system24_word(0x5006) & 0x6000);
    const uint16_t *pixels = video.system24_pixels(layer);
    const uint8_t *flags = video.system24_flags(layer);
    auto *back = static_cast<uint8_t *>(background);
    auto *front = static_cast<uint8_t *>(foreground);
    unsigned uploaded = 0;
    for (unsigned tile = 0; tile < 4096; ++tile) {
        if (!full && video.system24_tile_generation(layer, tile) <= previous) continue;
        const unsigned tx = (tile & 63u) * 8u, ty = (tile >> 6u) * 8u;
        for (unsigned y = ty; y < ty + 8u; ++y) {
            auto *back_row = reinterpret_cast<uint32_t *>(back + size_t(y) * background_stride);
            auto *front_row = reinterpret_cast<uint32_t *>(front + size_t(y) * foreground_stride);
            for (unsigned x = tx; x < tx + 8u; ++x) {
                const size_t i = size_t(y) * 512u + x;
                const bool opaque = (flags[i] & 0x10) != 0;
                const bool category1 = (flags[i] & 1) != 0;
                // Unlike normal-mode draw_rect, split-mode tilemap_draw
                // retains the category test even with DRAW_OPAQUE.
                const bool back_visible = opaque_background ? (!split_background || !category1) :
                                                              (opaque && !category1);
                const bool front_visible = opaque && category1;
                if constexpr (kIndices) {
                    back_row[x] = system24_index_texel(pixels[i], back_visible);
                    front_row[x] = system24_index_texel(pixels[i], front_visible);
                } else {
                    const uint32_t rgba = system24_argb_to_rgba(video.system24_pen(pixels[i]));
                    back_row[x] = back_visible ? rgba : 0u;
                    front_row[x] = front_visible ? rgba : 0u;
                }
            }
        }
        ++uploaded;
    }
    return uploaded;
}

inline unsigned upload_system24_layer(const rt::Video &video, int layer, uint64_t previous,
                                      void *background, size_t background_stride,
                                      void *foreground, size_t foreground_stride) {
    return upload_system24_layer_impl<false>(video, layer, previous, background, background_stride, foreground,
                                             foreground_stride);
}

inline unsigned upload_system24_layer_indices(const rt::Video &video, int layer, uint64_t previous,
                                              void *background, size_t background_stride,
                                              void *foreground, size_t foreground_stride) {
    return upload_system24_layer_impl<true>(video, layer, previous, background, background_stride, foreground,
                                            foreground_stride);
}

// The palette texture of the index form: pen p at texel (p % 128, p / 128),
// RGBA8, rows of row_pixels texels (>= 128).
constexpr unsigned kSystem24PaletteWidth = 128, kSystem24PaletteHeight = 64; // 8192 pens
inline bool upload_system24_palette(const rt::Video &video, uint32_t *texels, size_t row_pixels) {
    if (!texels || row_pixels < kSystem24PaletteWidth) return false;
    for (unsigned pen = 0; pen < kSystem24PaletteWidth * kSystem24PaletteHeight; ++pen)
        texels[size_t(pen / kSystem24PaletteWidth) * row_pixels + pen % kSystem24PaletteWidth] =
            system24_argb_to_rgba(video.system24_pen(pen));
    return true;
}

} // namespace vita
