// Per-core time accounting for the Vita GPU frontend (perf.log).
//
// Every loop iteration of frontend/main.cpp (one presented frame) adds the time of
// each measured section; a report is produced every few seconds of gameplay.
// For each core it gives the share of the core's wall time, the average per
// presented frame and the worst frame, so the sections to optimize first can
// be ranked. Pure bookkeeping: no SDK calls, host-testable, no allocation.
//
//   core 0  main thread: input, i960 + TGP, geometry kick (or the whole parse
//           in SYNC mode), 2D video, sound hand-off, GPU recording/submission
//   core 1  geometry thread (platform/vita/overrides/runtime/geo.cpp): the parse
//   core 2  sound worker (reference sound board: 68000 + MultiPCM + FM) and
//           the vitaGL 2D worker (System 24 uploads + layer rectangles)
//
// "Nested" sections are already contained in a main section (shown indented,
// never added twice). "idle/unmeasured" is the main core's remaining time:
// vsync waits outside the swap, SDL_Delay, unmeasured code.
//
// PerfLog (end of file) writes the reports to ux0:data/<set>/perf.log from
// a background thread.
#pragma once

#include <SDL.h>
#include <psp2/io/fcntl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "runtime/sound/sound_profile.h"

namespace vita {

class CoreProfiler {
public:
    enum Section : uint8_t {
        // Main core, in loop order. These add up to the measured main-core time.
        Input,        // pad, menu logic, frame clock
        Logic,        // i960 + TGP: game frame up to its wait-for-vblank
        GeoMain,      // vblank start: snapshot + start of the parse (ASYNC) or whole parse (SYNC)
        Irq,          // i960 + TGP: vblank interrupt handler
        Video2D,      // vblank end: System 24 tile cache / layers (Video::screen_update)
        BoardOther,   // scheduler, probes, sound byte hand-off inside the board frame
        SoundSync,    // join of the sound worker + dispatch of the next packet / native send
        GpuPrepare,   // renderer prepare_frame (cache resets)
        GfxBegin,     // frame begin: GPU fence wait + clear
        Upload,       // waiting for the 2D worker (or its job inline without the thread)
        Layers,       // 2D layer quads recording
        Sort,         // polygon priority sort
        Polygons,     // polygon recording (vertices, materials, batches)
        GfxEnd,       // frame end: GL/GXM submission + swap (vsync)
        Log,          // diagnostic formatting / enqueue
        kMainSections,
        // Nested in the main sections above.
        GeoWaitBoard = kMainSections, // main core blocked on the geometry thread (vblank start/end, count reads)
        Snapshot,            // buffer RAM copy (inside GeoMain)
        TexBuild,            // texture builds (inside Polygons)
        VertexWrite,         // vertex writing into GPU memory (inside Polygons)
        TexDecode,           // texture builds: texel decode + transparent fill (inside TexBuild)
        TexUpload,           // texture builds: make_texture / glTexImage2D (inside TexBuild)
        TexCheck,            // texture RAM checks of rewritten sources (inside TexBuild)
        Palette2D,           // dirty palette to pens (inside Video2D)
        TileCache2D,         // System 24 tile cache, palette included (inside Video2D)
        // Other cores.
        GeoParse,            // core 1: Geo::parse
        SoundBoard,          // sound thread: sound board frame
        SoundQueue,          // sound thread: sample conversion/queueing
        Worker2D,            // 2D worker: System 24 uploads + layer rectangles
        kSections
    };

    struct Info { const char *name; int core; bool nested; };
    static const Info &info(Section s) {
        static const Info table[kSections] = {
            {"input + menu + frame clock", 0, false},
            {"i960+TGP game logic (to vblank)", 0, false},
            {"geometry on main core", 0, false},
            {"i960+TGP vblank handler", 0, false},
            {"2D video update (System 24)", 0, false},
            {"board other (scheduler, probes)", 0, false},
            {"sound sync / dispatch", 0, false},
            {"renderer prepare_frame", 0, false},
            {"frame begin (GPU wait + clear)", 0, false},
            {"2D: wait for the core 2 worker", 0, false},
            {"2D layer recording", 0, false},
            {"polygon priority sort", 0, false},
            {"polygon recording", 0, false},
            {"frame end (submit + swap/vsync)", 0, false},
            {"logging", 0, false},
            {"(nested) WAIT for the geometry core", 0, true},
            {"of which buffer RAM snapshot", 0, true},
            {"of which texture builds", 0, true},
            {"of which vertex writing", 0, true},
            {"  texture decode", 0, true},
            {"  texture upload (glTexImage2D)", 0, true},
            {"  texture RAM checks", 0, true},
            {"of which palette", 0, true},
            {"of which tile cache (palette incl.)", 0, true},
            {"display list parse", 1, false},
            {"sound board (68000+PCM+FM)", 2, false},
            {"sample conversion/queue", 2, false},
            {"2D worker (uploads + rectangles)", 2, false},
        };
        return table[s];
    }

    // Window counters (summed by the caller, reported per board frame).
    struct Counters {
        uint64_t board_frames = 0, presents = 0;
        uint64_t i960_instructions = 0, tgp_instructions = 0;
        uint64_t i960_skipped = 0; // of i960_instructions: skipped in the game's wait loop (M2_SPIN_SKIP)
        uint64_t sharc_instructions = 0, sharc_us = 0; // Model 2B's SHARC (inside the i960+TGP lines)
        uint64_t geo_jobs = 0, geo_polys = 0, geo_polys_max = 0, geo_blocked = 0, geo_failures = 0;
        uint64_t geo_count_reads = 0, geo_latency = 0, geo_latency_max = 0, geo_wait_max = 0;
        uint64_t geo_parse_max = 0;
        uint64_t sound_frames = 0;
        uint64_t native_callback_peak = 0, native_callback_last = 0; // native audio (SDL callback thread)
        // Reference audio pacing (audio_rate.h), read at the report.
        uint64_t audio_gaps = 0;     // underruns in this window (one short silence each)
        double audio_speed = 1.0;    // playback speed (1 = real time; < 1: the emulation is slow)
        double audio_queue_ms = 0.0; // averaged queue level (target 64 ms)
        // Texture builds (vitaGL) and the System 24 tile cache, for their optimisation.
        uint64_t tex_builds = 0, tex_texels = 0, tex_rewritten = 0, tex_kept = 0, tex_changed = 0, tex_defers = 0, tex_resets = 0;
        uint64_t tex_frames_with_builds = 0, tex_builds_max = 0;
        uint64_t tiles_rebuilt = 0, characters_changed = 0, tile_frames = 0;
        // Sound board probe (diagnostics builds, runtime/sound/sound_profile.h): summed
        // over the window's sound frames; sound_clock_us is one clock read (removed).
        bool sound_probe = false;
        snd::SoundProfile sound{};
        double sound_clock_us = 0.0;
        void add_sound(const snd::SoundProfile &p) {
            sound_probe = true;
            sound.frames += p.frames; sound.advance_ticks += p.advance_ticks;
            sound.m68k_instructions += p.m68k_instructions; sound.dispatches += p.dispatches;
            sound.scsp_reads += p.scsp_reads; sound.scsp_writes += p.scsp_writes;
            sound.render_calls += p.render_calls; sound.render_runs += p.render_runs; sound.samples += p.samples;
            sound.render_timed += p.render_timed; sound.render_timed_samples += p.render_timed_samples;
            sound.render_ticks += p.render_ticks;
            sound.dsp_timed += p.dsp_timed; sound.dsp_ticks += p.dsp_ticks;
            sound.dsp_in_timed_render += p.dsp_in_timed_render;
            sound.slot_timed += p.slot_timed; sound.slot_ticks += p.slot_ticks;
            sound.slot_in_timed_render += p.slot_in_timed_render;
            sound.active_slots += p.active_slots; sound.plfo_slots += p.plfo_slots; sound.alfo_slots += p.alfo_slots;
            sound.mod_slots += p.mod_slots; sound.pcm8_slots += p.pcm8_slots;
        }
    };

    // What one loop iteration did besides its section times, for the slow-frame list
    // (set by the caller between begin_loop and end_loop).
    struct LoopNote {
        uint32_t tex_builds = 0, tex_texels = 0, tex_defers = 0, tex_changed = 0;
        uint32_t i960_instructions = 0, tgp_instructions = 0, board_frames = 0;
    };
    LoopNote &note() { return note_; }
    static constexpr int kSlowKept = 8; // the slowest loops of a window, listed in the report

    void reset(uint64_t now) {
        totals_ = {}; maxima_ = {}; hits_ = {}; current_ = {};
        counters = {};
        loops_ = 0; loop_max_ = 0;
        slow_count_ = 0;
        window_start_ = now; window_end_ = now;
        started_ = true;
    }
    // A loop iteration starts. Times added before end_loop belong to it.
    void begin_loop(uint64_t now) {
        if (!started_) reset(now);
        current_ = {};
        note_ = {};
        loop_start_ = now;
    }
    void add(Section s, uint64_t ticks) { current_[s] += ticks; }
    // Folds the iteration into the window.
    void end_loop(uint64_t now) {
        for (int i = 0; i < kSections; ++i) {
            const uint64_t v = current_[size_t(i)];
            if (!v) continue;
            totals_[size_t(i)] += v;
            maxima_[size_t(i)] = std::max(maxima_[size_t(i)], v);
            ++hits_[size_t(i)];
        }
        const uint64_t loop = now >= loop_start_ ? now - loop_start_ : 0;
        loop_max_ = std::max(loop_max_, loop);
        keep_if_slow(loop);
        ++loops_;
        window_end_ = now;
    }
    uint64_t window_ticks() const { return window_end_ >= window_start_ ? window_end_ - window_start_ : 0; }
    bool due(uint64_t now, uint64_t ticks_per_second, double seconds) const {
        return started_ && loops_ && now >= window_start_ &&
               double(now - window_start_) >= seconds * double(ticks_per_second);
    }
    uint64_t loops() const { return loops_; }
    uint64_t total(Section s) const { return totals_[size_t(s)]; }

    struct Header {
        unsigned index = 0;
        const char *geo_mode = "";
        int cpu_mhz = 0, gpu_mhz = 0, bus_mhz = 0;
        int main_core = -1, geo_core = -1, sound_core = -1;
        bool geo_threaded = false, sound_threaded = false, native_audio = false;
        const char *renderer = "";
    };

    // Writes the report (several lines). Returns the length written.
    size_t format(char *out, size_t size, const Header &h, uint64_t ticks_per_second) const {
        Writer w{out, size};
        const double tps = double(ticks_per_second ? ticks_per_second : 1);
        const double window_s = double(window_ticks()) / tps;
        // Averages per EMULATED frame: main-loop iterations without a board frame (the host
        // ahead of the 57.52 Hz clock) do almost nothing and are not frames.
        const double loops = double(counters.board_frames ? counters.board_frames : (loops_ ? loops_ : 1));
        const double frames = double(counters.board_frames ? counters.board_frames : 1);
        auto ms = [&](uint64_t ticks) { return double(ticks) * 1000.0 / tps; };
        auto pct = [&](uint64_t ticks) { return window_ticks() ? 100.0 * double(ticks) / double(window_ticks()) : 0.0; };
        constexpr double kBoardHz = 16000000.0 / (656.0 * 424.0);

        uint64_t main_sum = 0;
        for (int i = 0; i < kMainSections; ++i) main_sum += totals_[size_t(i)];
        const uint64_t idle = window_ticks() > main_sum ? window_ticks() - main_sum : 0;

        w.add("==== perf %u: %.2f s gameplay | geo=%s | renderer=%s | CPU %d MHz GPU %d MHz BUS %d MHz ====\n",
              h.index, window_s, h.geo_mode, h.renderer, h.cpu_mhz, h.gpu_mhz, h.bus_mhz);
        w.add("frames: emulated %.2f fps (target %.2f), main loop %.0f/s, worst loop %.2f ms, board frame budget %.2f ms\n",
              window_s > 0 ? double(counters.board_frames) / window_s : 0.0, kBoardHz,
              window_s > 0 ? double(loops_) / window_s : 0.0, ms(loop_max_), 1000.0 / kBoardHz);
        w.add("cores: main=%d geometry=%d (%s) sound=%d (%s)\n", h.main_core, h.geo_core,
              h.geo_threaded ? "thread" : "main core", h.sound_core,
              h.native_audio ? "native audio callback" : (h.sound_threaded ? "thread" : "main core"));
        w.add("%-40s %7s %9s %9s %6s\n", "section", "core%", "ms/frame", "max ms", "hits");

        w.add("CORE 0 main: busy %.1f%% (%.2f ms per emulated frame)\n", pct(main_sum), ms(main_sum) / loops);
        for (int i = 0; i < kMainSections; ++i) {
            line(w, Section(i), "  ", ms, pct, loops);
            // Nested detail right below its parent.
            if (i == GeoMain) line(w, Snapshot, "    ", ms, pct, loops);
            if (i == BoardOther) line(w, GeoWaitBoard, "    ", ms, pct, loops);
            if (i == Video2D) {
                line(w, TileCache2D, "    ", ms, pct, loops);
                line(w, Palette2D, "    ", ms, pct, loops);
            }
            if (i == Polygons) {
                line(w, TexBuild, "    ", ms, pct, loops);
                line(w, TexDecode, "    ", ms, pct, loops);
                line(w, TexUpload, "    ", ms, pct, loops);
                line(w, TexCheck, "    ", ms, pct, loops);
                line(w, VertexWrite, "    ", ms, pct, loops);
            }
        }
        w.add("  %-38s %7.1f %9.2f\n", "idle / unmeasured (vsync, delay)", pct(idle), ms(idle) / loops);
        if (counters.tex_builds || counters.tex_rewritten || counters.tex_defers || counters.tex_resets)
            w.add("  textures: %llu built (%.0f texels each, %.2f us per 1000 texels decode, %.2f upload) in %llu frames, "
                  "most %llu in a frame | texture RAM written under %llu: %llu kept (same words), %llu rebuilt | "
                  "%llu deferred, %llu cache resets\n",
                  ull(counters.tex_builds), counters.tex_builds ? double(counters.tex_texels) / double(counters.tex_builds) : 0.0,
                  counters.tex_texels ? 1e6 * ms(totals_[TexDecode]) / double(counters.tex_texels) : 0.0,
                  counters.tex_texels ? 1e6 * ms(totals_[TexUpload]) / double(counters.tex_texels) : 0.0,
                  ull(counters.tex_frames_with_builds), ull(counters.tex_builds_max), ull(counters.tex_rewritten),
                  ull(counters.tex_kept), ull(counters.tex_changed), ull(counters.tex_defers), ull(counters.tex_resets));
        if (counters.tile_frames)
            w.add("  tile cache: %.1f tiles and %.1f characters rebuilt per frame (%llu frames with work)\n",
                  double(counters.tiles_rebuilt) / frames, double(counters.characters_changed) / frames,
                  ull(counters.tile_frames));

        w.add("CORE 1 geometry: busy %.1f%%\n", pct(totals_[GeoParse]));
        line(w, GeoParse, "  ", ms, pct, loops);
        if (counters.geo_jobs) {
            w.add("  parses %llu, avg %.2f ms, worst %.2f ms | polys avg %.0f worst %llu | start->done avg %.2f ms worst %.2f ms\n",
                  ull(counters.geo_jobs), ms(totals_[GeoParse]) / double(counters.geo_jobs), ms(counters.geo_parse_max),
                  double(counters.geo_polys) / double(counters.geo_jobs), ull(counters.geo_polys_max),
                  ms(counters.geo_latency) / double(counters.geo_jobs), ms(counters.geo_latency_max));
            w.add("  main core blocked on %llu of %llu joins (worst wait %.2f ms), polygon count reads %llu, failures %llu\n",
                  ull(counters.geo_blocked), ull(counters.geo_jobs), ms(counters.geo_wait_max),
                  ull(counters.geo_count_reads), ull(counters.geo_failures));
        } else {
            w.add("  idle: the geometry runs on the main core (SYNC) or no 3D frame\n");
        }

        w.add("CORE 2 (sound + 2D worker): busy %.1f%% at most (the sound board line also counts the 2D worker when it interrupts the sound)\n", pct(totals_[SoundBoard] + totals_[SoundQueue] + totals_[Worker2D]));
        line(w, SoundBoard, "  ", ms, pct, loops);
        line(w, SoundQueue, "  ", ms, pct, loops);
        line(w, Worker2D, "  ", ms, pct, loops);
        if (h.native_audio)
            w.add("  native audio callback: last %.3f ms, peak %.3f ms (SDL audio thread)\n",
                  ms(counters.native_callback_last), ms(counters.native_callback_peak));
        else
            w.add("  reference audio: %llu gaps, playback speed %.2f%%, queue %.1f ms (target 64)\n",
                  ull(counters.audio_gaps), counters.audio_speed * 100.0, counters.audio_queue_ms);

        if (counters.sound_probe && counters.sound.frames) {
            const snd::SoundProfile &p = counters.sound;
            const double sf = double(p.frames), c = counters.sound_clock_us / 1000.0; // ms per clock read
            // A timed render holds one read of its own and two per DSP step timed inside it;
            // advance holds one of its own and two per timed render and DSP step. Renders are
            // timed one in kRenderEvery: scaled by the samples rendered.
            const double reads = double(1 + 2 * p.render_timed + 2 * p.dsp_timed + 2 * p.slot_timed); // inside advance
            const double render_timed = std::max(0.0, ms(p.render_ticks) -
                c * double(p.render_timed + 2 * p.dsp_in_timed_render + 2 * p.slot_in_timed_render));
            const double render = p.render_timed_samples
                                      ? render_timed * double(p.samples) / double(p.render_timed_samples) : 0.0;
            const double advance = std::max(0.0, ms(p.advance_ticks) - c * reads);
            const double dsp_sample = p.dsp_timed ? std::max(0.0, ms(p.dsp_ticks) / double(p.dsp_timed) - c) : 0.0;
            const double dsp = dsp_sample * double(p.samples);
            w.add("  sound probe, per sound frame (%llu frames): board %.2f ms = 68000 %.2f ms + SCSP render ~%.2f ms "
                  "(of which DSP ~%.2f ms, %.2f us a sample from %llu timed, slots/mix ~%.2f ms) | probe clock %.3f us a read, "
                  "~%.2f ms a frame of reads removed\n",
                  ull(p.frames), advance / sf, std::max(0.0, advance - render) / sf, render / sf, dsp / sf, dsp_sample * 1000.0,
                  ull(p.dsp_timed), std::max(0.0, render - dsp) / sf, counters.sound_clock_us, c * reads / sf);
            const double slot_sample = p.slot_timed ? std::max(0.0, ms(p.slot_ticks) / double(p.slot_timed) - c) : 0.0;
            const double slots = slot_sample * double(p.samples);
            const double samples = p.samples ? double(p.samples) : 1.0, active = double(p.active_slots);
            const auto share = [&](uint64_t n) { return active > 0 ? 100.0 * double(n) / active : 0.0; };
            w.add("  SCSP render per sound frame: slots ~%.2f ms (%.2f us a sample from %llu timed, %.1f active: "
                  "pitch LFO %.0f%%, amplitude LFO %.0f%%, modulation %.0f%%, 8-bit %.0f%%), DSP ~%.2f ms, "
                  "output mix and the rest ~%.2f ms | %.0f samples a frame\n",
                  slots / sf, slot_sample * 1000.0, ull(p.slot_timed), active / samples, share(p.plfo_slots),
                  share(p.alfo_slots), share(p.mod_slots), share(p.pcm8_slots), dsp / sf,
                  std::max(0.0, render - dsp - slots) / sf, double(p.samples) / sf);
            w.add("  68000 per sound frame: %.0f instructions, %.0f dispatches (switch entries), SCSP registers %.0f reads + "
                  "%.0f writes | render_to %.0f calls, %.0f rendering, %.1f samples each\n",
                  double(p.m68k_instructions) / sf, double(p.dispatches) / sf, double(p.scsp_reads) / sf,
                  double(p.scsp_writes) / sf, double(p.render_calls) / sf, double(p.render_runs) / sf,
                  p.render_runs ? double(p.samples) / double(p.render_runs) : 0.0);
        }

        w.add("work per emulated frame: i960 %.0f instr (%.0f of them skipped in the wait loop), TGP %.0f instr "
              "(both inside the i960+TGP lines), polygons %.0f\n",
              double(counters.i960_instructions) / frames, double(counters.i960_skipped) / frames,
              double(counters.tgp_instructions) / frames,
              counters.geo_jobs ? double(counters.geo_polys) / double(counters.geo_jobs) : 0.0);
        if (counters.sharc_instructions)
            w.add("SHARC (Model 2B, recompiled; inside the i960+TGP lines): %.0f instr, %.2f ms a frame, %.2f M instr/s\n",
                  double(counters.sharc_instructions) / frames, double(counters.sharc_us) / 1000.0 / frames,
                  counters.sharc_us ? double(counters.sharc_instructions) / double(counters.sharc_us) : 0.0);

        // Ranking of the main-core sections: what to optimize first.
        std::array<int, kMainSections> order{};
        for (int i = 0; i < kMainSections; ++i) order[size_t(i)] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return totals_[size_t(a)] > totals_[size_t(b)]; });
        w.add("PRIORITY main core:");
        for (int rank = 0; rank < 6; ++rank) {
            const int s = order[size_t(rank)];
            if (!totals_[size_t(s)]) break;
            w.add(" %d) %s %.1f%% (%.2f ms)", rank + 1, info(Section(s)).name, pct(totals_[size_t(s)]),
                  ms(totals_[size_t(s)]) / loops);
        }
        w.add("\n");

        // The slowest loops of the window, each with what it spent (sections of 0.3 ms
        // or more, largest first): where a visible hitch comes from.
        if (slow_count_) {
            w.add("SLOW loops (the %d longest of the window; t = seconds into it):\n", slow_count_);
            for (int k = 0; k < slow_count_; ++k) {
                const Slow &f = slow_[size_t(k)];
                w.add("  t=%5.2f s %6.2f ms |", ms(f.at) / 1000.0, ms(f.total));
                std::array<int, kSections> by{};
                for (int i = 0; i < kSections; ++i) by[size_t(i)] = i;
                std::sort(by.begin(), by.end(), [&](int a, int b) { return f.s[size_t(a)] > f.s[size_t(b)]; });
                for (int i : by) {
                    if (ms(f.s[size_t(i)]) < 0.3) break;
                    w.add(" %s %.2f,", csv_name(Section(i)), ms(f.s[size_t(i)]));
                }
                w.add(" | %u board frames, i960 %u, TGP %u instr | textures %u built (%u texels), %u changed, %u deferred\n",
                      f.note.board_frames, f.note.i960_instructions, f.note.tgp_instructions, f.note.tex_builds,
                      f.note.tex_texels, f.note.tex_changed, f.note.tex_defers);
            }
        }

        // One machine-readable line: averages in ms per shown frame.
        w.add("csv,%u,%.3f,%s,%.2f,%.2f", h.index, window_s, h.geo_mode,
              window_s > 0 ? double(counters.board_frames) / window_s : 0.0, window_s > 0 ? double(loops_) / window_s : 0.0);
        for (int i = 0; i < kSections; ++i) w.add(",%.3f", ms(totals_[size_t(i)]) / loops);
        w.add(",%.3f\n", ms(idle) / loops);
        return w.used;
    }

    // Column names of the csv line, written once at the top of perf.log.
    static size_t format_csv_header(char *out, size_t size) {
        Writer w{out, size};
        w.add("csv,window,seconds,geo_mode,emulated_fps,loops_per_s"); // section columns: ms per emulated frame
        for (int i = 0; i < kSections; ++i) w.add(",%s", csv_name(Section(i)));
        w.add(",idle\n");
        return w.used;
    }

    Counters counters;

private:
    struct Writer {
        char *out;
        size_t size, used = 0;
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        void add(const char *format, ...) {
            if (!out || used + 1 >= size) return;
            va_list args;
            va_start(args, format);
            const int n = std::vsnprintf(out + used, size - used, format, args);
            va_end(args);
            if (n > 0) used = std::min(size - 1, used + size_t(n));
        }
    };
    static unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }
    static const char *csv_name(Section s) {
        static const char *const names[kSections] = {
            "input", "logic", "geo_main", "irq", "video2d", "board_other", "sound_sync", "gpu_prepare",
            "gfx_begin", "upload", "layers", "sort", "polygons", "gfx_end", "log",
            "geo_wait_board", "snapshot", "tex_build", "vertex_write", "tex_decode", "tex_upload", "tex_check",
            "palette_2d", "tile_cache_2d", "geo_parse", "sound_board", "sound_queue", "worker_2d"};
        return names[s];
    }
    template<class Ms, class Pct>
    void line(Writer &w, Section s, const char *indent, Ms ms, Pct pct, double loops) const {
        const uint64_t total = totals_[s];
        char label[64];
        std::snprintf(label, sizeof label, "%s%s", indent, info(s).name);
        w.add("%-40s %7.1f %9.2f %9.2f %6llu\n", label, pct(total), ms(total) / loops, ms(maxima_[s]), ull(hits_[s]));
    }

    struct Slow {
        uint64_t at = 0, total = 0; // start in the window, duration
        std::array<uint64_t, kSections> s{};
        LoopNote note;
    };
    // Kept sorted, slowest first.
    void keep_if_slow(uint64_t loop) {
        if (slow_count_ == kSlowKept && loop <= slow_[kSlowKept - 1].total) return;
        int k = slow_count_ < kSlowKept ? slow_count_++ : kSlowKept - 1;
        while (k > 0 && slow_[size_t(k - 1)].total < loop) { slow_[size_t(k)] = slow_[size_t(k - 1)]; --k; }
        Slow &f = slow_[size_t(k)];
        f.at = loop_start_ >= window_start_ ? loop_start_ - window_start_ : 0;
        f.total = loop;
        f.s = current_;
        f.note = note_;
    }

    std::array<uint64_t, kSections> totals_{}, maxima_{}, hits_{}, current_{};
    std::array<Slow, kSlowKept> slow_{};
    int slow_count_ = 0;
    LoopNote note_{};
    uint64_t loops_ = 0, loop_max_ = 0, loop_start_ = 0, window_start_ = 0, window_end_ = 0;
    bool started_ = false;
};

// ux0:data/<set>/perf.log (M2_ROMSET), truncated at launch. A report submitted while
// the previous one is still being written is dropped (and counted): the
// gameplay thread never waits for memory-card I/O.

class PerfLog {
public:
#ifdef M2_ROMSET
    static constexpr const char *kPath = "ux0:data/" M2_ROMSET "/perf.log";
#else
    static constexpr const char *kPath = "ux0:data/daytona93/perf.log";
#endif
    static constexpr std::size_t kCapacity = 24 * 1024;      // one report (the i960 and SHARC profiles add ~8 KB)
    static constexpr std::size_t kFileLimit = 8u * 1024u * 1024u;

    PerfLog() = default;
    PerfLog(const PerfLog &) = delete;
    PerfLog &operator=(const PerfLog &) = delete;
    ~PerfLog() { close(); }

    bool open() {
        close();
        written_ = 0;
        const SceUID fd = sceIoOpen(kPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (fd < 0) return false;
        sceIoClose(fd);
        mutex_ = SDL_CreateMutex();
        ready_ = SDL_CreateCond();
        idle_ = SDL_CreateCond();
        if (!mutex_ || !ready_ || !idle_) { close(); return false; }
        stopping_ = busy_ = false;
        thread_ = SDL_CreateThreadWithStackSize(entry, "Daytona perf log", 32u * 1024u, this);
        if (!thread_) { close(); return false; }
        return true;
    }
    bool opened() const { return thread_ != nullptr; }
    uint64_t dropped() const { return dropped_; }
    unsigned written() const { return written_reports_.load(); }

    // Copies the text; never waits for I/O.
    bool submit(const char *text, std::size_t size) {
        if (!thread_ || !size) { ++dropped_; return false; }
        size = size > buffer_.size() ? buffer_.size() : size;
        if (SDL_TryLockMutex(mutex_) != 0) { ++dropped_; return false; }
        if (busy_) { SDL_UnlockMutex(mutex_); ++dropped_; return false; }
        std::memcpy(buffer_.data(), text, size);
        size_ = size;
        busy_ = true;
        SDL_CondSignal(ready_);
        SDL_UnlockMutex(mutex_);
        return true;
    }

    void close() noexcept {
        if (thread_) {
            SDL_LockMutex(mutex_);
            while (busy_) SDL_CondWait(idle_, mutex_); // let the last report reach the card
            stopping_ = true;
            SDL_CondSignal(ready_);
            SDL_UnlockMutex(mutex_);
            SDL_WaitThread(thread_, nullptr);
            thread_ = nullptr;
        }
        if (idle_) SDL_DestroyCond(idle_);
        if (ready_) SDL_DestroyCond(ready_);
        if (mutex_) SDL_DestroyMutex(mutex_);
        idle_ = ready_ = nullptr;
        mutex_ = nullptr;
    }

private:
    static int SDLCALL entry(void *opaque) {
        auto &self = *static_cast<PerfLog *>(opaque);
        SDL_LockMutex(self.mutex_);
        for (;;) {
            while (!self.busy_ && !self.stopping_) SDL_CondWait(self.ready_, self.mutex_);
            if (self.stopping_ && !self.busy_) break;
            SDL_UnlockMutex(self.mutex_);
            self.write(self.buffer_.data(), self.size_);
            SDL_LockMutex(self.mutex_);
            self.busy_ = false;
            ++self.written_reports_;
            SDL_CondSignal(self.idle_);
        }
        SDL_UnlockMutex(self.mutex_);
        return 0;
    }
    void write(const char *data, std::size_t size) {
        if (written_ + size > kFileLimit) return;
        const SceUID fd = sceIoOpen(kPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
        if (fd < 0) return;
        while (size) {
            const int n = sceIoWrite(fd, data, size);
            if (n <= 0) break;
            data += n; size -= std::size_t(n); written_ += std::size_t(n);
        }
        sceIoClose(fd); // closed per report: the last one survives a crash
    }

    SDL_Thread *thread_ = nullptr;
    SDL_mutex *mutex_ = nullptr;
    SDL_cond *ready_ = nullptr, *idle_ = nullptr;
    std::array<char, kCapacity> buffer_{};
    std::size_t size_ = 0, written_ = 0;
    uint64_t dropped_ = 0;                  // owner thread only
    std::atomic<unsigned> written_reports_{0};
    bool busy_ = false, stopping_ = false;
};


} // namespace vita
