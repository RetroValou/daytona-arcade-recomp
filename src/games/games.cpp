// The games the runtime knows: one record per MAME set (runtime/board/game.h), each
// defined in its game's file here in src/games/. A new game adds its file and
// one line to kGames (docs/adding-a-game.md).
#include "runtime/board/game.h"

#include <cstring>
#include <stdexcept>
#include <string>

// The ROM set this build is recompiled from (CMake's M2_ROMSET); builds that
// do not say are daytona93's.
#ifndef M2_ROMSET
#define M2_ROMSET "daytona93"
#endif

namespace rt {

// Defined in each game's file (constant-initialized: safe from any static initializer).
extern const Game daytona93_game, daytona_game, srallyc_game, vf2_game;

namespace {
const Game *const kGames[] = {
    &daytona93_game,
    &daytona_game,
    &srallyc_game,
    &vf2_game,
};
} // namespace

std::span<const Game *const> games() { return kGames; }

const Game *find_game(const char *set) {
    for (const Game *g : kGames)
        if (!std::strcmp(g->set, set)) return g;
    return nullptr;
}

const Game &game() {
    static const Game *const g = find_game(M2_ROMSET);
    if (!g) throw std::runtime_error(std::string("no game record for ROM set ") + M2_ROMSET + " (src/games/games.cpp)");
    return *g;
}

} // namespace rt
