// Host-side video measurements only. Never used to advance the board clock.
#pragma once
#include <cstdint>
namespace rt {
struct VideoProfile {
    uint64_t tile_cache = 0, tile_draw = 0, raster = 0, composite = 0;
    uint64_t palette = 0; // inside tile_cache: the dirty palette's conversion to pens
    uint32_t tiles_rebuilt = 0, characters_changed = 0;
    bool layers_rebuilt = false;
};
} // namespace rt
