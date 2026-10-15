// The Vita tile cache (M2_VITA_RENDER_OPT) with write tracking, which compares only the
// tile RAM blocks and character RAM pages the board reports, against the same cache
// comparing everything every frame (no tracking): the same layer pixels and tile
// generations after every frame of random writes (scroll registers every frame, tiles,
// sometimes characters, sometimes nothing), as the board reports them (changed bytes only).
#include "runtime/video.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    std::mt19937 rng(0x5124);
    std::vector<uint8_t> tiles_a(0x10000), chars_a(0x80000), palram(0x4000), xlat(0xc000);
    for (auto &b : tiles_a) b = uint8_t(rng());
    for (auto &b : chars_a) b = uint8_t(rng());
    std::vector<uint8_t> tiles_b = tiles_a, chars_b = chars_a;
    rt::Video tracked(tiles_a.data(), chars_a.data()), full(tiles_b.data(), chars_b.data());
    tracked.enable_write_tracking();
    rt::VideoMem mem;
    mem.palram = palram.data();
    mem.colorxlat = xlat.data();
    const std::vector<rt::GeoPoly> polys;
    auto write_tile = [&](uint32_t offset, uint8_t value) {
        if (tiles_a[offset] == value) return; // the board reports changed bytes only
        tiles_a[offset] = tiles_b[offset] = value;
        tracked.tile_memory_w(offset);
    };
    auto write_char = [&](uint32_t offset, uint8_t value) {
        if (chars_a[offset] == value) return;
        chars_a[offset] = chars_b[offset] = value;
        tracked.character_memory_w(offset);
    };
    unsigned frames = 0;
    for (int frame = 0; frame < 600; ++frame) {
        const unsigned kind = rng() % 8;
        if (kind != 0) {
            for (int i = 0; i < 8; ++i) write_tile(0xa000 + (rng() & 0xf), uint8_t(rng())); // scroll/control
            for (int i = 0; i < 4; ++i) write_tile(0x8000 + (rng() & 0xfff), uint8_t(rng())); // line scroll
        }
        if (kind >= 2) for (unsigned i = 0, n = rng() % 40; i < n; ++i) write_tile(rng() & 0x7fff, uint8_t(rng()));
        if (kind == 3) for (int i = 0; i < 3; ++i) write_tile(0xc000 + (rng() & 0x1fff), uint8_t(rng())); // windows
        if (kind == 4) write_tile(0xa00c + (rng() & 1), uint8_t(rng())); // tile(0x5006): the split mode
        if (kind == 5) for (unsigned i = 0, n = 1 + rng() % 64; i < n; ++i) write_char(rng() & 0x7ffff, uint8_t(rng()));
        if (kind == 6) { // a whole character page rewritten with the same bytes but one
            const uint32_t page = (rng() & 0x7ff) * 256;
            write_char(page + (rng() & 255), uint8_t(rng()));
        }
        tracked.frame_start(); full.frame_start();
        tracked.screen_update(polys, 0, mem);
        full.screen_update(polys, 0, mem);
        for (int layer = 0; layer < 4; ++layer) {
            CHECK(std::memcmp(tracked.system24_pixels(layer), full.system24_pixels(layer), 512 * 512 * 2) == 0);
            for (unsigned t = 0; t < 4096; ++t)
                CHECK(tracked.system24_tile_generation(layer, t) == full.system24_tile_generation(layer, t));
        }
        CHECK(tracked.last_profile().tiles_rebuilt == full.last_profile().tiles_rebuilt);
        CHECK(tracked.last_profile().characters_changed == full.last_profile().characters_changed);
        ++frames;
    }
    std::printf("Vita tile cache with write tracking equals the full comparison over %u frames\n", frames);
}
