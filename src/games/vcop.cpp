// Virtua Cop (original Model 2), Revision B (vcop), one player. Everything the
// runtime needs to know about this game; the board is the runtime's. Seeds:
// seeds/vcop.txt, seeds/vcop_snd.txt.
#include "games/game.h"

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(vcop) and MODEL2_CPU_BOARD at dddd7368. The
// program is two ROM pairs (512 KB); main_data has no ROM_COPY mirror.
const RomLoad kVcopRoms[] = {
    {"epr-17166b.12", 0xA5647C59, Program, 0x000000, 0x020000},
    {"epr-17167b.13", 0xF5DDE26A, Program, 0x000002, 0x020000},
    {"epr-17160a.14", 0x267F3242, Program, 0x040000, 0x020000},
    {"epr-17161a.15", 0xF7126876, Program, 0x040002, 0x020000},
    {"mpr-17164.10", 0xAC5FC501, MainData, 0x000000, 0x200000},
    {"mpr-17165.11", 0x82296D00, MainData, 0x000002, 0x200000},
    {"mpr-17162.8", 0x60DDD41E, MainData, 0x400000, 0x200000},
    {"mpr-17163.9", 0x8C1F9DC8, MainData, 0x400002, 0x200000},
    {"epr-17168a.6", 0x59091A37, MainData, 0x800000, 0x080000},
    {"epr-17169a.7", 0x0495808D, MainData, 0x800002, 0x080000},
    {"mpr-17159.16", 0xE218727D, Polygons, 0x000000, 0x200000},
    {"mpr-17156.20", 0xC4F4AABF, Polygons, 0x000002, 0x200000},
    {"mpr-17158.25", 0x1108D1EC, Textures, 0x000000, 0x200000},
    {"mpr-17157.24", 0xCF31E33D, Textures, 0x000002, 0x200000},
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},
    // Model 1 sound board (segam1audio): the 68000 program (ROM_LOAD16_WORD_SWAP)
    // and the MultiPCMs' sample ROMs (1 MB files in 2 MB slots in PCM 1)
    {"epr-17170.7", 0x06A38AE2, SoundProgram, 0x000000, 0x020000},
    {"epr-17171.8", 0xB5E436F8, SoundProgram, 0x020000, 0x020000},
    {"mpr-17172.32", 0xAB22CAC3, Pcm1, 0x000000, 0x100000},
    {"mpr-17173.33", 0x3CB4005C, Pcm1, 0x200000, 0x100000},
    {"mpr-17174.4", 0xA50369CC, Pcm2, 0x000000, 0x200000},
    {"mpr-17175.5", 0x9136D43C, Pcm2, 0x200000, 0x200000},
};

// One player with the light gun (MAME INPUT_PORTS(vcop)): the trigger is IN1
// bit 0; the gun itself goes through the I/O board (Inputs::gun_x/gun_y).
// Player 2 (IN1 bit 1, START 2, its gun) is never used.
constexpr ControlDef kControls[] = {
    {Control::Trigger, "TRIGGER", PadButton::Cross},
    {Control::Start, "START", PadButton::Start},
    {Control::Coin, "COIN", PadButton::Select},
};

// The sound driver's voice pools (Daytona's driver's layout, other offsets):
// 28 records of 12 bytes at 0xf01500 and 0xf01650, byte 3 the channel (the
// voice searches at 0xa12-0xa44 and 0xd5a-0xdbe: lea $f01500/$f01650, moveq
// #$1b, adda.l #$c).
constexpr Model1Voices kVoices{{0x1500, 0x1650}, 12, false};

// The TGP program the i960 uploads at boot: in the program ROM at 0x70c0,
// 2,024 words, equal to MAME's copro_tgp_program after boot (dumped with a
// Lua script, 18 Oct 2026).
// The wait for the next frame: 0x9124 ld 0x98000c,g4 / addo g0,1,g0 / and
// g4,4,g4 / 0x9134 cmpibe g5,g4,0x9124: until videoctl's frame bit changes,
// counting the passes in g0 (stored at 0x5016b4; read only by 0x2760, a load
// meter nothing calls). Where MAME takes 89 % of the interrupts (8,000 frames
// of attract).
} // namespace

extern const Game vcop_game = {
    .set = "vcop", .title = "Virtua Cop", .label = "Revision B", .companion = nullptr,
    .board = BoardType::Model2o,
    .roms = kVcopRoms, .mirror_from = 0, .tgp = {true, 0x70c0, 2024, 0x77D05CE6u},
    .idle = {{0x9124, 0x9137}, {0, 0}},
    .sound = SoundBoardType::Model1, .native_sound = nullptr,
    .io = {}, .link = false,
    .control_list = kControls,
    // The same wait, its passes skipped (M2_SPIN_SKIP): 4 instructions a pass,
    // looping while videoctl & 4 equals g5 (r21), each pass adding 1 to g0 (r16).
    .spin = {{Game::SpinWait::WordBitsCount, 0x9124, 0x0098000c, 4, 21, 0, 0, 4, 16}},
    // MAME vcop P1_X/P1_Y PORT_MINMAX: the screen's edges in the gun's units.
    .gun = {0x083, 0x276, 0x024, 0x1a9, 0x179, 0x0e8}, // player 2: MAME's P2_X/P2_Y defaults
    .model1_voices = &kVoices,
    // Daytona's driver's channels: the music on 0-8 and 15 (9 unused), the
    // shots and reloads on 11 (commands ae 20 01, ae 20 07).
    .music_channels = kDaytonaMusic, // 0-9 and 15
    .update_z_depth_scale = 7.2f, // ln of w: game 99.9th percentile 1,230 (max 1,320)
};

} // namespace rt
