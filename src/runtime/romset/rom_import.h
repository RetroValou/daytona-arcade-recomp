// Load the user's ROM set (a MAME-format zip or 7z) into the memory images
// the board runs from, in memory: every file CRC-checked against the ROM
// table of the set this build was recompiled from (rt::game().roms, from
// MAME's ROM_START: src/games/), then laid out as MAME loads it
// (ROM_LOAD32_WORD interleave, ROM_COPY mirrors). The build's importer
// (tools/m2import) uses the same tables; scripts/m2import.py is the zip-only
// daytona93 reference they were checked against.
#pragma once

#include "games/game.h"
#include "runtime/board/m2_board.h"

#include <string>
#include <vector>

namespace rt {

struct RomCheck {
    std::string file;
    bool ok = false;
    std::string problem; // missing, wrong size, wrong CRC
};

// Check a zip without loading it (for the launcher's status line).
std::vector<RomCheck> check_rom_set(const std::string &zip_path);

// Load and lay out the images. Throws ZipError naming the first problem.
M2Board::Images import_rom_set(const std::string &zip_path);

// The TGP program the i960 uploads at boot, cut from main_data (CRC-checked):
// empty for a board without a TGP.
std::vector<uint8_t> tgp_program(const M2Board::Images &img);
// Model 2B: the SHARC program the i960 uploads at boot (Game::sharc; 16-bit
// little-endian words, CRC-checked): empty for the other boards.
std::vector<uint8_t> sharc_program(const M2Board::Images &img);
// The SCSP DSP program the sound driver loads (Game::dsp), cut from the sound
// program (CRC-checked; big-endian words): empty for a game without one.
std::vector<uint8_t> dsp_program(const M2Board::Images &img);

} // namespace rt
