// Manx TT Superbike, Revision C, the Twin cabinet (manxttc; Model 2A-CRX).
// Everything the runtime needs to know about this game; the board is the
// runtime's. Seeds: seeds/manxttc.txt, seeds/manxttc_snd.txt.
#include "games/game.h"

#include <algorithm>

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(manxttc) and MODEL2_CPU_BOARD: the Twin's sound
// program and first sample ROM (the DX's Model 1 sound board is not on a Twin
// cabinet). The program is two ROM pairs (512 KB) and runs from ROM;
// main_data's last pair is mirrored (ROM_COPY 0xc00000 to 0xd00000-0xf00000).
// Not loaded: the comm board's program and MODEL2A_VID_BOARD's ROMs, as for vf2.
const RomLoad kManxttcRoms[] = {
    {"epr-18822c.12", 0xC7B3E45A, Program, 0x000000, 0x020000},
    {"epr-18823c.13", 0x6B0C1DFB, Program, 0x000002, 0x020000},
    {"epr-18824c.14", 0x352BB817, Program, 0x040000, 0x020000},
    {"epr-18825c.15", 0xF88B036C, Program, 0x040002, 0x020000},
    {"mpr-18751.10", 0x773AD43D, MainData, 0x000000, 0x200000},
    {"mpr-18752.11", 0x4DA3719E, MainData, 0x000002, 0x200000},
    {"mpr-18749.8", 0xC3FE0EEA, MainData, 0x400000, 0x200000},
    {"mpr-18750.9", 0x40B55494, MainData, 0x400002, 0x200000},
    {"mpr-18747.6", 0xA65EC1E8, MainData, 0x800000, 0x200000},
    {"mpr-18748.7", 0x375E3748, MainData, 0x800002, 0x200000},
    {"epr-18862.4", 0x9ADC3A30, MainData, 0xc00000, 0x080000},
    {"epr-18863.5", 0x603742E9, MainData, 0xc00002, 0x080000},
    {"mpr-18753.16", 0x33DDAA0D, Polygons, 0x000000, 0x200000},
    {"mpr-18756.20", 0x28713617, Polygons, 0x000002, 0x200000},
    {"mpr-18754.17", 0x09AABDE5, Polygons, 0x400000, 0x200000},
    {"mpr-18757.21", 0x25FC92E9, Polygons, 0x400002, 0x200000},
    {"mpr-18755.18", 0xBF094D9E, Polygons, 0x800000, 0x200000},
    {"mpr-18758.22", 0x1B5473D0, Polygons, 0x800002, 0x200000},
    {"mpr-18761.28", 0x4E39EC05, CoproData, 0x000000, 0x200000},
    {"mpr-18762.29", 0x4AB165D8, CoproData, 0x000002, 0x200000},
    {"mpr-18760.25", 0x4E3A4A89, Textures, 0x000000, 0x200000},
    {"mpr-18759.24", 0x278D8742, Textures, 0x000002, 0x200000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
    // the sound board: its 68000 program and the SCSP's samples (the Twin's)
    {"epr-18826.30", 0xED9FE4C1, SoundProgram, 0x000000, 0x040000},
    {"mpr-18827.31", 0x58D78CA1, Samples, 0x000000, 0x200000},
    {"mpr-18764.32", 0x0DC6A860, Samples, 0x200000, 0x200000},
    {"mpr-18765.36", 0xCA4A803C, Samples, 0x400000, 0x200000},
    {"mpr-18766.37", 0xE41892EA, Samples, 0x600000, 0x200000},
    // the default EEPROM, set to the Twin cabinet (without it the game boots as
    // the DX and waits on its moving bike's check)
    {"manxttc_twin_nvran", 0xF3BE38FE, Eeprom, 0x000000, 0x000080},
};

// The cabinet's wiring (MAME INPUT_PORTS(manxtt)) from rt::Inputs: IN0 coin
// 0x01/0x02, Test 0x04, Service 0x08, Start 0x40 from the Inputs' 0x10 (0x30
// unused); IN1 shift up 0x10 and down 0x20 from the view buttons' bits (in0
// 0x20, 0x40: Control::View1, View2); IN2 unused. All active low. The
// analog inputs (315-5649 channels 0-2: THROTTLE, BRAKE, BANK, the lean)
// rescaled from 0x20-0xe0 to the full 0x00-0xff, as Sega Rally's. BANK is
// PORT_REVERSE in MAME (a lean right reads low), so the wheel is mirrored
// about 0x80 first: unmirrored, the bike leaned left on a right input.
uint8_t full_range(uint8_t v) { return uint8_t(std::clamp((int(v) - 0x20) * 255 / 0xc0, 0, 255)); }

uint8_t port(unsigned p, const Inputs &in) {
    switch (p) {
    case 1: return uint8_t((in.in0 & 0x0f) | 0x30 | ((in.in0 & 0x10) << 2) | 0x80);
    case 2: return uint8_t(0xcf | ((in.in0 >> 1) & 0x30));
    default: return 0xff;
    }
}

uint8_t analog(unsigned ch, const Inputs &in) {
    return ch == 0 ? full_range(in.accel) : ch == 1 ? full_range(in.brake) : ch == 2 ? full_range(uint8_t(0x100 - in.steer)) : 0xff;
}

// The bike: throttle, brake, the lean (as the wheel), the shift buttons, start, coin.
constexpr ControlDef kControls[] = {
    {Control::Accelerate, "THROTTLE", PadButton::R},    {Control::Brake, "BRAKE", PadButton::L},
    {Control::SteerLeft, "LEAN LEFT", PadButton::Left}, {Control::SteerRight, "LEAN RIGHT", PadButton::Right},
    {Control::View1, "SHIFT UP", PadButton::Up},        {Control::View2, "SHIFT DOWN", PadButton::Down},
    {Control::Start, "START", PadButton::Start},        {Control::Coin, "COIN", PadButton::Select},
};

// The TGP program the i960 uploads at boot: in the program ROM at 0x787e8,
// 1,782 words, equal to MAME's copro_tgp_program after boot (Lua dump, 18 Oct
// 2026). The SCSP DSP program: in the sound program at 0x8f14, 74 steps, equal
// to MAME's MPRO after boot (read at 0x100800 from the sound 68000).
// The wait for the next frame: 0x3f1c ld 0x98000c,g4 ... cmpibe g4,g5,0x3f1c
// (until videoctl's frame bit, 0x98000c bit 2, changes; g0 counts the
// passes), as Sega Rally's: where MAME takes 87 % of the interrupts in
// attract and a race (10,000 frames).
// The SCSP driver's voice table, as VF2's, Sega Rally's and Virtua Cop 2's: 16
// bytes a slot at 0x2800 in the 68000's RAM, byte 4 the voice's MIDI channel.
// In a race (manxttc_race, 6,000 frames, 19 Oct 2026) every voice on channels
// 0-9 is the music's sequence (owner 0; 9 the drums); the engine, the brakes,
// the effects and the voices are on 0x0e and 0xff (owners 4-0x0d; 8 of 244
// owner 0). The test mode's sound test lists VOICE, EFFECT, BRAKE_P, ENGINE_P,
// ENGINE_E and B.G.M.
constexpr ScspVoices kVoices = {0x2800, 0x10, 4};
} // namespace

extern const Game manxttc_game = {
    .set = "manxttc", .title = "Manx TT Superbike", .label = "Twin, Revision C", .companion = nullptr,
    .board = BoardType::Model2a,
    .roms = kManxttcRoms, .mirror_from = 0xc00000, .tgp = {true, 0x787e8, 1782, 0xBB000CC3u},
    .idle = {{0x3f1c, 0x3f33}, {0, 0}},
    .sound = SoundBoardType::Scsp, .native_sound = nullptr,
    .io = {port, analog}, .link = false,
    .scsp_voices = &kVoices,
    // Its reference audio against Daytona's (m2run WAVs, 6,000 frames, 19 Oct
    // 2026): a race RMS -31.8 dBFS, peak -11.7; attract -36.4, -18.2 (Daytona
    // daytona93 race_basic -16.4, peak -1.0; attract -22.0, -6.0): x3.4 (+10.6
    // dB) brings its race peaks to Daytona's.
    .output_gain = 3.4f,
    // The engine is an effect, mixed by the game with the music (the Twin's
    // SCSP driver): effects at 100 %, not Daytona's 20 %. The lean softer than a
    // car's wheel on the Vita's stick: the soft curve and high smoothing.
    .defaults = {.effects_volume = 100, .steer_curve = 1, .steer_smoothing = 2},
    .control_list = kControls,
    .dsp = {0x8f14, 74, 0xAE4FDF9Cu},
    .music_channels = 0x03ff, // 0-9
    .update_z_depth_scale = 10.0f, // ln of w: race 99.9th percentile 20,900 (max 23,100)
    .parent = "manxtt", // MAME's merged manxtt.zip
};

} // namespace rt
