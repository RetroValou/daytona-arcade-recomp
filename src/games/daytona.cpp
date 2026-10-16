// Daytona USA (original Model 2): Deluxe '93 (daytona93) and Revision A, 1994
// (daytona). Everything the runtime needs to know about this game; the board
// is the runtime's. Seeds: seeds/daytona93.txt, seeds/daytona.txt; hooks:
// seeds/daytona93_hooks.txt, seeds/daytona_hooks.txt.
#include "games/hooks.h"
#include "runtime/enhance.h"
#include "runtime/game.h"

#include <algorithm>
#include <cstdlib>

namespace rt {

namespace {

using namespace rom_regions;

// MAME model2.cpp ROM_START(daytona93), ROM_START(daytona) and
// MODEL2_CPU_BOARD at dddd7368. The parts both sets share come first.
#define SHARED_LOADS                                                         \
    {"mpr-16528.10", 0x9CE591F6, MainData, 0x000000, 0x200000},              \
    {"mpr-16529.11", 0xF7095EAF, MainData, 0x000002, 0x200000},              \
    {"mpr-16537.ic28", 0x36B7C35A, CoproData, 0x000000, 0x200000},           \
    {"mpr-16536.ic29", 0x6D6AFED9, CoproData, 0x000002, 0x200000},           \
    {"mpr-16523.ic16", 0x2F484D42, Polygons, 0x000000, 0x200000},            \
    {"mpr-16518.ic20", 0xDF683BF7, Polygons, 0x000002, 0x200000},            \
    {"mpr-16524.ic17", 0x34658BD7, Polygons, 0x400000, 0x200000},            \
    {"mpr-16519.ic21", 0xFACD1C81, Polygons, 0x400002, 0x200000},            \
    {"mpr-16525.ic18", 0xFB517521, Polygons, 0x800000, 0x200000},            \
    {"mpr-16520.ic22", 0xD66BD9BD, Polygons, 0x800002, 0x200000},            \
    {"mpr-16522.25", 0x55D39A57, Textures, 0x000000, 0x200000},              \
    {"mpr-16521.24", 0xAF1934FB, Textures, 0x000002, 0x200000},              \
    {"opr-14742a.45", 0x90C6B117, CoproTables, 0x000000, 0x020000},          \
    {"opr-14743a.46", 0xAE7F446B, CoproTables, 0x000002, 0x020000},          \
    /* Model 1 sound board (segam1audio): the MultiPCMs' sample ROMs */      \
    {"mpr-16491.32", 0x89920903, Pcm1, 0x000000, 0x200000},                  \
    {"mpr-16492.33", 0x459E701B, Pcm1, 0x200000, 0x200000},                  \
    {"mpr-16493.4", 0x9990DB15, Pcm2, 0x000000, 0x200000},                   \
    {"mpr-16494.5", 0x600E1D6C, Pcm2, 0x200000, 0x200000}

const RomLoad kDaytona93Roms[] = {
    {"epr-16530a.12", 0x39E962B5, Program, 0x000000, 0x020000},
    {"epr-16531a.13", 0x693126EB, Program, 0x000002, 0x020000},
    {"mpr-16526.8", 0x5273B8B5, MainData, 0x400000, 0x200000},
    {"mpr-16527.9", 0xFC4CB0EF, MainData, 0x400002, 0x200000},
    {"epr-16534a.6", 0x1BB0D72D, MainData, 0x800000, 0x100000},
    {"epr-16535a.7", 0x459A8BFB, MainData, 0x800002, 0x100000},
    {"epr-16646.ic19", 0x7BA9FD6B, Polygons, 0xC00000, 0x080000},
    {"epr-16645.ic23", 0x78FE0B8A, Polygons, 0xC00002, 0x080000},
    {"mpr-16517.27", 0x4705D3DD, Textures, 0x800000, 0x200000},
    {"mpr-16516.26", 0xA260D45D, Textures, 0x800002, 0x200000},
    // the sound board's 68000 program (ROM_LOAD16_WORD_SWAP)
    {"epr-16489.7", 0xC20E543E, SoundProgram, 0x000000, 0x020000},
    {"epr-16490.8", 0xC24EDAAB, SoundProgram, 0x020000, 0x020000},
    SHARED_LOADS,
};

const RomLoad kDaytonaRoms[] = {
    {"epr-16722a.12", 0x48B94318, Program, 0x000000, 0x020000},
    {"epr-16723a.13", 0x8AF8B32D, Program, 0x000002, 0x020000},
    {"mpr-16808.8", 0x44F1F5A0, MainData, 0x400000, 0x200000},
    {"mpr-16809.9", 0x37A2DD12, MainData, 0x400002, 0x200000},
    {"epr-16724a.6", 0x469F10FD, MainData, 0x800000, 0x080000},
    {"epr-16725a.7", 0xBA0DF8DB, MainData, 0x800002, 0x080000},
    {"mpr-16772.ic19", 0x770ED912, Polygons, 0xC00000, 0x200000},
    {"mpr-16771.ic23", 0xA2205124, Polygons, 0xC00002, 0x200000},
    {"mpr-16770.27", 0xF9FA7BFB, Textures, 0x800000, 0x200000},
    {"mpr-16769.26", 0xE57429E9, Textures, 0x800002, 0x200000},
    {"epr-16720.7", 0x8E73CFFD, SoundProgram, 0x000000, 0x020000},
    {"epr-16721.8", 0x1BB3B7B7, SoundProgram, 0x020000, 0x020000},
    SHARED_LOADS,
};
#undef SHARED_LOADS

// The sound driver's tables (found from the driver's reachable command
// handlers: addresses, not copies of any table). Revision A's sound program
// has them from the profiles on 0x48 bytes later.
constexpr SoundLayout kDaytona93Sound = {0x2bc6, 0x505a, 0x523a, 0x56ea, 0x5c1a, 0x5cce, 0x5dce, 0x645a};
constexpr SoundLayout kDaytonaSound = {0x2bc6, 0x505a + 0x48, 0x523a + 0x48, 0x56ea + 0x48,
                                       0x5c1a + 0x48, 0x5cce + 0x48, 0x5dce + 0x48, 0x645a + 0x48};

// The TGP program the i960 uploads at boot: the same 2,024 words in both sets
// (found by matching MAME's upload), in main_data.
// The wait-for-vblank loops: 0x12b0 (until the frame counter at 0x00500000
// changes) and 0x12f0 (until it reaches 2). MAME takes 99% of vblank
// interrupts there; the rest land in CPU-bound code such as the boot-time
// texture upload at 0x1388. The same in both sets.
} // namespace

// The cabinet (MAME INPUT_PORTS(daytona)): wheel, pedals, the 4-speed gearbox,
// four view buttons (VR1-4), start, coin.
constexpr ControlDef kControls[] = {
    {Control::Accelerate, "ACCELERATE", PadButton::R},   {Control::Brake, "BRAKE", PadButton::L},
    {Control::SteerLeft, "STEER LEFT", PadButton::Left}, {Control::SteerRight, "STEER RIGHT", PadButton::Right},
    {Control::ShiftUp, "SHIFT UP", PadButton::Up},       {Control::ShiftDown, "SHIFT DOWN", PadButton::Down},
    {Control::Start, "START", PadButton::Start},         {Control::Coin, "COIN", PadButton::Select},
    {Control::View1, "VIEW 1 (RED)", PadButton::Cross},  {Control::View2, "VIEW 2 (BLUE)", PadButton::Circle},
    {Control::View3, "VIEW 3 (YELLOW)", PadButton::Square}, {Control::View4, "VIEW 4 (GREEN)", PadButton::Triangle},
};

extern const Game daytona93_game = {
    .set = "daytona93", .title = "Daytona USA", .label = "Deluxe '93", .companion = "daytona",
    .board = BoardType::Model2o,
    .roms = kDaytona93Roms, .mirror_from = 0x900000, .tgp = {false, 0x860020, 2024, 0xD6D611DDu},
    .idle = {{0x12b0, 0x12bb}, {0x12f0, 0x12ff}},
    .sound = SoundBoardType::Model1, .native_sound = &kDaytona93Sound,
    .io = {}, .link = false, .draw_distance = true, // hook_draw_list
    .control_list = kControls,
};
extern const Game daytona_game = {
    .set = "daytona", .title = "Daytona USA", .label = "Revision A, 1994", .companion = "daytona93",
    .board = BoardType::Model2o,
    .roms = kDaytonaRoms, .mirror_from = 0x800000, .tgp = {false, 0x800020, 2024, 0xD6D611DDu},
    .idle = {{0x12b0, 0x12bb}, {0x12f0, 0x12ff}},
    .sound = SoundBoardType::Model1, .native_sound = &kDaytonaSound,
    .io = {}, .link = true, // the communication board (837-10537)
    .draw_distance = true,  // hook_draw_list
    .control_list = kControls,
    // The wait for the next frame, 2 instructions a pass: 0x1394 ldob 0x500000,r3;
    // 0x139c cmpibe r3,g0,0x1394. 76 % of the i960's instructions in a race on
    // the Dreamcast (platform/dreamcast/HANDOFF.md; dcmemcheck matched the
    // desktop at its 100 checkpoints with the skip). daytona93 has no such loop.
    .spin = {{Game::SpinWait::ByteIsReg, 0x1394, 0x00500000, 2, 16}},
};


// --- Hooks (seeds/daytona93_hooks.txt, seeds/daytona_hooks.txt) --------------

// The draw list holds at most 63 cells (0x5016c1..0x5016ff; the game uses
// 0x501700), so the widest square is 7x7 (49).
// The game's polygon budget per frame: objects stop being drawn once their
// costs pass it (0x17a78, 0x1786c); set once at boot (0x1210: 0x1388). The
// hardware's limit, not ours: more cells need a bigger budget, or they come
// last in the list and are never drawn.
constexpr uint32_t kBudgetAddr = 0x5010f4, kGameBudget = 5000;
constexpr uint32_t kDrawCount = 0x5016c0, kDrawCells = 0x5016c1;

void hook_draw_list(Cpu &c) {
    const int level = std::clamp(Enhance::draw_distance, Enhance::kDrawMin, Enhance::kDrawMax);
    static bool raised = false;
    const uint32_t budget = level > 0 ? kGameBudget * uint32_t(1 + level) : kGameBudget;
    if (level > 0 || raised) {
        c.bus->write_dword(kBudgetAddr, budget);
        raised = level > 0;
    }
    if (level == 0) return;
    const uint32_t car = c.m_r[8];
    if (car > 255) return; // not a cell: leave the game's list alone
    const int cx = int(car & 15), cy = int(car >> 4);
    uint8_t list[63];
    int n = 0;
    if (level < 0) {
        // Less: the game's own list, cut to the car's cell (-2) or one cell around it (-1).
        const int keep = level == -1 ? 1 : 0;
        const int count = std::min<int>(c.bus->read_byte(kDrawCount), 63);
        for (int i = 0; i < count; i++) {
            const int cell = c.bus->read_byte(kDrawCells + uint32_t(i));
            const int dx = (cell & 15) - cx, dy = (cell >> 4) - cy;
            if (std::max(std::abs(dx), std::abs(dy)) <= keep) list[n++] = uint8_t(cell);
        }
    } else {
        // More: every cell within 2 (+1: the whole 5x5 the game chooses from)
        // or 3 (+2: 7x7) of the car's, nearest ring first, inside the
        // 16x16 grid (cells are bytes; no wrapping into the next row).
        const int radius = 1 + level;
        for (int r = 0; r <= radius; r++)
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || x > 15 || y < 0 || y > 15) continue;
                    list[n++] = uint8_t(x + 16 * y);
                }
    }
    c.bus->write_byte(kDrawCount, uint8_t(n));
    for (int i = 0; i < n; i++) c.bus->write_byte(kDrawCells + uint32_t(i), list[i]);
}

} // namespace rt
