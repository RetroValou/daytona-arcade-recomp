// Virtua Fighter 2, Version 2.1 (vf2; Model 2A-CRX, MAME's model2a machine:
// the board Sega Rally runs on, without its drive board and analog inputs).
// Everything the runtime needs to know about this game; the board is the
// runtime's. Seeds: seeds/vf2.txt; recompiler options: scripts/games.py.
// Played as one player: player 2's controls stay released.
#include "runtime/game.h"

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(vf2), with the SCSP sound board's 68000 program
// and samples. Not loaded: MODEL2A_VID_BOARD's mpr-16310/16311/16312, which
// MAME does not use either. No copro data ROM (MAME's region is erased to 0,
// as the importer's). The unfilled texture ROM space (0x400000-0x7fffff) is
// 0xff in MAME and 0 here.
const RomLoad kVf2Roms[] = {
    {"epr-18385.12", 0x78ED2D41, Program, 0x000000, 0x020000},
    {"epr-18386.13", 0x3418F428, Program, 0x000002, 0x020000},
    {"epr-18387.14", 0x124A8453, Program, 0x040000, 0x020000},
    {"epr-18388.15", 0x8D347980, Program, 0x040002, 0x020000},
    {"mpr-17560.10", 0xD1389864, MainData, 0x000000, 0x200000},
    {"mpr-17561.11", 0xB98D0101, MainData, 0x000002, 0x200000},
    {"mpr-17558.8", 0x4B15F5A6, MainData, 0x400000, 0x200000},
    {"mpr-17559.9", 0xD3264DE6, MainData, 0x400002, 0x200000},
    {"mpr-17566.6", 0xFB41EF98, MainData, 0x800000, 0x200000},
    {"mpr-17567.7", 0xC3396922, MainData, 0x800002, 0x200000},
    {"mpr-17564.4", 0xD8062489, MainData, 0xc00000, 0x200000},
    {"mpr-17565.5", 0x0517C6E9, MainData, 0xc00002, 0x200000},
    {"mpr-17554.16", 0x27896D82, Polygons, 0x000000, 0x200000},
    {"mpr-17548.20", 0xC95FACC2, Polygons, 0x000002, 0x200000},
    {"mpr-17555.17", 0x4DF2810B, Polygons, 0x400000, 0x200000},
    {"mpr-17549.21", 0xE0BCE0E6, Polygons, 0x400002, 0x200000},
    {"mpr-17556.18", 0x41A47616, Polygons, 0x800000, 0x200000},
    {"mpr-17550.22", 0xC36FF3F5, Polygons, 0x800002, 0x200000},
    {"mpr-17553.25", 0x5DA1C5D3, Textures, 0x000000, 0x200000},
    {"mpr-17552.24", 0xE91E7427, Textures, 0x000002, 0x200000},
    {"mpr-17547.27", 0xBE940431, Textures, 0x800000, 0x200000},
    {"mpr-17546.26", 0x042A194B, Textures, 0x800002, 0x200000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
    // the sound board: its 68000 program and the SCSP's samples
    {"epr-17574.30", 0x4D4C3A55, SoundProgram, 0x000000, 0x080000},
    {"mpr-17573.31", 0xE43557FE, Samples, 0x000000, 0x200000},
    {"mpr-17572.32", 0x4FEBECC8, Samples, 0x200000, 0x200000},
    {"mpr-17571.36", 0x51CAA584, Samples, 0x400000, 0x200000},
    {"mpr-17570.37", 0xBCCD324B, Samples, 0x600000, 0x200000},
};

// The cabinet's wiring (MAME INPUT_PORTS(vf2): model2 IN0, IN1 player 1, IN2
// player 2), from rt::Inputs as the frontends make them for every game:
// IN0 coin 0x01, Test 0x04, Service 0x08, Start 0x10 (player 2's start, 0x20,
// released); IN1 Punch 0x01, Kick 0x02, Guard 0x04 from the view buttons 1-3
// (Inputs in0 0x20, 0x40, 0x80), the joystick down 0x10, up 0x20, right 0x40,
// left 0x80 from the brake, accelerator and wheel (past half way); IN2 player
// 2, released. All active low.
uint8_t port(unsigned p, const Inputs &in) {
    switch (p) {
    case 1: return uint8_t((in.in0 & 0x1f) | 0xe0);
    case 2: {
        uint8_t v = 0xff;
        if (!(in.in0 & 0x20)) v &= uint8_t(~0x01); // punch
        if (!(in.in0 & 0x40)) v &= uint8_t(~0x02); // kick
        if (!(in.in0 & 0x80)) v &= uint8_t(~0x04); // guard
        if (in.brake >= 0x80) v &= uint8_t(~0x10); // down
        if (in.accel >= 0x80) v &= uint8_t(~0x20); // up
        if (in.steer >= 0xa0) v &= uint8_t(~0x40); // right
        if (in.steer <= 0x60) v &= uint8_t(~0x80); // left
        return v;
    }
    default: return 0xff;
    }
}

// The sound driver's voice table, as srallyc's (the same driver family, other
// code): 16 bytes per SCSP slot from 0x2800 in the 68000's RAM (A1 at every
// key-on routine: 0x600c8e, 0x600d86, 0x600da8, 0x600ecc, 0x600ee4, 0x600f02),
// byte 5 the voice's owner: 0 for the music, 1-4 for effects (none in the
// title's first 500 frames, 3-11 % of key-ons once fights start, demo or
// played). Found by logging the driver's registers at key-on (15 Oct 2026).
constexpr ScspVoices kVoices = {0x2800, 0x10, 5};
// The TGP program the i960 uploads at boot: in the program ROM at 0x3e90,
// 3,802 words, equal to MAME's copro_tgp_program after boot (dumped with a
// Lua script, 15 Oct 2026).
// The wait for the next frame: 0x10f98 ldob 0x500000,r3; 0x10fa0 cmpibe
// r3,g0,0x10f98 (the work RAM byte the vblank handler changes, as Daytona's
// 0x1394): 62 % of the i960's instructions without it (m2run --i960-profile).
} // namespace

extern const Game vf2_game = {
    .set = "vf2", .title = "Virtua Fighter 2", .label = "Version 2.1", .companion = nullptr,
    .board = BoardType::Model2a,
    .roms = kVf2Roms, .mirror_from = 0, .tgp = {true, 0x3e90, 3802, 0xFD8D0383u},
    .idle = {{0x10f98, 0x10fa3}, {0, 0}},
    .sound = SoundBoardType::Scsp, .native_sound = nullptr,
    .io = {port, nullptr}, .link = false,
    .scsp_voices = &kVoices,
    // Its reference audio against Daytona's (m2run WAVs, 8,000 frames, after
    // the first 10 s): RMS -25.3 dBFS attract, -22.8 in a fight, peak -4.7
    // (Daytona's race -14.4, peak -0.9): x1.5 (+3.5 dB) brings its peaks there.
    .output_gain = 1.5f,
    // The arcade's mix: its hits are already 13 dB below its music (a fight,
    // m2run --volumes), unlike Daytona's engine.
    .defaults = {.effects_volume = 100},
    .controls = ControlScheme::Fighting, // one player: the joystick and Punch/Kick/Guard
};

} // namespace rt
