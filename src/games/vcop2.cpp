// Virtua Cop 2 (Model 2A-CRX), one player. Everything the runtime needs to
// know about this game; the board is the runtime's. Seeds: seeds/vcop2.txt,
// seeds/vcop2_snd.txt.
#include "games/game.h"

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(vcop2) and MODEL2_CPU_BOARD at dddd7368. The
// program is two ROM pairs (2 MB) and runs from ROM; main_data's last pair is
// mirrored (ROM_COPY 0xc00000 to 0xd00000-0xf00000). Not loaded: the
// MODEL2A_VID_BOARD ROMs (mpr-16310/16311/16312), as for vf2.
const RomLoad kVcop2Roms[] = {
    {"epr-18524.12", 0x1858988B, Program, 0x000000, 0x080000},
    {"epr-18525.13", 0x0C13DF3F, Program, 0x000002, 0x080000},
    {"epr-18518.14", 0x7842951B, Program, 0x100000, 0x080000},
    {"epr-18519.15", 0x31A30EDC, Program, 0x100002, 0x080000},
    {"mpr-18516.10", 0xA3928FF0, MainData, 0x000000, 0x200000},
    {"mpr-18517.11", 0x4BD73DA4, MainData, 0x000002, 0x200000},
    {"mpr-18514.8", 0x791283C5, MainData, 0x400000, 0x200000},
    {"mpr-18515.9", 0x6BA1FFEC, MainData, 0x400002, 0x200000},
    {"mpr-18522.6", 0x61D18536, MainData, 0x800000, 0x200000},
    {"mpr-18523.7", 0x61D08DC4, MainData, 0x800002, 0x200000},
    {"epr-18520.4", 0x1D4EC5E8, MainData, 0xc00000, 0x080000},
    {"epr-18521.5", 0xB8B3781C, MainData, 0xc00002, 0x080000},
    {"mpr-18513.16", 0x777A3633, Polygons, 0x000000, 0x200000},
    {"mpr-18510.20", 0xE83DE997, Polygons, 0x000002, 0x200000},
    {"mpr-18511.24", 0xCAE77A4F, Textures, 0x000002, 0x200000},
    {"mpr-18512.25", 0xD9BC7E71, Textures, 0x000000, 0x200000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
    // the sound board: its 68000 program and the SCSP's samples
    {"epr-18530.30", 0xAC9C8357, SoundProgram, 0x000000, 0x080000},
    {"mpr-18529.31", 0xF76715B1, Samples, 0x000000, 0x200000},
    {"mpr-18528.32", 0x287A2F9A, Samples, 0x200000, 0x200000},
    {"mpr-18527.36", 0xE6A49314, Samples, 0x400000, 0x200000},
    {"mpr-18526.37", 0x6516D9B5, Samples, 0x600000, 0x200000},
};

// The cabinet's wiring (MAME INPUT_PORTS(vcop2): model2 IN0, IN1 the triggers):
// IN0 coin 0x01, Test 0x04, Service 0x08, Start 0x10 (player 2's start, 0x20,
// released); IN1 player 1's trigger 0x01 (Inputs in1 0x01, Control::Trigger),
// player 2's released; IN2 unused. All active low. The gun itself goes through
// the 315-5649's serial channel 2 (IoBoard, Game::gun).
uint8_t port(unsigned p, const Inputs &in) {
    switch (p) {
    case 1: return uint8_t((in.in0 & 0x1f) | 0xe0);
    case 2: return (in.in1 & 0x01) ? 0xff : 0xfe;
    default: return 0xff;
    }
}

// One player with the light gun: the trigger, start and coin.
constexpr ControlDef kControls[] = {
    {Control::Trigger, "TRIGGER", PadButton::Cross},
    {Control::Start, "START", PadButton::Start},
    {Control::Coin, "COIN", PadButton::Select},
};

// The TGP program the i960 uploads at boot: in the program ROM at 0x5d480,
// 1,778 words, equal to MAME's copro_tgp_program after boot (Lua dump, 18 Oct
// 2026). The SCSP DSP program: in the sound program at 0x8cc8, 84 steps, equal
// to MAME's MPRO after boot (read at 0x100800 from the sound 68000).
// The wait for the next frame: 0x36fc ldob 0x500000,r3; 0x3704 cmpibe
// r3,g0,0x36fc (until the frame counter changes, as Daytona's 0x1394): where
// MAME takes 98 % of the interrupts in 3,600 frames of attract.
// The SCSP driver's voice table, as VF2's and Sega Rally's: 16 bytes a slot
// at 0x2800 in the 68000's RAM, byte 4 the voice's MIDI channel, byte 5 its
// owner. In 9,000 frames of play (18 Oct 2026) every voice on channels 0-9 is
// the music's sequence (owner 0; 9 the drums) and channels 10-15 are the
// shots, voices and effects (owners 1-0x13, 18 of 268 owner 0), 15 included,
// unlike VF2.
constexpr ScspVoices kVoices = {0x2800, 0x10, 4};
} // namespace

extern const Game vcop2_game = {
    .set = "vcop2", .title = "Virtua Cop 2", .label = "Virtua Cop 2", .companion = nullptr,
    .board = BoardType::Model2a,
    .roms = kVcop2Roms, .mirror_from = 0xc00000, .tgp = {true, 0x5d480, 1778, 0x6B49E475u},
    .idle = {{0x36fc, 0x3707}, {0, 0}},
    .sound = SoundBoardType::Scsp, .native_sound = nullptr,
    .io = {port, nullptr}, .link = false,
    .scsp_voices = &kVoices,
    // Its reference audio against Daytona's (m2run WAVs, 18 Oct 2026): a
    // scripted 9,000-frame game RMS -32.6 dBFS, peak -15.8; attract -32.5,
    // -16.5 (Daytona daytona93 race_basic -16.4, peak -1.0; attract -22.0,
    // -6.0): x5.5 (+14.8 dB) brings its game's peaks to Daytona's race.
    // x6.9 since the SCSP's MADRS mirror is no longer written (Scsp::
    // kMadrsMirrorWrites, 20 Oct 2026): vcop2_shoot fell 2.0 dB at its peaks
    // and 4.6 dB in its loud windows (the collapsed reverb had added a wash);
    // +2.0 dB keeps the peaks where they were, more would clip (8,000 frames
    // of attract peak -17.8 dBFS at the board: -0.9 here).
    .output_gain = 6.9f,
    .control_list = kControls,
    .dsp = {0x8cc8, 84, 0xD08F9EB4u},
    // The wait loop's passes skipped (M2_SPIN_SKIP): 2 instructions a pass,
    // looping while the byte equals g0 (r16).
    .spin = {{Game::SpinWait::ByteIsReg, 0x36fc, 0x00500000, 2, 16}},
    // MAME vcop2 P1_X/P1_Y PORT_MINMAX: the screen's edges in the gun's units.
    .gun = {137, 630, 36, 425, 0x17c, 0x0e6}, // player 2: MAME's P2_X/P2_Y defaults
    .music_channels = 0x03ff, // 0-9
    .update_z_depth_scale = 7.3f, // ln of w: game 99.9th percentile 1,230 (max 1,420)
};

} // namespace rt
