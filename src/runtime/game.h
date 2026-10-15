// What differs between the Model 2 games this runtime runs, in one record per
// MAME set: the board the game was made for, its ROM files and their layout,
// where its TGP program is, its idle loops, how its cabinet's controls reach
// the board, and its sound driver's tables. Everything else (the board, the
// TGP, the geometrizer, video, sound chips) is the same for every game.
//
// The records live in src/games/, one file per game; src/games/games.cpp
// lists them. A build is recompiled from one set (M2_ROMSET) and game() is
// that set's record. Adding a game: docs/adding-a-game.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rt {

enum class BoardType : uint8_t {
    Model2o, // original Model 2 (MAME model2o_state): Model 1 I/O board behind a dual-port RAM, Model 1 sound board
    Model2a, // Model 2A-CRX (model2a_state): 315-5649 I/O chip and 93C46 EEPROM on the bus, SCSP sound board
};

enum class SoundBoardType : uint8_t {
    None,
    Model1, // Model 1 sound board: 68000 + YM3438 + 2x MultiPCM (MAME segam1audio; original Model 2)
    Scsp,   // Model 2A/2B/2C sound board: 68000 + SCSP (MAME model2_scsp)
};

// The cabinet's controls, as every frontend fills them (a driving cabinet:
// MAME's daytona port layout). A game on another board, or with other
// controls, maps them to its own ports in Game::io.
struct Inputs {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20; // ADC values, 0x20-0xe0
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;       // switches, active low (MAME daytona IN0/IN1/IN2 layout)
    uint8_t handbrake = 0;                            // hand brake (analog 0x00-0xff); 0: off
};

// One ROM file: MAME's ROM_LOAD32_WORD (16-bit words into bytes 0-1, offset
// 0, or 2-3, offset 2, of each dword) for the i960-side regions; the sound
// regions load whole files (the 68000 program and the SCSP samples
// byte-swapped: ROM_LOAD16_WORD_SWAP).
enum class LoadRegion : uint8_t { Program, MainData, CoproData, Polygons, Textures, CoproTables, SoundProgram, Pcm1, Pcm2, Samples };
namespace rom_regions { // for the ROM tables in src/games: `using namespace rom_regions;`
inline constexpr LoadRegion Program = LoadRegion::Program, MainData = LoadRegion::MainData, CoproData = LoadRegion::CoproData,
                           Polygons = LoadRegion::Polygons, Textures = LoadRegion::Textures, CoproTables = LoadRegion::CoproTables,
                           SoundProgram = LoadRegion::SoundProgram, Pcm1 = LoadRegion::Pcm1, Pcm2 = LoadRegion::Pcm2,
                           Samples = LoadRegion::Samples;
} // namespace rom_regions
struct RomLoad {
    const char *file;
    uint32_t crc;
    LoadRegion region;
    uint32_t offset, size;
};

// Where the TGP program the i960 uploads at boot sits in the ROMs (CRC-checked
// when it is cut out: rom_import.cpp tgp_program).
struct TgpProgram {
    bool in_program; // in the i960 program ROM, else in main_data
    uint32_t offset, words, crc;
};

// The Model 1 sound driver's tables in the game's 68000 program (the native
// audio engine reads them: native_sound_sequencer.cpp).
struct SoundLayout {
    uint32_t sequence_banks, profiles, fine_pitch, bend_curves, note_codes, velocity, instruments, percussion;
};

// The SCSP sound driver's voice table in the 68000's RAM, which says whose
// voice each SCSP slot plays: `entry` bytes per slot from `table`, the owner
// at `owner` (0: the music's sequence, else a sound effect's). Read at each
// key-on, for the music and effects volumes (snd::ScspBoard::set_volumes).
struct ScspVoices {
    uint32_t table, entry, owner;
};

// How the player's controls reach the game's wiring (Game::io and the frontends):
// the driving cabinet's (wheel, pedals, gearbox, view buttons), or one player's
// joystick and buttons, which the frontends hand over in the same rt::Inputs
// (left/right as the wheel at its ends, up/down as the accelerator/brake fully
// pressed, the buttons as view buttons 1-3: src/games/vf2.cpp).
enum class ControlScheme : uint8_t { Driving, Fighting };

// A frontend's default settings for the game (the Vita's: platform/vita
// VitaSettings; RESET DEFAULTS goes back to them). Volumes in percent.
struct FrontendDefaults {
    int music_volume = 100;
    // The effects against the music: Daytona's effects cover its music on the
    // Vita's speakers, so 20 % by default.
    int effects_volume = 20;
};

// How the cabinet's controls reach a Model 2A-CRX board's 315-5649 I/O chip
// (MAME's INPUT_PORTS for the game). port: 1 = port B (IN0, outside the
// EEPROM mode), 2 = C (IN1), 3 = D (IN2); channel: the analog mux, 0-7.
struct CrxInputs {
    uint8_t (*port)(unsigned port, const Inputs &in);
    uint8_t (*analog)(unsigned channel, const Inputs &in);
};

struct Game {
    const char *set;   // MAME set name (M2_ROMSET, seeds/<set>.txt, scripts/games.py)
    const char *title; // for window titles and messages
    const char *label; // this set among its game's sets, e.g. "Revision A, 1994"
    // The other set of the same game a Vita package can switch to (Daytona's
    // two sets share one app), or nullptr.
    const char *companion;
    BoardType board;

    std::span<const RomLoad> roms;
    uint32_t mirror_from; // main_data's ROM_COPY source, copied to every 1 MB from there up to 0xF00000; 0: none
    TgpProgram tgp;

    struct Range {
        uint32_t first, last;
    };
    // The game's wait-for-vblank loops (inclusive i960 address ranges): the
    // frame ends when the game idles in one. Unused entries are {0, 0}.
    Range idle[2];

    // The sound board, and (Model 1 only) the native audio engine's tables
    // for its driver (nullptr: the recompiled sound board only).
    SoundBoardType sound;
    const SoundLayout *native_sound;

    CrxInputs io;    // Model 2A-CRX boards only
    bool link;       // has the communication board (link play)
    // The draw distance enhancement works on it (Enhance::draw_distance: a
    // hook of the game's, src/games/hooks.h). Frontends list the option only then.
    bool draw_distance = false;
    // SCSP board: its driver's voice table (music or effect per slot), or
    // nullptr (no music and effects volumes).
    const ScspVoices *scsp_voices = nullptr;
    // The frontends' output gain for the reference audio: the game's own
    // levels against Daytona's (the cabinet's amplifier makes up the rest).
    // m2run's WAV and every parity check stay at the board's output.
    float output_gain = 1.0f;
    // The frontends' default settings for it.
    FrontendDefaults defaults{};
    ControlScheme controls = ControlScheme::Driving;
};


// The game's sound board can mix its music and effects at their own volumes
// (snd::Board::set_volumes): the Model 1 board (its driver's voice pools) or
// an SCSP board whose driver's voice table is known.
inline bool has_music_effects(const Game &g) {
    return g.sound == SoundBoardType::Model1 || g.scsp_voices != nullptr;
}

// Every game the runtime knows (src/games/games.cpp).
std::span<const Game *const> games();
// The record for a set, or nullptr.
const Game *find_game(const char *set);
// The set this build was recompiled from.
const Game &game();

} // namespace rt
