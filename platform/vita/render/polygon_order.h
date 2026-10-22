#pragma once

#include "games/game.h"
#include "runtime/video/geo.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vita {

// Same total order as the CPU reference: descending window, ascending depth,
// newest polygon first on ties. The painter renderer traverses it in reverse.
// Extract small integer keys once instead of repeatedly chasing GeoPoly data.
class PolygonOrder {
public:
    struct Entry { uint32_t key; size_t index; };

    const std::vector<Entry> &sort(const std::vector<rt::GeoPoly> &polys) {
        const size_t count = polys.size();
        first_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const size_t index = count - 1 - i;
            first_[i] = {((255u - uint32_t(polys[index].window)) << 16) |
                         uint32_t(polys[index].z), index};
        }
        if (count <= 32) {
            std::sort(first_.begin(), first_.end(), [](const Entry &a, const Entry &b) {
                return a.key != b.key ? a.key < b.key : a.index > b.index;
            });
            return first_;
        }
        std::array<std::array<size_t, 256>, 3> counts{};
        for (const Entry &entry : first_)
            for (unsigned digit = 0; digit < 3; ++digit)
                ++counts[digit][(entry.key >> (digit * 8)) & 255u];
        second_.resize(count);
        auto *source = &first_, *destination = &second_;
        for (unsigned digit = 0; digit < 3; ++digit) {
            auto &offsets = counts[digit];
            size_t total = 0;
            bool single_bucket = false;
            for (size_t &bucket : offsets) {
                const size_t size = bucket;
                single_bucket |= size == count;
                bucket = total;
                total += size;
            }
            if (single_bucket) continue;
            for (const Entry &entry : *source)
                (*destination)[offsets[(entry.key >> (digit * 8)) & 255u]++] = entry;
            std::swap(source, destination);
        }
        return *source;
    }

private:
    std::vector<Entry> first_, second_;
};

// The hybrid depth (the Vita option POLYGON ORDER): the polygons' depth range cut into
// this many equal bands, one per Model 2 window (kVertexShader decodes them).
constexpr unsigned kZDepthBands = 32;

// The hybrid depth's band key of a window: its group's first window when the game
// groups it with others (rt::Game::update_z_depth_window_groups), else its own.
inline unsigned z_band_key(unsigned window) {
    for (const auto &r : rt::game().update_z_depth_window_groups)
        if (r.first < r.last && r.first <= window && window <= r.last) return r.first;
    return window;
}

// Model 2 draw priority -> one depth per polygon (see the Depth comment in
// render/gl_internal.h), for the arcade order or the hybrid one. Run by the geometry
// thread right after its parse, published with the list (rt::Geo::depth_of), so the
// main core only reads it; the renderer runs it itself when that one does not match
// (Sync mode, the frame the option changes).
inline void polygon_depth(const std::vector<rt::GeoPoly> &polys, bool hybrid, PolygonOrder &sorter,
                          std::vector<float> &depth) {
    depth.resize(polys.size());
    if (polys.empty()) return;
    const auto &order = sorter.sort(polys); // first = drawn first = wins
    if (!hybrid) {
        const float step = 1.0f / float(polys.size() + 1);
        for (size_t rank = 0; rank < order.size(); ++rank) depth[order[rank].index] = 1.0f - float(rank + 1) * step;
        return;
    }
    // A Model 2 window is drawn over the ones before it, so each keeps its own band
    // and the shader mixes rank and distance inside it only. Coded as 1 + band + the
    // rank inside the band, in (0, 1), larger in front (kVertexShader decodes it). The
    // order runs window by window, front (highest) first: the front window gets band
    // kZDepthBands - 1; past kZDepthBands windows, the backmost share band 0. The
    // windows a game groups (contiguous in the order) share one band.
    for (size_t first = 0, run = 0; first < order.size(); ++run) {
        const unsigned band = run < kZDepthBands - 1 ? unsigned(kZDepthBands - 1 - run) : 0u;
        size_t end = first + 1;
        while (end < order.size() &&
               (band == 0 || z_band_key(polys[order[end].index].window) == z_band_key(polys[order[end - 1].index].window)))
            ++end;
        const float step = 1.0f / float(end - first + 1);
        for (size_t rank = first; rank < end; ++rank)
            depth[order[rank].index] = 1.0f + float(band) + (1.0f - float(rank - first + 1) * step);
        first = end;
    }
}

} // namespace vita
