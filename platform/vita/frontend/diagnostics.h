#pragma once

// What the frontend measures, kept out of the game loop: the per-core report of
// perf.log (system/core_profile.h), the periodic "gpu25:" line of vita-diag.log,
// the gl.log loop profile, the geometrizer's statistics and the TGP/i960 profile
// sections. All of it exists only in diagnostics builds (scripts/build_vita.py
// --diagnostics), except the shown frame rate, which the menu prints.

#include "audio/audio.h"
#include "audio/native_audio.h"
#include "audio/sound_worker.h"
#include "render/renderer.h"
#include "runtime/board/game_loop.h"
#include "runtime/cpu/i960_profile.h"
#include "runtime/sound/native/native_sound_engine.h"
#include "runtime/tgp/tgp_profile.h"
#include "runtime/video/geo.h"
#include "system/async_log.h"
#include "system/core_profile.h"
#include "system/diagnostic_log.h"

#include <psp2/kernel/processmgr.h>

#include <cstdint>

#ifndef DAYTONA_VITA_DIAGNOSTICS
#define DAYTONA_VITA_DIAGNOSTICS 0
#endif
// Core of each busy thread (CMake DAYTONA_VITA_*_CORE; -1 = unpinned, --free-core).
#ifndef DAYTONA_VITA_MAIN_CORE
#define DAYTONA_VITA_MAIN_CORE 0
#endif
#ifndef DAYTONA_VITA_GEO_CORE
#define DAYTONA_VITA_GEO_CORE 1
#endif
#ifndef DAYTONA_VITA_SOUND_CORE
#define DAYTONA_VITA_SOUND_CORE 2
#endif

namespace vita {

constexpr const char *kRendererName = "VITAGL"; // in the menu's title and the logs
constexpr bool kDiagnostics = DAYTONA_VITA_DIAGNOSTICS != 0;
// Every log file (vita-diag.log periodic lines, perf.log, gl.log) and the profiling
// clocks exist only in diagnostic builds: scripts/build_vita.py --diagnostics.
// Faults are always appended to vita-diag.log.
constexpr bool kPerfLog = kDiagnostics;
constexpr bool kProfile = kDiagnostics;
constexpr int kMainCore = DAYTONA_VITA_MAIN_CORE, kGeoCore = DAYTONA_VITA_GEO_CORE,
              kSoundCore = DAYTONA_VITA_SOUND_CORE;
constexpr bool kPinnedCores = kMainCore >= 0 || kGeoCore >= 0 || kSoundCore >= 0;
constexpr double kPerfWindowSeconds = 5.0;

// Microseconds. SDL2's Vita performance counter is this same call at 1 MHz; calling it
// directly skips SDL and a 64-bit division per read (measured 1.39 us per read through SDL,
// 0.70 us direct): the profiling clocks of a diagnostics build cost half as much.
inline uint64_t ticks_us() { return sceKernelGetProcessTimeWide(); }
// Measurement only; never use this clock to pace game or audio playback.
inline uint64_t diagnostic_ticks_us() { return kProfile ? ticks_us() : 0; }

// The game's geometrizer (platform/vita/overrides/runtime/video/geo.h), found from its
// board's TGP buffer RAM: M2Board itself is the unmodified shared runtime.
inline rt::Geo *geometry_of(rt::GameLoop *game) {
    return game ? rt::Geo::find(game->board().tgp().buffer_data()) : nullptr;
}

class Diagnostics {
public:
    using NativeOut = NativeAudio<snd::NativeSoundEngine>;

    // perf.log opened with its start line (diagnostics builds).
    void open(DiagnosticLog &log, unsigned main_affinity, int main_affinity_result, const SoundWorker &sound);
    void close() { perf_file_.close(); }
    // A line of perf.log (the geometry thread's set-up).
    void perf_note(const char *line, int length);

    // A new game: the TGP/i960/sound probes started on it.
    void game_started(rt::GameLoop &game);
    void geometry_changed() { geo_seen_ = nullptr; } // the per-loop statistics re-baselined

    void begin_loop(uint64_t now);
    void input_done(uint64_t now);

    // One emulated frame, around GameLoop::run_frame_sound_packet.
    struct FrameStart {
        uint64_t i960 = 0, skipped = 0, tgp = 0, begin = 0;
        const rt::Geo *geo = nullptr;
        rt::Geo::Stats geo_stats;
    };
    FrameStart frame_start(rt::GameLoop &game);
    void frame_done(rt::GameLoop &game, const FrameStart &start, uint64_t board_end);
    void sound_sync(uint64_t begin) {
        if constexpr (kPerfLog) core_profile_.add(CoreProfiler::SoundSync, diagnostic_ticks_us() - begin);
    }
    // The sound worker joined (begin: when the main core started waiting).
    void sound_joined(uint64_t begin, const SoundWorker &sound);
    void sound_failed(uint64_t begin) { perf_sound_wait_us_ += diagnostic_ticks_us() - begin; }

    // A presented loop (timestamps of diagnostic_ticks_us; gameplay: a game frame, not the menu).
    struct Present {
        uint64_t begin = 0, ready = 0, begun = 0, drawn = 0, ended = 0;
        bool gameplay = false;
    };
    void presented(GpuGlRenderer &gpu, const Present &p);

    // End of a loop, in two parts with the loop's idle delay between them (as measured
    // before): gl.log's loop profile and the periodic vita-diag line, then perf.log's window.
    struct LoopEnd {
        uint64_t now = 0;              // when the loop started
        uint64_t prepare = 0;          // GpuGlRenderer::prepare_frame
        bool menu = false, menu_at_start = false, simulated = false, native_audio = false;
    };
    void loop_done(const LoopEnd &e, rt::GameLoop *game, GpuGlRenderer &gpu, AsyncLog &perf_log, NativeOut &native);
    void window_done(const LoopEnd &e, rt::GameLoop *game, Audio &audio, NativeOut &native, const SoundWorker &sound);

    double display_fps() const { return display_fps_; } // emulated frames per second, the menu's

private:
    rt::Geo::Stats take_geo_delta(rt::GameLoop *game);
    void periodic_line(const LoopEnd &e, uint64_t after, rt::GameLoop *game, GpuGlRenderer &gpu, AsyncLog &perf_log,
                       NativeOut &native);
    void perf_window(rt::GameLoop *game, const rt::Geo::Stats &geo_delta, Audio &audio, NativeOut &native,
                     const SoundWorker &sound, bool native_audio);

    // Per-core profiling (perf.log, diagnostics builds). Reports cover gameplay only.
    CoreProfiler core_profile_;
    PerfLog perf_file_;
    unsigned perf_index_ = 0;
    unsigned audio_gaps_seen_ = 0;      // reference audio underruns already reported
    uint64_t perf_log_pending_us_ = 0;  // report formatting time, charged to the next window
    double sound_clock_us_ = 0.0;       // one ticks_us read (sound probe, M2_SOUND_PROFILE)
    unsigned resets_seen_ = 0;          // renderer texture cache resets already counted
#ifdef M2_TGP_PROFILE
    // TGP profiling (build_vita.py --tgp-profile): a TGP section in each perf.log report.
    rt::TgpBench tgp_bench_;
    rt::TgpProfileReport tgp_report_;
#endif
#ifdef M2_I960_PROFILE
    // i960 profiling (build_vita.py --i960-profile): an i960 section in each perf.log report.
    rt::I960ProfileReport i960_report_;
#endif
    // Per-loop geometry statistics; re-baselined when the board changes.
    const rt::Geo *geo_seen_ = nullptr;
    rt::Geo::Stats geo_prev_;
    rt::Geo::Stats geo_delta_;   // this loop's, from loop_done to window_done
    GlLoopProfile loop_profile_; // this iteration, sent to gl.log at its end

    // The periodic vita-diag line's window (every 2 s).
    uint64_t perf_start_ = ticks_us(), perf_frames_ = 0, perf_run_us_ = 0, perf_gpu_us_ = 0;
    uint64_t perf_core_us_ = 0, perf_geo_us_ = 0, perf_video_us_ = 0, perf_sound_us_ = 0;
    uint64_t perf_presents_ = 0, perf_wait_us_ = 0, perf_encode_us_ = 0;
    uint64_t perf_sound_wait_us_ = 0, perf_audio_queue_us_ = 0, perf_sound_frames_ = 0;
    uint64_t perf_sort_us_ = 0, perf_polygon_us_ = 0, perf_tile_us_ = 0, perf_upload_us_ = 0;
    uint64_t previous_log_us_ = 0, perf_frame_peak_us_ = 0;
    double display_fps_ = 0.0;
};

} // namespace vita
