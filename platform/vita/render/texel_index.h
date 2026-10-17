// One 4-bit Model 2 texel index from packed texture RAM, for the vitaGL index
// textures: rt::read_texel_quad(bx, by, u, u, v, v, sheet).t00 >> 4 without the
// quad's three other reads (tests/vita/test_vita_texel_index.cpp checks they agree).
#pragma once

#include <cstdint>
#include <cstring>

namespace vita {

inline uint32_t texel_index(uint32_t bx, uint32_t by, uint32_t u, uint32_t v, const uint32_t *sheet) {
    uint32_t x = bx + u;
    const uint32_t y = by + v;
    const uint32_t fold = x >= 1024 ? 512 : 0; // the x>=1024 fold flips y bit 10 (see raster_texel.h)
    if (fold) x -= 1024;
    const uint32_t off = (((y >> 1) ^ fold) << 9) + (x >> 1);
    const unsigned bit = unsigned(((off & 1) << 4) | ((1u ^ (v & 1u)) << 3) | ((1u ^ (u & 1u)) << 2));
    return (sheet[(off >> 1) & 0x7ffff] >> bit) & 15u;
}

// The texture RAM words of a texture at (bx, by), size w x h texels, in decode order: per
// row pair, per group of 4 texels. One 32-bit word holds a group of 4 consecutive x
// (bx is a multiple of 32: groups never straddle the x = 1024 fold) on 2 rows (y even,
// odd): the nibble at bit ((x >> 1) & 1) << 4 | (1 ^ (y & 1)) << 3 | (1 ^ (x & 1)) << 2,
// as texel_index reads it. h even (32 << n).
inline uint32_t texel_word(uint32_t bx, uint32_t by, uint32_t gx, uint32_t pair, const uint32_t *sheet) {
    uint32_t x = bx + gx;
    const uint32_t fold = x >= 1024 ? 512 : 0;
    if (fold) x -= 1024;
    const uint32_t off = ((((by >> 1) + pair) ^ fold) << 9) + (x >> 1);
    return sheet[(off >> 1) & 0x7ffff];
}

// A 64-bit hash of those words (index_texture_hash, decode_index_texture: the same
// value): whether a texture's texture RAM still holds what it was built from.
inline uint64_t texel_word_hash(uint64_t hash, uint32_t word) { return (hash ^ word) * 0x100000001b3ull; }
inline uint64_t index_texture_hash(uint32_t bx, uint32_t by, uint32_t w, uint32_t h, const uint32_t *sheet) {
    uint64_t hash = 0xcbf29ce484222325ull ^ (uint64_t(w) << 32 | h);
    for (uint32_t pair = 0; pair < h / 2; ++pair)
        for (uint32_t gx = 0; gx < w; gx += 4) hash = texel_word_hash(hash, texel_word(bx, by, gx, pair, sheet));
    return hash;
}

// The vitaGL index texture (GL_LUMINANCE_ALPHA: L = index * 16, A = 0 for index 15 when
// transparent_alpha is 0, else 255) of a w x h texture, a word at a time: texel_index's
// result for every texel (tests/vita/test_vita_texel_index.cpp). w a multiple of 4, h even.
// Returns index_texture_hash.
inline uint64_t decode_index_texture(uint32_t bx, uint32_t by, uint32_t w, uint32_t h, const uint32_t *sheet,
                                     uint8_t transparent_alpha, uint8_t *la) {
    uint16_t lut[16]; // L, A of each index, little-endian pairs
    for (uint32_t i = 0; i < 16; ++i) lut[i] = uint16_t(i * 16 | (i == 15 ? transparent_alpha : 255) << 8);
    uint64_t hash = 0xcbf29ce484222325ull ^ (uint64_t(w) << 32 | h);
    for (uint32_t pair = 0; pair < h / 2; ++pair) {
        uint8_t *even = la + size_t(pair) * 2u * w * 2u, *odd = even + size_t(w) * 2u;
        for (uint32_t gx = 0; gx < w; gx += 4) {
            const uint32_t word = texel_word(bx, by, gx, pair, sheet);
            hash = texel_word_hash(hash, word);
            // Row y even: bits 12, 8, 28, 24 for x = 0..3; row y odd: 4, 0, 20, 16.
            const uint16_t e[4] = {lut[(word >> 12) & 15], lut[(word >> 8) & 15], lut[(word >> 28) & 15], lut[(word >> 24) & 15]};
            const uint16_t o[4] = {lut[(word >> 4) & 15], lut[word & 15], lut[(word >> 20) & 15], lut[(word >> 16) & 15]};
            std::memcpy(even + gx * 2u, e, sizeof e);
            std::memcpy(odd + gx * 2u, o, sizeof o);
        }
    }
    return hash;
}

} // namespace vita
