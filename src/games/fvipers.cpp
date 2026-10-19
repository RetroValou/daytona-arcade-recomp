// Fighting Vipers, Revision D (fvipers; Model 2B-CRX, MAME's model2b machine:
// the 2A-CRX i960 board with an ADSP-21062 SHARC as the copro). Everything the
// runtime needs to know about this game; the board is the runtime's. Seeds:
// seeds/fvipers.txt; recompiler options: scripts/games.py.
// Bring-up (HANDOFF.md, 19 Oct 2026): the i960 side first, checked against
// MAME with the copro replayed from the trace; the SHARC is not native yet.
// Played as one player: player 2's controls stay released.
#include "runtime/board/game.h"

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(fvipers), with the SCSP sound board's 68000
// program and samples. main_data's two upper blocks are mirrored every 1 MB
// (ROM_COPY: 0x1000000 to 0x1700000, 0x1800000 to 0x1f00000). No TGP tables
// (no TGP on this board).
const RomLoad kFvipersRoms[] = {
    {"epr-18606d.15", 0x7334DE7D, Program, 0x000000, 0x020000},
    {"epr-18607d.16", 0x700D2ADE, Program, 0x000002, 0x020000},
    {"epr-18604d.13", 0x704FDFCF, Program, 0x040000, 0x020000},
    {"epr-18605d.14", 0x7DDDF81F, Program, 0x040002, 0x020000},
    {"mpr-18614.11", 0x0EBC899F, MainData, 0x0000000, 0x400000},
    {"mpr-18615.12", 0x018ABDB7, MainData, 0x0000002, 0x400000},
    {"mpr-18612.9", 0x1F174CD1, MainData, 0x0800000, 0x400000},
    {"mpr-18613.10", 0xF057CDF2, MainData, 0x0800002, 0x400000},
    {"epr-18610d.7", 0xA1871703, MainData, 0x1000000, 0x080000},
    {"epr-18611d.8", 0x39A75FEE, MainData, 0x1000002, 0x080000},
    {"epr-18608d.5", 0x5BC11881, MainData, 0x1800000, 0x080000},
    {"epr-18609d.6", 0xCD426035, MainData, 0x1800002, 0x080000},
    {"mpr-18622.29", 0xC74D99E3, CoproData, 0x000000, 0x200000},
    {"mpr-18623.30", 0x746AE931, CoproData, 0x000002, 0x200000},
    {"mpr-18616.17", 0x15A239BE, Polygons, 0x000000, 0x200000},
    {"mpr-18619.21", 0x9D5E8E2B, Polygons, 0x000002, 0x200000},
    {"mpr-18617.18", 0xA62CAB7D, Polygons, 0x400000, 0x200000},
    {"mpr-18620.22", 0x4D432AFD, Polygons, 0x400002, 0x200000},
    {"mpr-18618.19", 0xADAB589F, Polygons, 0x800000, 0x200000},
    {"mpr-18621.23", 0xF5EEAA95, Polygons, 0x800002, 0x200000},
    {"mpr-18626.27", 0x9DF0A961, Textures, 0x000000, 0x200000},
    {"mpr-18624.25", 0x1D74433E, Textures, 0x000002, 0x200000},
    {"mpr-18627.28", 0x946175A0, Textures, 0x800000, 0x200000},
    {"mpr-18625.26", 0x182FD572, Textures, 0x800002, 0x200000},
    // the sound board: its 68000 program and the SCSP's samples
    {"epr-18628.31", 0xAA7DD79F, SoundProgram, 0x000000, 0x080000},
    {"mpr-18629.32", 0x5D0006CC, Samples, 0x000000, 0x200000},
    {"mpr-18630.33", 0x9D405615, Samples, 0x200000, 0x200000},
    {"mpr-18631.34", 0x9DAE5B45, Samples, 0x400000, 0x200000},
    {"mpr-18632.35", 0x39DA6805, Samples, 0x600000, 0x200000},
};

// The cabinet's wiring: MAME gives fvipers INPUT_PORTS(vf2) (model2 IN0, IN1
// player 1, IN2 player 2), so the same mapping as vf2.cpp's: IN0 coin 0x01,
// Test 0x04, Service 0x08, Start 0x10; IN1 Punch 0x01, Kick 0x02, Guard 0x04
// from the view buttons 1-3, the joystick down 0x10, up 0x20, right 0x40,
// left 0x80 from the brake, accelerator and wheel; IN2 player 2, released.
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

// One player: the joystick, Punch, Kick, Guard, start, coin.
constexpr ControlDef kControls[] = {
    {Control::Up, "UP", PadButton::Up},           {Control::Down, "DOWN", PadButton::Down},
    {Control::Left, "LEFT", PadButton::Left},     {Control::Right, "RIGHT", PadButton::Right},
    {Control::Punch, "PUNCH", PadButton::Cross},  {Control::Kick, "KICK", PadButton::Circle},
    {Control::Guard, "GUARD", PadButton::Square}, {Control::Start, "START", PadButton::Start},
    {Control::Coin, "COIN", PadButton::Select},
};
// The SHARC program the i960 uploads at frame 3 (copro control bit 31 set, DMA
// channel 6 set up through the IOP port: control 0xa1, 16/48 packing, to
// 0x20000, 0x10ff+1 instructions; then the FIFO): 13,053 16-bit words, 4,351
// instructions, in main_data at 0x100000c after a header holding that DMA
// set-up. Equal to every word MAME's FIFO received in boot mode (Lua taps, 19
// Oct 2026, -nodrc). A second block with the same header at 0x100661c is not
// uploaded in attract.
// The wait for the next frame: 0x118dc ldob 0x500000,r3; 0x118e4 cmpibe
// r3,g0,0x118dc (the work RAM byte the vblank handler changes, as Daytona's
// 0x1394): where MAME takes 1,887 of 2,992 vblank interrupts in 3,000 frames
// of attract (the harvest's interrupted addresses). The other frequent ones,
// 0x4c5bc-0x4c8ac, are decompression loops. Its M2_SPIN_SKIP entry is not
// checked yet (the board does not run the game natively before the SHARC).
} // namespace

extern const Game fvipers_game = {
    .set = "fvipers", .title = "Fighting Vipers", .label = "Revision D", .companion = nullptr,
    .board = BoardType::Model2b,
    .roms = kFvipersRoms, .mirror_from = 0, .tgp = {false, 0, 0, 0},
    .mirrors = {{0x1000000, 0x1700000}, {0x1800000, 0x1f00000}},
    .sharc = {0x100000c, 13053, 0x92DCEC90u},
    .idle = {{0x118dc, 0x118e7}, {0, 0}},
    .sound = SoundBoardType::Scsp, .native_sound = nullptr,
    .io = {port, nullptr}, .link = false,
    .control_list = kControls,
    .spin = {{Game::SpinWait::ByteIsReg, 0x118dc, 0x00500000, 2, 16}},
};

} // namespace rt
