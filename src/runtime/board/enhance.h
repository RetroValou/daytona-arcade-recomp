// Enhancements, all off by default (rules.md: parity runs that way). Those
// that act inside the recompiled game code do so through the game's hooks
// (src/games/hooks.h), which read these options.
#pragma once

namespace rt {

struct Enhance {
    // Draw distance (Daytona: hook_draw_list): 0 = the game's own; -1 one
    // cell around the car, -2 the car's cell only; +1 the whole 5x5, +2 7x7.
    static constexpr int kDrawMin = -2, kDrawMax = 2;
    static inline int draw_distance = 0;
};

} // namespace rt
