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

#include "runtime/board/cabinet_controls.h"

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
    // Player 1's light gun (games with Game::gun): where it points, in the
    // game's units (MAME's P1_X/P1_Y); at the range's edge it is off the screen.
    uint16_t gun_x = 0x17c, gun_y = 0x0e6;
};

// A light gun's range (MAME's PORT_MINMAX for P1_X/P1_Y): the values at the
// screen's edges, and player 2's gun (MAME's P2_X/P2_Y defaults: one player,
// player 2's never moves). All zero: no light gun.
struct GunRange {
    uint16_t x_min, x_max, y_min, y_max;
    uint16_t p2_x, p2_y;
    bool present() const { return x_max != 0; }
};

// One ROM file: MAME's ROM_LOAD32_WORD (16-bit words into bytes 0-1, offset
// 0, or 2-3, offset 2, of each dword) for the i960-side regions; the sound
// regions load whole files (the 68000 program and the SCSP samples
// byte-swapped: ROM_LOAD16_WORD_SWAP).
// Eeprom: the settings EEPROM's default contents (MAME's "eeprom" region, 128
// bytes: a game whose set ships them, such as manxttc's Twin cabinet setting).
enum class LoadRegion : uint8_t { Program, MainData, CoproData, Polygons, Textures, CoproTables, SoundProgram, Pcm1, Pcm2, Samples, Eeprom };
namespace rom_regions { // for the ROM tables in src/games: `using namespace rom_regions;`
inline constexpr LoadRegion Program = LoadRegion::Program, MainData = LoadRegion::MainData, CoproData = LoadRegion::CoproData,
                           Polygons = LoadRegion::Polygons, Textures = LoadRegion::Textures, CoproTables = LoadRegion::CoproTables,
                           SoundProgram = LoadRegion::SoundProgram, Pcm1 = LoadRegion::Pcm1, Pcm2 = LoadRegion::Pcm2,
                           Samples = LoadRegion::Samples, Eeprom = LoadRegion::Eeprom;
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

// The SCSP DSP program the sound driver copies into the SCSP's MPRO registers
// at boot: in the 68000's program ROM at `offset` (byte offset in
// sound_program.bin, big-endian words as there), `steps` steps of 4 words,
// CRC-32 `crc` of those bytes. m2import cuts it out (dsp_program.bin) and
// m2dsprecomp recompiles it; steps 0: none (the DSP interpreter only).
struct ScspDspProgram {
    uint32_t offset = 0, steps = 0, crc = 0;
};

// The SCSP sound driver's voice table in the 68000's RAM, which says what
// each SCSP slot plays: `entry` bytes per slot from `table`, the voice's MIDI
// channel at `channel` (0-8 and 15: the music's; 9-14 and 0xff: sound effects
// and voices). Read at each key-on and while the slot plays, for the music
// and effects volumes (snd::ScspBoard::set_volumes).
struct ScspVoices {
    uint32_t table, entry, channel;
};

// The Model 1 sound driver's voice pools in the 68000's RAM (offsets from
// 0xf00000): two pools of 28 records, `stride` bytes each. Byte 0 is zero when
// free (bit 3: the MultiPCM), byte 1 the slot code it writes to the chip, byte
// 3 the owning channel; with `reserved_ff6`, byte 6 0xff marks the engine
// layers' reserved records (Daytona). Read at each key-on, for the music and
// effects volumes (snd::SoundBoard::voice_channel).
struct Model1Voices {
    uint32_t pool[2];
    uint32_t stride;
    bool reserved_ff6;
};
// Daytona's driver (the allocator at 0x1688).
inline constexpr Model1Voices kDaytonaVoices{{0x1500, 0x1618}, 10, true};
// Daytona's driver's music channels, 0-9 and 15: the ones its "stop music"
// command (0x50 0x7e) stops (Game::music_channels).
inline constexpr uint16_t kDaytonaMusic = 0x83ff;

// A frontend's default settings for the game (the Vita's: platform/vita
// VitaSettings; RESET DEFAULTS goes back to them). Volumes in percent.
struct FrontendDefaults {
    int music_volume = 100;
    // The effects against the music: Daytona's effects cover its music on the
    // Vita's speakers, so 20 % by default.
    int effects_volume = 100;
    // The Vita's steering curve (0 linear, 1 soft, 2 extra soft) and smoothing
    // (0 off, 1 low, 2 high: the wheel's top speed, platform/vita/frontend/
    // controls.h kSteerRate): a wheel turns rather than jumps, so low by default
    // (the d-pad's left/right too); Manx TT's lean softer still.
    int steer_curve = 0, steer_smoothing = 1;
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
    // Its cabinet's controls (runtime/board/cabinet_controls.h), as the frontends list and
    // bind them; each game's own (Daytona's four view buttons, Sega Rally's one and
    // its hand brake, Virtua Fighter 2's joystick and three buttons).
    std::span<const ControlDef> control_list;
    // SCSP board: its DSP program, recompiled (tools/m2dsprecomp), or none.
    ScspDspProgram dsp{};
    // Wait loops of the game's whose passes the board skips (M2_SPIN_SKIP,
    // M2Board::spin_skip): each pass starts with an ldob of a work RAM byte (or,
    // WordBitsCount, an ld of a device register) at `ip`. Between two lockstep
    // events (interrupts, board callbacks) nothing changes that value, so the
    // passes up to just before the next event (and,
    // with a timer, before the pass that would leave on it) are skipped by
    // moving the instruction count on: the state is the one those passes leave.
    // A wrong entry changes the game silently: check a new one with m2run's
    // frame dumps, WAV and counts against a build without M2_SPIN_SKIP.
    struct SpinWait {
        enum Kind : uint8_t {
            None,
            ByteIsReg,     // loops while the byte equals register `reg`
            ByteZeroTimer, // loops while the byte is 0 and timer `timer`'s count, masked to
                           // 20 bits and read `timer_at` instructions into the pass, is not
                           // 0xfffff and above register `reg`
            WordBitsCount, // loops while the 32-bit device word ANDed with `mask` equals
                           // register `reg`, adding 1 to register `counter` each pass
        } kind = None;
        uint32_t ip = 0, addr = 0; // the pass's first instruction (the ldob/ld) and the byte/word
        uint8_t pass = 0;          // instructions in a pass
        uint8_t reg = 0;           // Cpu::m_r index (r13: 13, g0: 16)
        uint8_t timer = 0, timer_at = 0;
        uint32_t mask = 0;         // WordBitsCount: the bits the loop compares
        uint8_t counter = 0;       // WordBitsCount: the register the passes count in
    };
    SpinWait spin[2]{}; // unused entries: None
    // The light gun (IoBoard): player 1's range, player 2's fixed aim. Model 1
    // I/O board with the interface 837-12079 (Virtua Cop) or the 2A-CRX
    // 315-5649's serial channel 2 (Virtua Cop 2). None: no gun.
    GunRange gun{};
    // Model 1 sound board: its driver's voice pools (nullptr: Daytona's).
    const Model1Voices *model1_voices = nullptr;
    // Which of the sound driver's channels play the music (bit n: channel n,
    // 0-15); every other channel, and a voice no channel owns, is an effect.
    // The music and effects volumes split the voices by it (both sound boards
    // and the native engine; music_channel below). 0: no split.
    uint16_t music_channels = 0;
    // Vita renderer, hybrid depth test (upgrade_z_depth, render/shaders.cpp): the
    // local slope's share of the depth range, update_z_depth_LOCAL_WEIGHT (0: the Model 2 order
    // alone; up to 0.9999: closer to a true z-buffer). Each game's own.
    float update_z_depth_local_weight = 0.9f;
    // The same test: the distance scale, update_z_depth_SCALE, the natural log of
    // the farthest vertex distance w the game draws (its 99.9th percentile:
    // nearness 1 at w 1, 0 there and beyond). Each game's own, measured on the PC.
    float update_z_depth_scale = 12.5f;
    // The same test: Model 2 windows that share one depth band (each range,
    // first to last, inclusive), so the hybrid depth mixes them as one scene;
    // every other window keeps its own band, in front of the windows before
    // it. Found by drawing each window alone on the PC. Unused entries: {0, 0}.
    struct WindowRange {
        uint8_t first = 0, last = 0;
    };
    WindowRange update_z_depth_window_groups[4]{};
};

// Channel `channel` of the game's sound driver plays the music (Game::music_channels).
inline bool music_channel(const Game &g, int channel) {
    return channel >= 0 && channel < 16 && (g.music_channels >> channel & 1);
}


// The game's sound board can mix its music and effects at their own volumes
// (snd::Board::set_volumes): the Model 1 board (its driver's voice pools) or
// an SCSP board whose driver's voice table is known.
inline bool has_music_effects(const Game &g) {
    return g.music_channels != 0 && (g.sound == SoundBoardType::Model1 || g.scsp_voices != nullptr);
}

// Every game the runtime knows (src/games/games.cpp).
std::span<const Game *const> games();
// The record for a set, or nullptr.
const Game *find_game(const char *set);
// The set this build was recompiled from.
const Game &game();

} // namespace rt
