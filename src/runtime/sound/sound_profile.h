// Sound board profiling: where the SCSP sound board's time goes (the 68000,
// the SCSP's rendering, its DSP), as tgp_profile.h does for the TGP. Built
// only with M2_SOUND_PROFILE, which the Vita defines in diagnostics builds
// (build_vita.py --diagnostics; written to perf.log). Without it the runtime
// and the generated code are unchanged.
//
// What it measures, per sound board frame (ScspBoard::advance):
//   - time: advance() as a whole; one render_to in kRenderEvery of those that
//     render samples (they run inside the 68000's register accesses and
//     scheduled events, so the 68000's own time is advance minus the renders);
//     the DSP step of one sample in kDspEvery. A clock read costs more than a
//     sample: sampled times are scaled up by the samples rendered;
//   - the 68000: instructions, dispatches (switch entries of the recompiled
//     code: returns, indirect jumps, interrupt entries, budget boundaries;
//     m2sndrecomp emits M2_SND_DISPATCH()), SCSP register reads and writes
//     (each one first renders up to the current instruction count);
//   - render_to calls (each a 64-bit division) and the ones that rendered.
// Counters never change what the board computes.
#pragma once

#include <cstdint>

namespace snd {

struct SoundProfile {
    static constexpr uint32_t kDspEvery = 16, kRenderEvery = 16;
    uint64_t (*clock)() = nullptr; // host ticks (set by the frontend); null: counts only
    uint64_t frames = 0, advance_ticks = 0;
    uint64_t m68k_instructions = 0, dispatches = 0, scsp_reads = 0, scsp_writes = 0;
    uint64_t render_calls = 0, render_runs = 0, samples = 0;
    uint64_t render_timed = 0, render_timed_samples = 0, render_ticks = 0;
    uint64_t dsp_timed = 0, dsp_ticks = 0, dsp_in_timed_render = 0;
    uint32_t dsp_phase = 0, render_phase = 0;
    bool timing_render = false;

    void clear() {
        auto *const c = clock;
        *this = {};
        clock = c;
    }
};

inline SoundProfile &sound_profile() {
    static SoundProfile profile;
    return profile;
}

} // namespace snd
