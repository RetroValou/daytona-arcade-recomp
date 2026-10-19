// SHARC profiling (Model 2B's copro): where the recompiled SHARC program's
// time goes. Built only with M2_SHARC_PROFILE (scripts/recompile.py
// --sharc-profile, CMake -DM2_SHARC_PROFILE=ON; scripts/build_vita.py
// --sharc-profile, written to perf.log; m2sharcprof for the per-address
// counts alone). Without it the runtime and the generated code are unchanged.
//
// What it measures:
//   - by caller (TgpBoard): run to the wait (vblank, the TGP's sync), need
//     the next output word (the i960 reading the output FIFO: run_generated
//     in 16-instruction calls), a 256-instruction slice (an i960 buffer RAM
//     read): calls, run_generated calls, instructions, time (one clock read
//     pair a caller's call, the clock's cost measured at start);
//   - run_generated: calls by size, chunk calls, dispatches (a chunk's
//     switch), plain run ends, pinned branches taken, delayed branches taken
//     after their slots, counter loops looping back, full steps, the
//     interpreter's steps (plain_ready), exits (waiting, budget);
//   - memory: the data space by region (internal 32-bit, the 16-bit view,
//     IOP registers, the board), program memory data, the board by kind
//     (input FIFO, empty input FIFO, output FIFO, buffer RAM, data ROM);
//   - executions per instruction of the program (M2SHARC_COUNT in the
//     generated code: a load, add and store an instruction), so the window's
//     hottest instructions and handlers, and the share of plain, pinned and
//     full instructions.
// Counters never change what the SHARC computes; they cost time (the counts
// above all), so the report's times are a profiling build's.
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef M2_SHARC_PROFILE

#include <vector>

namespace rt::sharc {

enum SharcCaller : uint8_t { SharcToWait, SharcNeedOutput, SharcSlice, kSharcCallers };

// run_generated's calls by instructions run: 0, 1-16, 17-64, 65-256, 257-4096, more.
constexpr int kSharcSizeBuckets = 6;

struct SharcProfile {
    uint64_t (*clock)() = nullptr; // host ticks (set by the frontend); null: no timing
    uint64_t ticks_per_second = 1000000;
    // TgpBoard, by caller.
    uint64_t calls[kSharcCallers]{}, runs[kSharcCallers]{}, instructions[kSharcCallers]{}, ticks[kSharcCallers]{};
    // run_generated (sharc_gen.cpp) and the chunks.
    uint64_t gen_calls = 0, chunk_calls = 0, dispatches = 0, run_ends = 0, taken = 0, slot_taken = 0, loop_backs = 0;
    uint64_t full_steps = 0, fallback_entries = 0, fallback_steps = 0, exit_wait = 0, exit_budget = 0;
    uint64_t sizes[kSharcSizeBuckets]{};
    // The data space (data_space::read_dword / write_dword) and program memory data.
    uint64_t dm_r = 0, dm_w = 0, short_r = 0, short_w = 0, iop_r = 0, iop_w = 0, ext_r = 0, ext_w = 0;
    uint64_t pm_r = 0, pm_w = 0, pm_w_code = 0; // pm_w_code: into the recompiled code's range (read twice: the rewrite check)
    // The board (SharcCopro).
    uint64_t fifo_in = 0, fifo_in_empty = 0, fifo_out = 0, buffer_r = 0, buffer_w = 0, rom_r = 0, other = 0;
};
extern SharcProfile sharc_profile;

// m2sharcrecomp: what each instruction of the program is (the generated
// sharc_gen.cpp defines them; sharc_nogen.cpp none).
enum SharcKind : uint8_t { SharcPlain, SharcPinned, SharcFull };
extern const uint32_t sharc_gen_count;
extern const char *const sharc_gen_names[];
extern const uint8_t sharc_gen_kinds[];

// The report of one window: start() at its beginning, format() at its end.
// counts: the board's per-instruction executions (adsp21062_device::set_profile).
class SharcProfileReport {
public:
    void start(uint64_t sharc_instructions, const uint32_t *counts);
    // frames: emulated frames in the window; cpu_mhz: for cycles (0: none).
    size_t format(char *out, size_t size, uint64_t frames, uint64_t sharc_instructions, const uint32_t *counts, int cpu_mhz);

private:
    SharcProfile at_{};
    uint64_t instructions_ = 0;
    std::vector<uint32_t> counts_;
    double clock_ns_ = -1; // one clock read, measured once
};

} // namespace rt::sharc

#define M2SHARC_EV(field) (++::rt::sharc::sharc_profile.field)
// A run_generated call's size, by instructions run.
#define M2SHARC_SIZE(n) (++::rt::sharc::sharc_profile.sizes[(n) == 0 ? 0 : (n) <= 16 ? 1 : (n) <= 64 ? 2 : (n) <= 256 ? 3 : (n) <= 4096 ? 4 : 5])

#else

#define M2SHARC_EV(field) ((void)0)
#define M2SHARC_SIZE(n) ((void)0)

#endif // M2_SHARC_PROFILE
