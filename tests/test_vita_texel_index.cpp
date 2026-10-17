// vita::texel_index (vitaGL index textures) against rt::read_texel_quad, the
// CPU rasterizer's texel fetch, on random and edge positions (x >= 1024 fold).
#include "../platform/vita/render/texel_index.h"
#include "runtime/raster_texel.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

int main() {
    std::mt19937 rng(0x7e1);
    std::vector<uint32_t> sheet(0x80000);
    for (auto &w : sheet) w = rng();
    unsigned checked = 0;
    auto check = [&](uint32_t bx, uint32_t by, uint32_t u, uint32_t v) {
        const uint32_t expected = rt::read_texel_quad(bx, by, u, u, v, v, sheet.data()).t00 >> 4;
        if (vita::texel_index(bx, by, u, v, sheet.data()) != expected) {
            std::fprintf(stderr, "texel_index mismatch at bx=%u by=%u u=%u v=%u\n", bx, by, u, v);
            std::exit(1);
        }
        ++checked;
    };
    for (unsigned i = 0; i < 2000000; ++i)
        check(32u * (rng() & 63u), 32u * (rng() & 31u), rng() & 2047u, rng() & 2047u);
    for (uint32_t bx : {0u, 992u, 1024u - 32u, 2016u})
        for (uint32_t u = 0; u < 64; ++u)
            for (uint32_t v = 0; v < 8; ++v) check(bx, 992u, 1000u + u, v);
    // decode_index_texture (a word at a time) against texel_index on whole textures, the
    // fold included (bx 992 / 1024 - 32 with widths past x = 1024); its hash against
    // index_texture_hash, and the hash's sensitivity to one changed word.
    unsigned textures = 0;
    std::vector<uint8_t> la;
    auto texture = [&](uint32_t bx, uint32_t by, uint32_t w, uint32_t h, uint8_t transparent_alpha) {
        la.assign(size_t(w) * h * 2u, 0xcd);
        const uint64_t hash = vita::decode_index_texture(bx, by, w, h, sheet.data(), transparent_alpha, la.data());
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t index = vita::texel_index(bx, by, x, y, sheet.data());
                const size_t i = (size_t(y) * w + x) * 2u;
                if (la[i] != index * 16 || la[i + 1] != (index == 15 ? transparent_alpha : 255)) {
                    std::fprintf(stderr, "decode_index_texture mismatch at bx=%u by=%u %ux%u texel %u,%u\n", bx, by, w, h, x, y);
                    std::exit(1);
                }
            }
        if (hash != vita::index_texture_hash(bx, by, w, h, sheet.data())) {
            std::fprintf(stderr, "hash mismatch at bx=%u by=%u %ux%u\n", bx, by, w, h);
            std::exit(1);
        }
        ++textures;
    };
    for (unsigned i = 0; i < 300; ++i)
        texture(32u * (rng() & 63u), 32u * (rng() & 31u), 32u << (rng() % 5), 32u << (rng() % 5), (rng() & 1) ? 0 : 255);
    for (uint32_t bx : {960u, 992u, 1024u - 32u, 2016u}) texture(bx, 992u, 128, 64, 0);
    { // One changed texel in the texture changes the hash (texture (64, 64), texel (61, 17)).
        const uint64_t before = vita::index_texture_hash(64, 64, 64, 32, sheet.data());
        const uint32_t x = 64 + 61, off = ((64 + 17) >> 1 << 9) + (x >> 1);
        sheet[(off >> 1) & 0x7ffff] ^= 1u << 20;
        if (vita::index_texture_hash(64, 64, 64, 32, sheet.data()) == before) {
            std::fprintf(stderr, "hash missed a changed word\n");
            std::exit(1);
        }
    }
    std::printf("vita::texel_index equals rt::read_texel_quad on %u reads; decode_index_texture equals it on %u textures\n",
                checked, textures);
}
