#include "frontend/diagnostics.h"

#include "runtime/sound/sound_profile.h"

#include <psp2/power.h>

#include <algorithm>
#include <cstdio>

namespace vita {
namespace {
// Difference of two cumulative geometrizer statistics (maxima are not differenced).
rt::Geo::Stats geo_stats_delta(const rt::Geo::Stats &now, const rt::Geo::Stats &before) {
    rt::Geo::Stats d;
    d.parse_calls = now.parse_calls - before.parse_calls;
    d.inline_ticks = now.inline_ticks - before.inline_ticks;
    d.jobs = now.jobs - before.jobs;
    d.worker_ticks = now.worker_ticks - before.worker_ticks;
    d.latency_ticks = now.latency_ticks - before.latency_ticks;
    d.snapshot_ticks = now.snapshot_ticks - before.snapshot_ticks;
    d.wait_ticks = now.wait_ticks - before.wait_ticks;
    d.blocked_waits = now.blocked_waits - before.blocked_waits;
    d.count_reads = now.count_reads - before.count_reads;
    d.polys = now.polys - before.polys;
    d.failures = now.failures - before.failures;
    return d;
}
} // namespace

void Diagnostics::open(DiagnosticLog &log, unsigned main_affinity, int main_affinity_result, const SoundWorker &sound) {
    if constexpr (kPerfLog) {
        if (perf_file_.open()) {
            char text[1536];
            int n = std::snprintf(text, sizeof text,
                "start: renderer=%s cores=%s profile_window=%.0fs | main core=%d mask=0x%x result=%d | geometry core=%d | "
                "sound core=%d threaded=%d mask=0x%x result=%d\n",
                kRendererName, kPinnedCores ? "pinned" : "free", kPerfWindowSeconds, kMainCore, main_affinity,
                main_affinity_result, kGeoCore, kSoundCore, int(sound.threaded()),
                unsigned(sound.affinity_mask()), sound.affinity_result());
            if (n < 0) n = 0;
            n = std::min(n, int(sizeof text) - 1);
            n += int(CoreProfiler::format_csv_header(text + n, sizeof text - size_t(n)));
            perf_file_.submit(text, size_t(n));
        } else {
            log.fault("GPU25: cannot create %s\n", PerfLog::kPath);
        }
    }
}

void Diagnostics::perf_note(const char *line, int length) {
    if constexpr (kPerfLog) if (length > 0) perf_file_.submit(line, size_t(length));
}

void Diagnostics::game_started(rt::GameLoop &game) {
    (void)game;
#ifdef M2_TGP_PROFILE
    rt::tgp_profile.clock = ticks_us;
    rt::tgp_profile.ticks_per_second = 1000000u;
    tgp_bench_ = rt::tgp_microbench(game.board().tgp()); // about 0.1 s, once per game start
    tgp_report_.start(game.board().tgp().tgp_instructions());
#endif
#ifdef M2_I960_PROFILE
    rt::i960_profile.clock = ticks_us;
    rt::i960_profile.ticks_per_second = 1000000u;
    i960_report_.start(game.instructions(), uint64_t(game.interrupts()));
#endif
#ifdef M2_SOUND_PROFILE
    {
        snd::SoundProfile &sp = snd::sound_profile();
        sp.clock = ticks_us;
        sp.clear();
        // One clock read, removed from the probe's times.
        constexpr int kReads = 20000;
        const uint64_t t0 = ticks_us();
        for (int i = 0; i < kReads; ++i) (void)ticks_us();
        sound_clock_us_ = double(ticks_us() - t0) / kReads;
    }
#endif
}

void Diagnostics::begin_loop(uint64_t now) {
    loop_profile_ = {};
    if constexpr (kPerfLog) core_profile_.begin_loop(now);
}

void Diagnostics::input_done(uint64_t now) {
    if constexpr (kPerfLog) core_profile_.add(CoreProfiler::Input, diagnostic_ticks_us() - now);
}

Diagnostics::FrameStart Diagnostics::frame_start(rt::GameLoop &game) {
    // Work counters and geometrizer statistics before the frame (the
    // shared runtime itself is not instrumented beyond FrameProfile).
    FrameStart s;
    s.i960 = game.instructions();
#ifdef M2_SPIN_SKIP
    s.skipped = game.board().spin_skipped();
#endif
    s.tgp = game.board().tgp().tgp_instructions();
    s.geo = geometry_of(&game);
    s.geo_stats = s.geo ? s.geo->stats() : rt::Geo::Stats{};
    s.begin = diagnostic_ticks_us();
    return s;
}

void Diagnostics::frame_done(rt::GameLoop &game, const FrameStart &s, uint64_t board_end) {
    const uint64_t begin = s.begin;
    perf_run_us_ += board_end - begin;
    const auto &fp = game.last_profile();
    const rt::Geo::Stats geo_frame = s.geo ? geo_stats_delta(s.geo->stats(), s.geo_stats) : rt::Geo::Stats{};
    if constexpr (kPerfLog) {
        // Main-core board frame: game logic -> vblank start (geometry) ->
        // vblank handler -> vblank end (2D video). FrameProfile gives the
        // vblank start/end durations; the geometrizer's timestamps of its
        // parse() call (vblank start) and of the board's read of the list
        // (vblank end) split the i960+TGP time between the two phases.
        using P = CoreProfiler;
        const rt::Geo::Stats &gs = s.geo ? s.geo->stats() : s.geo_stats;
        const uint64_t wall = board_end - begin;
        uint64_t logic = 0, irq = 0;
        const bool parsed = geo_frame.parse_calls != 0 && gs.last_parse_call >= begin;
        const bool read = gs.last_output_call >= begin && gs.last_output_call <= board_end;
        if (parsed && read && gs.last_output_call >= gs.last_parse_return) {
            logic = gs.last_parse_call - begin;
            irq = gs.last_output_call - gs.last_parse_return;
        } else if (read) {
            // No parse this frame (30 Hz): the vblank handler stays in "logic".
            logic = gs.last_output_call - begin;
            logic = logic > fp.geometry ? logic - fp.geometry : 0;
        } else {
            logic = fp.core();
        }
        const uint64_t known = logic + fp.geometry + irq + fp.video;
        core_profile_.add(P::Logic, logic);
        core_profile_.add(P::GeoMain, fp.geometry);
        core_profile_.add(P::Irq, irq);
        core_profile_.add(P::Video2D, fp.video);
        const auto &vp = game.board().video().last_profile();
        core_profile_.add(P::TileCache2D, vp.tile_cache);
        core_profile_.add(P::Palette2D, vp.palette);
        core_profile_.add(P::BoardOther, wall > known ? wall - known : 0);
        core_profile_.add(P::GeoWaitBoard, geo_frame.wait_ticks);
        core_profile_.add(P::Snapshot, geo_frame.snapshot_ticks);
        auto &c = core_profile_.counters;
        ++c.board_frames;
        c.i960_instructions += game.instructions() - s.i960;
#ifdef M2_SPIN_SKIP
        c.i960_skipped += game.board().spin_skipped() - s.skipped;
#endif
        c.tgp_instructions += game.board().tgp().tgp_instructions() - s.tgp;
        auto &note = core_profile_.note();
        ++note.board_frames;
        note.i960_instructions += uint32_t(game.instructions() - s.i960);
        note.tgp_instructions += uint32_t(game.board().tgp().tgp_instructions() - s.tgp);
        c.geo_count_reads += geo_frame.count_reads;
        c.tiles_rebuilt += vp.tiles_rebuilt;
        c.characters_changed += vp.characters_changed;
        c.tile_frames += (vp.tiles_rebuilt || vp.characters_changed) ? 1 : 0;
    }
    perf_core_us_ += fp.core(); perf_geo_us_ += fp.geometry;
    perf_video_us_ += fp.video;
    ++loop_profile_.board_frames;
    loop_profile_.board += diagnostic_ticks_us() - begin;
    loop_profile_.board_core += fp.core();
    loop_profile_.board_geometry += fp.geometry;
    loop_profile_.board_video += fp.video;
    ++perf_frames_;
}

void Diagnostics::sound_joined(uint64_t begin, const SoundWorker &sound) {
    perf_sound_wait_us_ += diagnostic_ticks_us() - begin;
#ifdef M2_SOUND_PROFILE
    // The sound frame is joined: its counters are read and cleared before the next one.
    if constexpr (kPerfLog) core_profile_.counters.add_sound(snd::sound_profile());
    snd::sound_profile().clear();
#endif
    perf_sound_us_ += sound.last_sound_ticks();
    perf_audio_queue_us_ += sound.last_audio_ticks();
    ++perf_sound_frames_;
    if constexpr (kPerfLog) {
        core_profile_.add(CoreProfiler::SoundBoard, sound.last_sound_ticks());
        core_profile_.add(CoreProfiler::SoundQueue, sound.last_audio_ticks());
        ++core_profile_.counters.sound_frames;
    }
    loop_profile_.sound_wait += diagnostic_ticks_us() - begin;
    loop_profile_.sound_worker += sound.last_sound_ticks();
}

void Diagnostics::presented(GpuGlRenderer &gpu, const Present &p) {
    if constexpr (kPerfLog) {
        using P = CoreProfiler;
        core_profile_.add(P::GpuPrepare, p.ready - p.begin);
        core_profile_.add(P::GfxBegin, p.begun - p.ready);
        const uint64_t draw_total = p.drawn - p.begun;
        if (p.gameplay) {
            const uint64_t upload = gpu.last_upload_us(), layers = gpu.last_tile_us(), sort = gpu.last_sort_us();
            const uint64_t parts = upload + layers + sort;
            core_profile_.add(P::Upload, upload);
            core_profile_.add(P::Layers, layers);
            core_profile_.add(P::Sort, sort);
            core_profile_.add(P::Polygons, draw_total > parts ? draw_total - parts : 0);
            core_profile_.add(P::TexBuild, gpu.last_texture_us());
            const auto tp = gpu.take_texture_probe();
            core_profile_.add(P::TexDecode, tp.decode_us);
            core_profile_.add(P::TexUpload, tp.upload_us);
            core_profile_.add(P::TexCheck, tp.check_us);
            auto &c = core_profile_.counters;
            c.tex_builds += tp.builds;
            c.tex_texels += tp.texels;
            c.tex_rewritten += tp.rewritten;
            c.tex_kept += tp.kept;
            c.tex_changed += tp.changed;
            c.tex_frames_with_builds += tp.builds ? 1 : 0;
            c.tex_builds_max = std::max<uint64_t>(c.tex_builds_max, tp.builds);
            // material_defers counts this frame (reset by draw_polygons); cache_resets
            // since the start.
            c.tex_defers += gpu.material_defers();
            auto &note = core_profile_.note();
            note.tex_builds += tp.builds;
            note.tex_texels += uint32_t(tp.texels);
            note.tex_changed += tp.changed;
            note.tex_defers += gpu.material_defers();
            if (gpu.cache_resets() >= resets_seen_) c.tex_resets += gpu.cache_resets() - resets_seen_;
            resets_seen_ = gpu.cache_resets();
            core_profile_.add(P::VertexWrite, gpu.last_vertex_us());
            core_profile_.add(P::Worker2D, gpu.last_2d_worker_us()); // core 2, in parallel
        } else {
            core_profile_.add(P::Upload, draw_total); // the menu
        }
        // The gl.log write is diagnostics, not frame end (render/gl_context.cpp).
        const uint64_t gl_diagnostic = std::min<uint64_t>(gl_last_diagnostic_us(), p.ended - p.drawn);
        core_profile_.add(P::GfxEnd, p.ended - p.drawn - gl_diagnostic);
        core_profile_.add(P::Log, gl_diagnostic);
    }
    if (p.gameplay) {
        ++perf_presents_;
        perf_wait_us_ += p.ready - p.begin;
        perf_encode_us_ += uint64_t(gpu.last_gpu_ms() * 1000.0);
        perf_sort_us_ += gpu.last_sort_us(); perf_polygon_us_ += gpu.last_polygon_us();
        perf_tile_us_ += gpu.last_tile_us(); perf_upload_us_ += gpu.last_upload_us();
        perf_gpu_us_ += diagnostic_ticks_us() - p.begin;
    }
}

rt::Geo::Stats Diagnostics::take_geo_delta(rt::GameLoop *game) {
    const rt::Geo *geo = geometry_of(game);
    if (geo != geo_seen_ || !geo || geo->stats().parse_calls < geo_prev_.parse_calls) {
        geo_seen_ = geo;
        geo_prev_ = geo ? geo->stats() : rt::Geo::Stats{};
        return {};
    }
    const rt::Geo::Stats d = geo_stats_delta(geo->stats(), geo_prev_);
    geo_prev_ = geo->stats();
    return d;
}

void Diagnostics::loop_done(const LoopEnd &e, rt::GameLoop *game, GpuGlRenderer &gpu, AsyncLog &perf_log,
                            NativeOut &native) {
    // Geometry core work joined during this loop (folded in at each join).
    geo_delta_ = take_geo_delta(game);
    loop_profile_.menu = e.menu;
    loop_profile_.prepare = e.prepare;
    if constexpr (kDiagnostics) gl_profile_loop(loop_profile_);
    const uint64_t after = ticks_us();
    if (e.simulated) perf_frame_peak_us_ = std::max(perf_frame_peak_us_, after - e.now);
    if (after - perf_start_ >= 2000000) periodic_line(e, after, game, gpu, perf_log, native);
}

void Diagnostics::window_done(const LoopEnd &e, rt::GameLoop *game, Audio &audio, NativeOut &native,
                              const SoundWorker &sound) {
    if constexpr (kPerfLog) {
        // Windows cover gameplay only: a loop that showed or left the menu
        // (ROM import, pause) restarts the window.
        if (!game || e.menu || e.menu_at_start) {
            core_profile_.reset(ticks_us());
#ifdef M2_TGP_PROFILE
            if (game) tgp_report_.start(game->board().tgp().tgp_instructions());
#endif
#ifdef M2_I960_PROFILE
            if (game) i960_report_.start(game->instructions(), uint64_t(game->interrupts()));
#endif
        } else {
            perf_window(game, geo_delta_, audio, native, sound, e.native_audio);
        }
    }
}

void Diagnostics::periodic_line(const LoopEnd &e, uint64_t after, rt::GameLoop *game, GpuGlRenderer &gpu,
                                AsyncLog &perf_log, NativeOut &native) {
    const double sec = double(after - perf_start_) / 1000000.0;
    display_fps_ = sec > 0 ? double(perf_frames_) / sec : 0.0;
    const double run_ms = perf_frames_ ? double(perf_run_us_) / perf_frames_ / 1000.0 : 0.0;
    const double present_divisor = perf_presents_ ? double(perf_presents_) : 1.0;
    const double gpu_ms = double(perf_gpu_us_) / present_divisor / 1000.0;
    const double frame_divisor = perf_frames_ ? double(perf_frames_) : 1.0;
    const double sound_divisor = perf_sound_frames_ ? double(perf_sound_frames_) : 1.0;
    if (kDiagnostics && game) {
        const auto &vp = game->board().video().last_profile();
        const auto log_stats = perf_log.stats();
        const auto native_stats = native.stats();
        const uint64_t log_begin = ticks_us();
        perf_log.try_log("gpu25: mode=%s menu=%d frames=%u presents=%u window_ms=%.2f log_enqueue_prev_ms=%.2f log_write_prev_ms=%.2f log_write_peak_ms=%.2f log_drops=%u log_failures=%u sound_frames=%u frame_peak_ms=%.2f sim_fps=%.2f run_ms=%.2f core_ms=%.2f geo_ms=%.2f video_ms=%.2f sound_ms=%.2f gpu_submit_ms=%.2f gpu_encode_ms=%.2f gpu_wait_ms=%.2f sound_wait_ms=%.2f audio_worker_queue_ms=%.2f sort_ms=%.2f poly_ms=%.2f tiles_ms=%.2f upload_ms=%.2f cpu_mhz=%d gpu_mhz=%d cpu_raster_ms=%.2f tile_cache_ms=%.2f tile_draw_ms=%.2f compose_ms=%.2f layers=%u tiles=%u chars=%u materials=%u sources=%u builds=%u defers=%u cache_mb=%.2f material_drops=%u vertices=%u tile_uploads=%u cache_resets=%u tile_quads=%u clips=%u polys=%u/%u checker_polys=%u textured_checker_polys=%u sys24_ctrl=%04x/%04x audio=%s native_frames=%u native_last_ms=%.3f native_peak_ms=%.3f native_queue=%u native_overflows=%u native_failed=%u native_unsupported=%u native_invalid=%u native_notes=%u native_voices=%u\n",
                kRendererName, int(e.menu), unsigned(perf_frames_), unsigned(perf_presents_),
                sec * 1000.0, double(previous_log_us_) / 1000.0,
                double(log_stats.last_write_ticks) / 1000.0, double(log_stats.max_write_ticks) / 1000.0,
                unsigned(log_stats.dropped), unsigned(log_stats.failures), unsigned(perf_sound_frames_),
                double(perf_frame_peak_us_) / 1000.0, display_fps_, run_ms,
                double(perf_core_us_)/frame_divisor/1000.0, double(perf_geo_us_)/frame_divisor/1000.0,
                double(perf_video_us_)/frame_divisor/1000.0, double(perf_sound_us_)/sound_divisor/1000.0, gpu_ms, double(perf_encode_us_)/present_divisor/1000.0, double(perf_wait_us_)/present_divisor/1000.0, double(perf_sound_wait_us_)/frame_divisor/1000.0, double(perf_audio_queue_us_)/sound_divisor/1000.0,
                double(perf_sort_us_)/present_divisor/1000.0, double(perf_polygon_us_)/present_divisor/1000.0,
                double(perf_tile_us_)/present_divisor/1000.0, double(perf_upload_us_)/present_divisor/1000.0,
                scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(),
                double(vp.raster)/1000.0, double(vp.tile_cache)/1000.0, double(vp.tile_draw)/1000.0,
                double(vp.composite)/1000.0, unsigned(vp.layers_rebuilt), vp.tiles_rebuilt, vp.characters_changed,
                unsigned(gpu.cached_materials()), unsigned(gpu.cached_sources()), gpu.material_builds(), gpu.material_defers(), double(gpu.cached_bytes())/(1024.0*1024.0),
                gpu.material_drops(), unsigned(gpu.submitted_vertices()), gpu.system24_uploaded_tiles(), gpu.cache_resets(), gpu.system24_quads(), gpu.clip_changes(), gpu.textured_polys(), gpu.solid_polys(), gpu.checker_polys(), gpu.textured_checker_polys(),
                unsigned(game->board().video().system24_word(0x5004)),
                unsigned(game->board().video().system24_word(0x5006)),
                e.native_audio ? "NATIVE_TEST" : "REFERENCE", native_stats.frames,
                double(native_stats.last_us)/1000.0, double(native_stats.peak_us)/1000.0,
                native_stats.queued, native_stats.overflows, native_stats.failed,
                native_stats.unsupported, native_stats.invalid, native_stats.notes, native_stats.voices);
        previous_log_us_ = ticks_us() - log_begin;
        if constexpr (kPerfLog) core_profile_.add(CoreProfiler::Log, previous_log_us_);
    }
    perf_start_ = after; perf_frames_ = perf_run_us_ = perf_gpu_us_ = 0;
    perf_core_us_ = perf_geo_us_ = perf_video_us_ = perf_sound_us_ = 0;
    perf_presents_ = perf_wait_us_ = perf_encode_us_ = 0;
    perf_sound_wait_us_ = perf_audio_queue_us_ = perf_sound_frames_ = 0;
    perf_frame_peak_us_ = 0;
    perf_sort_us_ = perf_polygon_us_ = perf_tile_us_ = perf_upload_us_ = 0;
}

void Diagnostics::perf_window(rt::GameLoop *game, const rt::Geo::Stats &geo_delta, Audio &audio, NativeOut &native,
                              const SoundWorker &sound, bool native_audio) {
    using P = CoreProfiler;
    core_profile_.add(P::GeoParse, geo_delta.worker_ticks);
    core_profile_.add(P::Log, perf_log_pending_us_);
    perf_log_pending_us_ = 0;
    auto &c = core_profile_.counters;
    ++c.presents;
    c.geo_jobs += geo_delta.jobs;
    c.geo_polys += geo_delta.polys;
    c.geo_blocked += geo_delta.blocked_waits;
    c.geo_failures += geo_delta.failures;
    c.geo_latency += geo_delta.latency_ticks;
    c.geo_latency_max = std::max(c.geo_latency_max, geo_delta.latency_ticks);
    c.geo_parse_max = std::max(c.geo_parse_max, geo_delta.worker_ticks);
    c.geo_wait_max = std::max(c.geo_wait_max, geo_delta.wait_ticks);
    if (geo_delta.jobs) c.geo_polys_max = std::max(c.geo_polys_max, geo_delta.polys / geo_delta.jobs);
    const uint64_t loop_end = ticks_us();
    core_profile_.end_loop(loop_end);
    if (!core_profile_.due(loop_end, 1000000u, kPerfWindowSeconds)) return;
    const auto native_stats = native.stats();
    c.native_callback_last = native_stats.last_us;
    c.native_callback_peak = native_stats.peak_us;
    const auto pacing = audio.stats();
    c.audio_gaps = pacing.underruns - std::min(audio_gaps_seen_, pacing.underruns);
    audio_gaps_seen_ = pacing.underruns;
    c.audio_speed = pacing.ratio;
    c.audio_queue_ms = pacing.queued_ms;
    c.sound_clock_us = sound_clock_us_;
    CoreProfiler::Header h;
    h.index = ++perf_index_;
    const rt::Geo *geo = geometry_of(game);
    h.geo_mode = rt::geo_mode_name(geo ? geo->mode() : rt::GeoMode::Sync);
    h.renderer = kRendererName;
    h.cpu_mhz = scePowerGetArmClockFrequency();
    h.gpu_mhz = scePowerGetGpuClockFrequency();
    h.bus_mhz = scePowerGetBusClockFrequency();
    h.main_core = kMainCore; h.geo_core = kGeoCore;
    h.sound_core = kSoundCore;
    h.geo_threaded = geo && geo->worker().threaded && geo->mode() != rt::GeoMode::Sync;
    h.sound_threaded = sound.threaded();
    h.native_audio = native_audio;
    static char report[PerfLog::kCapacity];
    size_t size = core_profile_.format(report, sizeof report, h, 1000000u);
#ifdef M2_I960_PROFILE
    {
        const double frames = double(c.board_frames ? c.board_frames : 1);
        const double logic_ms = double(core_profile_.total(P::Logic) + core_profile_.total(P::Irq)) / 1000.0 / frames;
#ifdef M2_TGP_PROFILE
        const double tgp_ms = tgp_report_.ms_per_frame(c.board_frames, tgp_bench_);
#else
        const double tgp_ms = -1.0;
#endif
        size += i960_report_.format(report + size, sizeof report - size, c.board_frames, game->instructions(),
                                    uint64_t(game->interrupts()), h.cpu_mhz, tgp_ms, logic_ms,
                                    "the i960+TGP lines (game logic + vblank handler)");
        i960_report_.start(game->instructions(), uint64_t(game->interrupts()));
    }
#endif
#ifdef M2_TGP_PROFILE
    {
        const double frames = double(c.board_frames ? c.board_frames : 1);
        const double logic_ms = double(core_profile_.total(P::Logic) + core_profile_.total(P::Irq)) / 1000.0 / frames;
        size += tgp_report_.format(report + size, sizeof report - size, c.board_frames,
                                   game->board().tgp().tgp_instructions(), h.cpu_mhz, logic_ms,
                                   "the i960+TGP lines (game logic + vblank handler)", tgp_bench_);
        tgp_report_.start(game->board().tgp().tgp_instructions());
    }
#endif
    perf_file_.submit(report, size);
    const uint64_t reported = ticks_us();
    perf_log_pending_us_ = reported - loop_end;
    core_profile_.reset(reported);
}

} // namespace vita
