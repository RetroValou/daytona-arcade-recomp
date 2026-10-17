// Sega Rally Championship, Revision C (srallyc; Model 2A-CRX). Everything the
// runtime needs to know about this game; the board is the runtime's. Seeds:
// seeds/srallyc.txt; recompiler options (the boot copies its program to work
// RAM): scripts/games.py.
#include "runtime/board/game.h"

#include <algorithm>

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(srallyc), with the SCSP sound board's 68000
// program and samples. Not loaded: the drive board's and comm board's
// programs, and MODEL2A_VID_BOARD's mpr-16310/16311/16312, which MAME does
// not use either.
const RomLoad kSrallycRoms[] = {
    {"epr-17888c.12", 0x3D6808AA, Program, 0x000000, 0x080000},
    {"epr-17889c.13", 0xF43C7802, Program, 0x000002, 0x080000},
    {"mpr-17746.10", 0x8FE311F4, MainData, 0x000000, 0x200000},
    {"mpr-17747.11", 0x543593FD, MainData, 0x000002, 0x200000},
    {"mpr-17744.8", 0x71FED098, MainData, 0x400000, 0x200000},
    {"mpr-17745.9", 0x8ECCA705, MainData, 0x400002, 0x200000},
    {"mpr-17884.6", 0x4CFC95E1, MainData, 0x800000, 0x200000},
    {"mpr-17885.7", 0xA08D2467, MainData, 0x800002, 0x200000},
    {"mpr-17754.28", 0x81A84F67, CoproData, 0x000000, 0x200000},
    {"mpr-17755.29", 0x2A6E7DA4, CoproData, 0x000002, 0x200000},
    {"mpr-17748.16", 0x3148A2B2, Polygons, 0x000000, 0x200000},
    {"mpr-17750.20", 0x232AEC29, Polygons, 0x000002, 0x200000},
    {"mpr-17749.17", 0x0838D184, Polygons, 0x400000, 0x200000},
    {"mpr-17751.21", 0xED87AC62, Polygons, 0x400002, 0x200000},
    {"mpr-17753.25", 0x6DB0EB36, Textures, 0x000000, 0x200000},
    {"mpr-17752.24", 0xD6AA86CE, Textures, 0x000002, 0x200000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
    // the sound board: its 68000 program and the SCSP's samples
    {"epr-17890a.30", 0x5BAC3FA1, SoundProgram, 0x000000, 0x040000},
    {"mpr-17756.31", 0x7725F111, Samples, 0x000000, 0x200000},
    {"mpr-17757.32", 0x1616E649, Samples, 0x200000, 0x200000},
    {"mpr-17886.36", 0x54A72923, Samples, 0x400000, 0x200000},
    {"mpr-17887.37", 0x38C31FDD, Samples, 0x600000, 0x200000},
};

// The cabinet's wiring (MAME INPUT_PORTS(srallyc)) from rt::Inputs: IN0 coin
// 0x01/0x02, Test 0x04, Service 0x08, VR (view) 0x20 from any of the four
// view buttons, Start 0x40 from the Inputs' 0x10; IN1 the gearbox in bits
// 0x70 as Daytona's; IN2 the hand brake; the analog inputs rescaled from
// 0x20-0xe0 to the full 0x00-0xff.
uint8_t full_range(uint8_t v) { return uint8_t(std::clamp((int(v) - 0x20) * 255 / 0xc0, 0, 255)); }

uint8_t port(unsigned p, const Inputs &in) {
    switch (p) {
    case 1: {
        const uint8_t view = (in.in0 & 0xe0) != 0xe0 || !(in.in1 & 0x01) ? 0 : 0x20; // any VR button, active low
        return uint8_t((in.in0 & 0x0f) | 0x10 | view | ((in.in0 & 0x10) << 2) | 0x80);
    }
    case 2: return uint8_t(0x8f | (in.in1 & 0x70));
    case 3: return in.handbrake;
    default: return 0xff;
    }
}

uint8_t analog(unsigned ch, const Inputs &in) {
    return ch == 0 ? full_range(in.steer) : ch == 1 ? full_range(in.accel) : ch == 2 ? full_range(in.brake) : 0xff;
}

// The TGP program: in the program ROM at 0x5ae94, 1,665 words, equal to
// The sound driver's voice table: 16 bytes per SCSP slot from 0x2800 in the
// 68000's RAM (the key-on routines take the slot's register offset from its
// first word, 0x600eba, 0x60103a), whose byte 5 is the voice's owner at
// key-on: 0 for the music's sequence (the whole attract, music only), 1-15
// for the sound effects' (only in a race; one in the attract, at the coin's
// effect). Found by logging the driver's A1 at every key-on (15 Oct 2026).
constexpr ScspVoices kVoices = {0x2800, 0x10, 5};
// The cabinet (MAME INPUT_PORTS(srallyc)): wheel, pedals, the 4-speed gearbox, one
// view button (VR), the hand brake (IN2), start, coin.
constexpr ControlDef kControls[] = {
    {Control::Accelerate, "ACCELERATE", PadButton::R},   {Control::Brake, "BRAKE", PadButton::L},
    {Control::SteerLeft, "STEER LEFT", PadButton::Left}, {Control::SteerRight, "STEER RIGHT", PadButton::Right},
    {Control::ShiftUp, "SHIFT UP", PadButton::Up},       {Control::ShiftDown, "SHIFT DOWN", PadButton::Down},
    {Control::HandBrake, "HAND BRAKE", PadButton::Cross}, {Control::View1, "VIEW", PadButton::Triangle},
    {Control::Start, "START", PadButton::Start},         {Control::Coin, "COIN", PadButton::Select},
};
// MAME's copro_tgp_program after boot; the game uploads it from its RAM copy
// (0x5f9e94), with the count at 0x5fb898 (ROM 0x5c898).
// The wait for the next frame: 0x5a379c (in its RAM copy of the program ROM,
// at 0x479c), until videoctl_r's frame bit changes.
} // namespace

extern const Game srallyc_game = {
    .set = "srallyc", .title = "Sega Rally Championship", .label = "Revision C", .companion = nullptr,
    .board = BoardType::Model2a,
    .roms = kSrallycRoms, .mirror_from = 0, .tgp = {true, 0x5ae94, 1665, 0x072484B1u},
    .idle = {{0x5a379c, 0x5a37b3}, {0, 0}},
    .sound = SoundBoardType::Scsp, .native_sound = nullptr,
    .io = {port, analog}, .link = false,
    .scsp_voices = &kVoices,
    // Its reference audio is 20-25 dB below Daytona's (m2run WAVs, 6,000
    // frames: RMS -38.5/-47.3 dBFS in race/attract against -14.4/-21.8; peak
    // -20.9 against -0.9): x10 (+20 dB) brings its race peaks to Daytona's.
    .output_gain = 10.0f,
    // Its effects (engine, gravel, co-driver) need more than Daytona's 20 %.
    .defaults = {.effects_volume = 85},
    .control_list = kControls,
};


} // namespace rt
