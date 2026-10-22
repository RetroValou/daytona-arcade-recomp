// Dead or Alive, Revision A (doaa; Model 2A-CRX, MAME's model2a_0229 machine:
// vf2's board with Sega's 317-0229 protection chip, Game::protection). Tecmo's
// game, Dec 4 1996. Everything the runtime needs to know about this game; the
// board is the runtime's. Seeds: seeds/doaa.txt, seeds/doaa_snd.txt.
// Played as one player: player 2's controls stay released.
#include "games/game.h"

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(doaa) and MODEL2_CPU_BOARD. The program is one ROM
// pair (1 MB) and runs from ROM. main_data's ROM_COPY (0x1800000, 4 MB, to
// 0x1c00000) is the last pair loaded twice. Not loaded: the MODEL2A_VID_BOARD
// ROMs (mpr-16310/16311/16312), as for vf2. MAME's merged doa.zip names most
// of these files after the parent (Model 2B) board's sockets (mpr-19318.11
// for doaa's mpr-19318.10): the importer finds them by CRC (rom_import.cpp).
const RomLoad kDoaaRoms[] = {
    {"epr-19310a.12", 0x06486F7A, Program, 0x000000, 0x080000},
    {"epr-19311a.13", 0x1BE62912, Program, 0x000002, 0x080000},
    {"mpr-19318.10", 0xAB431BFE, MainData, 0x0000000, 0x400000},
    {"mpr-19319.11", 0xC5CB694D, MainData, 0x0000002, 0x400000},
    {"mpr-19316.8", 0x2D2D1B1A, MainData, 0x0800000, 0x400000},
    {"mpr-19317.9", 0x96B17BCF, MainData, 0x0800002, 0x400000},
    {"mpr-19314.6", 0xA8D963FB, MainData, 0x1000000, 0x400000},
    {"mpr-19315.7", 0x90AE5682, MainData, 0x1000002, 0x400000},
    {"mpr-19312.4", 0x1DCEDB10, MainData, 0x1800000, 0x200000},
    {"mpr-19313.5", 0x8C63055E, MainData, 0x1800002, 0x200000},
    {"mpr-19312.4", 0x1DCEDB10, MainData, 0x1c00000, 0x200000}, // ROM_COPY 0x1800000 -> 0x1c00000
    {"mpr-19313.5", 0x8C63055E, MainData, 0x1c00002, 0x200000},
    {"mpr-19322.16", 0xD0E6ECF0, Polygons, 0x0000000, 0x400000},
    {"mpr-19325.20", 0x7CBE432D, Polygons, 0x0000002, 0x400000},
    {"mpr-19323.17", 0x453D3F4A, Polygons, 0x0800000, 0x400000},
    {"mpr-19326.21", 0xB976DA02, Polygons, 0x0800002, 0x400000},
    {"mpr-19324.18", 0x0D6BF454, Polygons, 0x1000000, 0x400000},
    {"mpr-19327.22", 0x6A75634C, Polygons, 0x1000002, 0x400000},
    {"mpr-19321.25", 0x9C49E845, Textures, 0x000000, 0x400000},
    {"mpr-19320.24", 0x190C017F, Textures, 0x000002, 0x400000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
    // the sound board: its 68000 program and the SCSP's samples
    {"epr-19328.30", 0x400BDBFB, SoundProgram, 0x000000, 0x080000},
    {"mpr-19329.31", 0x8FD2708A, Samples, 0x000000, 0x200000},
    {"mpr-19330.32", 0x0C69787D, Samples, 0x200000, 0x200000},
    {"mpr-19331.36", 0xC18EA0B8, Samples, 0x400000, 0x200000},
    {"mpr-19332.37", 0x2877F96F, Samples, 0x600000, 0x200000},
};

// The cabinet's wiring (MAME INPUT_PORTS(doa): model2crx IN0, IN1 player 1,
// IN2 player 2), from rt::Inputs as the frontends make them for every game:
// IN0 coin 0x01, Test 0x04, Service 0x08, Start 0x10 (player 2's start, 0x20,
// released); IN1 Hold 0x01, Punch 0x02, Kick 0x04 from the view buttons 3, 1
// and 2 (Inputs in0 0x80, 0x20, 0x40: Control::Guard, Punch, Kick), the
// joystick down 0x10, up 0x20, right 0x40, left 0x80 as vf2's; IN2 player 2,
// released. All active low.
uint8_t port(unsigned p, const Inputs &in) {
    switch (p) {
    case 1: return uint8_t((in.in0 & 0x1f) | 0xe0);
    case 2: {
        uint8_t v = 0xff;
        if (!(in.in0 & 0x80)) v &= uint8_t(~0x01); // hold
        if (!(in.in0 & 0x20)) v &= uint8_t(~0x02); // punch
        if (!(in.in0 & 0x40)) v &= uint8_t(~0x04); // kick
        if (in.brake >= 0x80) v &= uint8_t(~0x10); // down
        if (in.accel >= 0x80) v &= uint8_t(~0x20); // up
        if (in.steer >= 0xa0) v &= uint8_t(~0x40); // right
        if (in.steer <= 0x60) v &= uint8_t(~0x80); // left
        return v;
    }
    default: return 0xff;
    }
}

// One player: the joystick, Punch, Kick, Hold (the fighting games' third
// button, Control::Guard), start, coin; player 2 stays released.
constexpr ControlDef kControls[] = {
    {Control::Up, "UP", PadButton::Up},           {Control::Down, "DOWN", PadButton::Down},
    {Control::Left, "LEFT", PadButton::Left},     {Control::Right, "RIGHT", PadButton::Right},
    {Control::Punch, "PUNCH", PadButton::Cross},  {Control::Kick, "KICK", PadButton::Circle},
    {Control::Guard, "HOLD", PadButton::Square},  {Control::Start, "START", PadButton::Start},
    {Control::Coin, "COIN", PadButton::Select},
};
// The TGP program the i960 uploads at boot: in the program ROM at 0x9664,
// 2,859 words, equal to MAME's copro_tgp_program after boot (Lua dump, 20 Oct
// 2026).
// The wait for the next frame, 4 instructions a pass: 0x2800 ld 0x54b150,g4;
// cmpibne 0,g4,0x2814 (out); addo g0,1,g0; b 0x2800. The vblank handler adds 1
// to that word (0x7844), the loop clears it on the way out. g0 counts the
// passes, which the caller stores at 0x54fbd0 for a debug display (0x5603c,
// "%8X"). Where MAME takes 1,685 of 1,731 interrupts in 1,800 frames of attract.
// The SCSP driver's voice table, as VF2's (the same Sega driver family): 16
// bytes a slot at 0x2800 in the 68000's RAM (A1 at both key-on routines,
// 0x602338 and 0x6024b8; its first word the slot's register offset), byte 4
// the voice's MIDI channel, byte 5 its owner. In a 6,000-frame fight (2,807
// key-ons, doaa_fight; logged at key-on, 20 Oct 2026) every voice on channels
// 0-11 is the music's sequence (owner 0; 9 the drums: a note every 0.227 s),
// 12, 14 and 15 the hits, voices and menu effects (owners 1-5, 3 of 162 owner
// 0); 13 unused.
constexpr ScspVoices kVoices = {0x2800, 0x10, 4};
} // namespace

extern const Game doaa_game = {
    .set = "doaa", .title = "Dead or Alive", .label = "Revision A", .companion = nullptr,
    .board = BoardType::Model2a,
    .roms = kDoaaRoms, .mirror_from = 0, .tgp = {true, 0x9664, 2859, 0xFB1BDB4Eu},
    .idle = {{0x2800, 0x2813}, {0, 0}},
    .sound = SoundBoardType::Scsp, .native_sound = nullptr,
    .io = {port, nullptr}, .link = false,
    .scsp_voices = &kVoices,
    // Its reference audio against Daytona's (m2run WAVs after the first 10 s, 20
    // Oct 2026, with the SCSP's MADRS mirror not written): doaa_fight 6,000
    // frames RMS -30.5 dBFS, peak -13.0; 6,000 frames of attract -30.0, peak
    // -11.0 (Daytona daytona93 race_basic -14.4, peak -0.9; at its default
    // music 100 / effects 30, -21.4): x3.2 (+10.1 dB) brings its loudest peaks
    // to Daytona's race without clipping; a fight then sits at RMS -20.4.
    .output_gain = 3.2f,
    .control_list = kControls, // one player: the joystick and Punch/Kick/Hold
    // Its SCSP DSP program (the reverb): in the sound program ROM at 0x8292, 58
    // steps, equal to MAME's MPRO after boot (read at 0x100800 from the sound
    // 68000, Lua, 20 Oct 2026).
    .dsp = {0x8292, 58, 0xF2177ADDu},
    // The wait loop's passes skipped (M2_SPIN_SKIP), g0 (r16) counting them.
    .spin = {{Game::SpinWait::WordZeroCount, 0x2800, 0x0054b150, 4, 0, 0, 0, 0, 16}},
    .music_channels = 0x0fff, // 0-11
    .protection = Protection::Comp0229Doa,
    .parent = "doa", // MAME's merged doa.zip
};

} // namespace rt
