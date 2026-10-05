#define SDL_MAIN_HANDLED
#include "audio.h"
#include "native_audio.h"
#include "runtime/native_sound_engine.h"
#include "sound_worker.h"
#include "controls.h"
#include "diagnostic_log.h"
#include "async_log.h"
#include "core_profile.h"
// Renderer selection (CMake: DAYTONA_VITA_GPU_FAST -> libvita2d, DAYTONA_VITA_GPU_GL -> vitaGL).
// Both initialise sceGxm, so exactly one is linked into a given build.
#ifndef DAYTONA_VITA_GPU_GL
#define DAYTONA_VITA_GPU_GL 0
#endif
#if DAYTONA_VITA_GPU_GL
#include "gpu_gl.h"
#else
#include "gpu_fast.h"

#endif
#include "gpu_text.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#if !DAYTONA_VITA_GPU_GL
#include <vita2d.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
#include <utility>

#ifndef DAYTONA_VITA_DIAGNOSTICS
#define DAYTONA_VITA_DIAGNOSTICS 0
#endif
// Default geometrizer placement: 0 main core (reference), 1 geometry core
// exact, 2 geometry core pipelined (3D one frame behind, fastest: default).
// Saved in vita.cfg.
#ifndef DAYTONA_VITA_GEO_MODE
#define DAYTONA_VITA_GEO_MODE 1
#endif
// Application cores (0-2) of the three busy threads; -1 = leave unpinned.
#ifndef DAYTONA_VITA_MAIN_CORE
#define DAYTONA_VITA_MAIN_CORE 0
#endif
#ifndef DAYTONA_VITA_GEO_CORE
#define DAYTONA_VITA_GEO_CORE 1
#endif

extern "C" { unsigned int _newlib_heap_size_user = 192 * 1024 * 1024; }

namespace {
// ---- Graphics backend glue: the only place that knows vita2d from vitaGL ----
#if DAYTONA_VITA_GPU_GL
using Renderer = vita::GpuGlRenderer;
constexpr const char *kGfxApi = "VITAGL";
constexpr const char *kRendererName = "VITAGL";
bool gfx_init(const char *&error) {
    if (!vita::gl_init()) { error = vita::gl_error(); return false; }
    return true;
}
void gfx_begin() { vita::gl_begin_frame(); }   // clears colour + depth
void gfx_end() { vita::gl_end_frame(); }       // replays the draw list and swaps
void gfx_fini() { vita::gl_fini(); }
#else
using Renderer = vita::GpuFastRenderer;
constexpr const char *kGfxApi = "GXM";
constexpr const char *kRendererName = "VITA2D";
bool gfx_init(const char *&error) {
    if (vita2d_init_advanced(8 * 1024 * 1024) < 0) { error = "vita2d init failed"; return false; }
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(0,0,0,255));
    return true;
}
void gfx_begin() { vita2d_start_drawing(); vita2d_clear_screen(); }
void gfx_end() { vita2d_end_drawing(); vita2d_swap_buffers(); }
void gfx_fini() { vita2d_fini(); }
#endif

constexpr bool kDiagnostics = DAYTONA_VITA_DIAGNOSTICS != 0;
// Every log file (vita-diag.log periodic lines, perf.log, gl.log) and the profiling
// clocks exist only in diagnostic builds: scripts/build_vita.py --diagnostics.
// Faults are always appended to vita-diag.log.
constexpr bool kPerfLog = kDiagnostics;
constexpr bool kProfile = kDiagnostics;
constexpr int kMainCore = DAYTONA_VITA_MAIN_CORE, kGeoCore = DAYTONA_VITA_GEO_CORE;
constexpr double kPerfWindowSeconds = 5.0;
constexpr const char *kDirectory = "ux0:data/daytona93";
constexpr const char *kRom = "ux0:data/daytona93/daytona93.zip";

vita::Pad read_pad() {
    SceCtrlData native{};
    vita::Pad pad;
    if (sceCtrlPeekBufferPositive(0, &native, 1) <= 0) return pad;
    pad.lx = native.lx; pad.ry = native.ry;
    const struct { uint32_t native, portable; } map[] = {
        {SCE_CTRL_CROSS, vita::Cross}, {SCE_CTRL_CIRCLE, vita::Circle},
        {SCE_CTRL_SQUARE, vita::Square}, {SCE_CTRL_TRIANGLE, vita::Triangle},
        {SCE_CTRL_UP, vita::Up}, {SCE_CTRL_DOWN, vita::Down},
        {SCE_CTRL_LEFT, vita::Left}, {SCE_CTRL_RIGHT, vita::Right},
        {SCE_CTRL_LTRIGGER, vita::L}, {SCE_CTRL_RTRIGGER, vita::R},
        {SCE_CTRL_START, vita::Start}, {SCE_CTRL_SELECT, vita::Select}
    };
    for (auto e : map) if (native.buttons & e.native) pad.buttons |= e.portable;
    return pad;
}

bool load_bytes(const std::string &path, uint8_t *data, size_t size) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(data, 1, size, f) == size && std::fgetc(f) == EOF;
    std::fclose(f);
    return ok;
}
bool save_bytes(const std::string &path, const uint8_t *data, size_t size) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, size, f) == size;
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

template<class C> void load_nv(const char *name, C &data) {
    load_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size());
}

template<class C> bool save_nv(const char *name, const C &data) {
    return save_bytes(std::string(kDirectory) + "/" + name, data.data(), data.size());
}

rt::Inputs map_input(const vita::Input &in) {
    rt::Inputs out;
    out.steer = in.steer; out.accel = in.accel; out.brake = in.brake;
    out.in0 = in.in0; out.in1 = in.in1; out.in2 = in.in2;
    return out;
}

uint64_t ticks_us() {
    const uint64_t f = SDL_GetPerformanceFrequency();
    return f ? SDL_GetPerformanceCounter() * 1000000ull / f : 0;
}

// Measurement only; never use this clock to pace game or audio playback.
uint64_t diagnostic_ticks_us() {
    return kProfile ? ticks_us() : 0;
}

struct VitaSettings {
    int cpu_clock = 333;
    int gpu_clock = 111;
    int volume = 80;
    int deadzone = 12;
    bool mute = false;
    bool native_audio = false; // explicitly selected while native fidelity is validated
    bool steer_invert = false;
    int geo_mode = DAYTONA_VITA_GEO_MODE; // rt::GeoMode: 0 single core (exact image), 1 two cores (+1 frame)

    void defaults() { *this = VitaSettings{}; }
    void sanitize() {
        const auto valid = [](int value, const int *choices, int count, int fallback) {
            for (int i = 0; i < count; ++i) if (value == choices[i]) return value;
            return fallback;
        };
        static const int cpus[] = {111, 222, 333, 444};
        static const int gpus[] = {41, 77, 111, 166};
        cpu_clock = valid(cpu_clock, cpus, 4, 333);
        gpu_clock = valid(gpu_clock, gpus, 4, 111);
        volume = std::clamp(volume, 0, 100);
        deadzone = std::clamp(deadzone, 0, 40);
        geo_mode = geo_mode ? 1 : 0; // older builds saved 2 for the two-core mode
    }
    void load() {
        FILE *f = std::fopen("ux0:data/daytona93/vita.cfg", "r");
        if (!f) return;
        char line[96], key[40]; int value = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "%39[^=]=%d", key, &value) != 2) continue;
            if (!std::strcmp(key, "cpu_clock")) cpu_clock = value;
            else if (!std::strcmp(key, "gpu_clock")) gpu_clock = value;
            else if (!std::strcmp(key, "volume")) volume = value;
            else if (!std::strcmp(key, "mute")) mute = value != 0;
            else if (!std::strcmp(key, "native_audio")) native_audio = value != 0;
            else if (!std::strcmp(key, "deadzone")) deadzone = value;
            else if (!std::strcmp(key, "steer_invert")) steer_invert = value != 0;
            else if (!std::strcmp(key, "geo_mode")) geo_mode = value;
        }
        std::fclose(f); sanitize();
    }
    bool save() const {
        FILE *f = std::fopen("ux0:data/daytona93/vita.cfg.tmp", "w");
        if (!f) return false;
        std::fprintf(f, "cpu_clock=%d\ngpu_clock=%d\nvolume=%d\nmute=%d\nnative_audio=%d\ndeadzone=%d\nsteer_invert=%d\ngeo_mode=%d\n",
                     cpu_clock, gpu_clock, volume, int(mute), int(native_audio), deadzone, int(steer_invert), geo_mode);
        bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { std::remove("ux0:data/daytona93/vita.cfg.tmp"); return false; }
        std::remove("ux0:data/daytona93/vita.cfg");
        return std::rename("ux0:data/daytona93/vita.cfg.tmp", "ux0:data/daytona93/vita.cfg") == 0;
    }
};

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

// The game's geometrizer (platform/vita/src/runtime/geo.h), found from its
// board's TGP buffer RAM: M2Board itself is the unmodified shared runtime.
rt::Geo *geometry_of(rt::GameLoop *game) {
    return game ? rt::Geo::find(game->board().tgp().buffer_data()) : nullptr;
}

int cycle_value(int value, const int *choices, int count, int direction) {
    int index = 0;
    for (int i = 0; i < count; ++i) if (choices[i] == value) index = i;
    return choices[(index + (direction < 0 ? count - 1 : 1)) % count];
}

void draw_menu(bool have_game, bool options, int selection, const VitaSettings &settings,
               const std::string &status, double fps) {
    const unsigned white = RGBA8(235,235,235,255), yellow = RGBA8(255,200,70,255);
    vita::gpu_text(options ? "DAYTONA RECOMP - OPTIONS" : "DAYTONA RECOMP - GPU25", 30, 24, white, 3, 49, 1);
    char line[128];
    if (!options) {
        std::snprintf(line, sizeof line, "FPS %.1F  CPU %d MHz  GPU %d MHz  GXM", fps,
                      scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency());
        vita::gpu_text(line, 30, 64, white, 2, 70, 1);
        const char *labels[4] = {have_game ? "RESUME GAME" : "START GAME", "RESET GAME", "OPTIONS", "SAVE AND QUIT"};
        for (int i = 0; i < 4; ++i) {
            std::string label = std::string(i == selection ? "> " : "  ") + labels[i];
            vita::gpu_text(label, 46, 126 + i * 44, i == selection ? yellow : white, 2, 70, 1);
        }
        vita::gpu_text(status, 30, 342, white, 2, 74, 6);
        vita::gpu_text("CROSS SELECT  CIRCLE RESUME  START+SELECT MENU", 30, 516, white, 2, 74, 1);
        return;
    }
    static const char *const geo_labels[2] = {"GEOMETRY: 1 CORE (EXACT IMAGE)", "GEOMETRY: 2 CORES (3D +1 FRAME)"};
    const char *values[14];
    char cpu[32], gpu[32], volume[32], mute[32], deadzone[32], invert[32];
    std::snprintf(cpu, sizeof cpu, "CPU CLOCK: %d MHz", settings.cpu_clock);
    std::snprintf(gpu, sizeof gpu, "GPU CLOCK: %d MHz", settings.gpu_clock);
    std::snprintf(volume, sizeof volume, "VOLUME: %d%%", settings.volume);
    std::snprintf(mute, sizeof mute, "MUTE: %s", settings.mute ? "ON" : "OFF");
    std::snprintf(deadzone, sizeof deadzone, "DEAD ZONE: %d%%", settings.deadzone);
    std::snprintf(invert, sizeof invert, "INVERT STEERING: %s", settings.steer_invert ? "ON" : "OFF");
    values[0]=cpu; values[1]=gpu; values[2]=volume; values[3]=mute; values[4]=deadzone; values[5]=invert;
    values[6]=settings.native_audio ? "AUDIO ENGINE: NATIVE (TEST)" : "AUDIO ENGINE: REFERENCE";
    values[7]=geo_labels[settings.geo_mode ? 1 : 0];
    values[8]="GRAPHICS API: GXM"; values[9]="FULLSCREEN: ON"; values[10]="ROM: DAYTONA93.ZIP";
    values[11]="BINDINGS: VITA FIXED"; values[12]="RESET DEFAULTS"; values[13]="BACK";
    for (int i = 0; i < 14; ++i) {
        std::string label = std::string(i == selection ? "> " : "  ") + values[i];
        vita::gpu_text(label, 42, 68 + i * 30, i == selection ? yellow : white, 2, 70, 1);
    }
    vita::gpu_text(status, 30, 489, white, 1, 112, 2);
    vita::gpu_text("LEFT/RIGHT CHANGE  CROSS SELECT  CIRCLE BACK", 30, 526, white, 1, 112, 1);
}
} // namespace

int main(int, char **) {
    sceIoMkdir(kDirectory, 0777);
    vita::DiagnosticLog log(kDiagnostics);
    log.begin();
    log.literal("GPU25: configurable clocks and GXM System24 fast path starting\n");
    void *heap_probe = std::malloc(1024);
    if (!heap_probe) { log.fault("GPU25: heap unavailable\n"); sceKernelExitProcess(1); }
    std::free(heap_probe);

    VitaSettings settings;
    settings.load();
    const int arm_clock_before = scePowerGetArmClockFrequency();
    const int gpu_clock_before = scePowerGetGpuClockFrequency();
    const int cpu_clock_result = scePowerSetArmClockFrequency(settings.cpu_clock);
    const int gpu_clock_result = scePowerSetGpuClockFrequency(settings.gpu_clock);
    if (cpu_clock_result < 0 || gpu_clock_result < 0)
        log.fault("GPU25: clock request failed: cpu=%d gpu=%d\n", cpu_clock_result, gpu_clock_result);
    auto restore_clocks = [&] {
        if (arm_clock_before > 0) scePowerSetArmClockFrequency(arm_clock_before);
        if (gpu_clock_before > 0) scePowerSetGpuClockFrequency(gpu_clock_before);
    };
    const int arm_clock = scePowerGetArmClockFrequency();
    const int bus_clock = scePowerGetBusClockFrequency();
    const int gpu_clock = scePowerGetGpuClockFrequency();
    const int xbar_clock = scePowerGetGpuXbarClockFrequency();
    log.log("GPU25 clocks: requested_cpu=%d requested_gpu=%d arm=%d bus=%d gpu=%d xbar=%d cpu_before=%d gpu_before=%d set_cpu=%d set_gpu=%d\n",
            settings.cpu_clock, settings.gpu_clock, arm_clock, bus_clock, gpu_clock, xbar_clock,
            arm_clock_before, gpu_clock_before, cpu_clock_result, gpu_clock_result);
    // Core placement: the main thread (i960, TGP, 2D video, GPU submission)
    // owns one application core; geometry and sound get the other two.
    const SceUID main_thread = sceKernelGetThreadId();
    const int main_affinity_before = sceKernelGetThreadCpuAffinityMask(main_thread);
    const int main_affinity_result = kMainCore >= 0 && kMainCore <= 2
        ? sceKernelChangeThreadCpuAffinityMask(main_thread, SCE_KERNEL_CPU_MASK_USER_0 << kMainCore) : 0;
    const int main_affinity = sceKernelGetThreadCpuAffinityMask(main_thread);
    log.log("GPU25 cores: main requested=%d before=0x%x result=%d mask=0x%x priority=%d\n", kMainCore,
            unsigned(main_affinity_before), main_affinity_result, unsigned(main_affinity),
            sceKernelGetThreadCurrentPriority());
    log.literal("GPU25 stage: SDL timer/audio init begin\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        log.fault("GPU25: SDL timer/audio init failed: %s\n", SDL_GetError());
        restore_clocks();
        return 1;
    }
    log.literal("GPU25 stage: SDL init done; init begin\n");
    #if DAYTONA_VITA_GPU_GL
        const char *gfx_error = "";
        if (!gfx_init(gfx_error)) {
            log.fault("GPU25: %s initialization failed: %s\n", kGfxApi, gfx_error);
            SDL_Quit();
            restore_clocks();
            return 1;
        }
        log.literal("GPU25 stage: init done; framebuffer textures begin\n");
        
        Renderer gpu;
        if (!gpu.ok()) {
            log.fault("GPU25: framebuffer texture allocation failed\n");
            gpu.shutdown(); gfx_fini(); SDL_Quit(); restore_clocks(); return 1;
        }    
    #else
        if (vita2d_init_advanced(8 * 1024 * 1024) < 0) {
            log.fault("GPU25: vita2d/GXM initialization failed\n");
            SDL_Quit();
            restore_clocks();
            return 1;
        }
        log.literal("GPU25 stage: vita2d init done; framebuffer textures begin\n");
        vita2d_set_vblank_wait(1);
        vita2d_set_clear_color(RGBA8(0,0,0,255));
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

        vita::GpuFastRenderer gpu;
        if (!gpu.ok()) {
            log.fault("GPU25: framebuffer texture allocation failed\n");
            gpu.shutdown(); vita2d_fini(); SDL_Quit(); restore_clocks(); return 1;
        }
    #endif
    log.log("GPU25 stage: GPU texture arenas ready: reserved_mb=%.2f; audio begin\n", double(gpu.reserved_bytes()) / (1024.0 * 1024.0));
    vita::Audio audio;
    vita::NativeAudio<snd::NativeSoundEngine> native_audio;
    bool active_native_audio = false;
    if (!audio.open()) log.fault("GPU25: audio unavailable: %s\n", SDL_GetError());
    audio.volume(float(settings.volume) / 100.0f);
    audio.mute(settings.mute);
    log.literal("GPU25 stage: audio done; main loop ready\n");

    std::unique_ptr<rt::GameLoop> game;
    // Destruction order keeps the detached packet, game and audio alive until
    // the worker has drained. Only one sound packet may be in flight.
    rt::GameLoop::SoundPacket active_sound;
    vita::SoundWorker sound_worker;
    sound_worker.open();
    log.log("GPU25 sound worker: threaded=%d affinity_result=%d affinity_mask=0x%x\n",
            int(sound_worker.threaded()), sound_worker.affinity_result(), sound_worker.affinity_mask());

    // Per-core profiling (perf.log). Reports cover gameplay only.
    vita::CoreProfiler core_profile;
    vita::PerfLog perf_file;
    unsigned perf_index = 0;
    uint64_t perf_log_pending_us = 0; // report formatting time, charged to the next window
    const rt::Geo *geo_seen = nullptr;
    rt::Geo::Stats geo_prev;
    // Per-loop geometry statistics; re-baselined when the board changes.
    auto take_geo_delta = [&]() -> rt::Geo::Stats {
        const rt::Geo *geo = geometry_of(game.get());
        if (geo != geo_seen || !geo || geo->stats().parse_calls < geo_prev.parse_calls) {
            geo_seen = geo;
            geo_prev = geo ? geo->stats() : rt::Geo::Stats{};
            return {};
        }
        const rt::Geo::Stats d = geo_stats_delta(geo->stats(), geo_prev);
        geo_prev = geo->stats();
        return d;
    };
    if constexpr (kPerfLog) {
        if (perf_file.open()) {
            char text[1536];
            int n = std::snprintf(text, sizeof text,
                "start: renderer=%s profile_window=%.0fs | main core=%d mask=0x%x result=%d | geometry core=%d | "
                "sound worker threaded=%d mask=0x%x | geo_mode saved=%d build_default=%d\n",
                kRendererName, kPerfWindowSeconds, kMainCore, unsigned(main_affinity), main_affinity_result, kGeoCore,
                int(sound_worker.threaded()), unsigned(sound_worker.affinity_mask()), settings.geo_mode, DAYTONA_VITA_GEO_MODE);
            if (n < 0) n = 0;
            n = std::min(n, int(sizeof text) - 1);
            n += int(vita::CoreProfiler::format_csv_header(text + n, sizeof text - size_t(n)));
            perf_file.submit(text, size_t(n));
        } else {
            log.fault("GPU25: cannot create %s\n", vita::PerfLog::kPath);
        }
    }
    vita::AsyncLog perf_log;
    if constexpr (kDiagnostics) perf_log.open(log, ticks_us);
    log.log("GPU25 periodic log worker: threaded=%d; unavailable worker drops periodic records only\n",
            int(perf_log.threaded()));
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz, 1);
    bool running = true, menu = true, options = false, wait_release = true;
    int selection = 0;
    uint32_t previous_buttons = 0;
    std::string status = "CROSS SELECT. OPTIONS INCLUDE CLOCKS, AUDIO, DISPLAY AND CONTROLS.";
    uint64_t last = ticks_us(), perf_start = last, perf_frames = 0, perf_run_us = 0, perf_gpu_us = 0;
    uint64_t perf_core_us = 0, perf_geo_us = 0, perf_video_us = 0, perf_sound_us = 0;
    uint64_t perf_presents = 0, perf_wait_us = 0, perf_encode_us = 0;
    uint64_t perf_sound_wait_us = 0, perf_audio_queue_us = 0, perf_sound_frames = 0;
    uint64_t perf_sort_us = 0, perf_polygon_us = 0, perf_tile_us = 0, perf_upload_us = 0;
    uint64_t previous_log_us = 0, perf_frame_peak_us = 0;
    bool sound_in_flight = false;
    double display_fps = 0.0;
#if DAYTONA_VITA_GPU_GL
    vita::GlLoopProfile loop_profile; // this iteration, sent to gl.log at its end
#endif

    // Join only after the following frame's board work, or before an operation
    // that mutates audio/lifetime state. The worker never touches GameLoop's
    // profile or pending bytes while the next board frame runs.
    auto finish_sound = [&]() -> bool {
        if (!sound_in_flight) return true;
        sound_in_flight = false;
        const uint64_t begin = diagnostic_ticks_us();
        try { sound_worker.finish(); }
        catch (const std::exception &e) {
            perf_sound_wait_us += diagnostic_ticks_us() - begin;
            status = e.what();
            perf_log.sync([&] { log.fault("GPU25 sound runtime: %s\n", e.what()); });
            menu = true;
            audio.pause(); native_audio.pause();
            active_sound = {};
            game.reset(); // finish has joined, including its error path
            return false;
        }
        perf_sound_wait_us += diagnostic_ticks_us() - begin;
        perf_sound_us += sound_worker.last_sound_ticks();
        perf_audio_queue_us += sound_worker.last_audio_ticks();
        ++perf_sound_frames;
        if constexpr (kPerfLog) {
            core_profile.add(vita::CoreProfiler::SoundBoard, sound_worker.last_sound_ticks());
            core_profile.add(vita::CoreProfiler::SoundQueue, sound_worker.last_audio_ticks());
            ++core_profile.counters.sound_frames;
        }
#if DAYTONA_VITA_GPU_GL
        loop_profile.sound_wait += diagnostic_ticks_us() - begin;
        loop_profile.sound_worker += sound_worker.last_sound_ticks();
#endif
        return true;
    };

    auto apply_preferences = [&] {
        audio.volume(float(settings.volume) / 100.0f);
        audio.mute(settings.mute);
        native_audio.volume(float(settings.volume) / 100.0f);
        native_audio.mute(settings.mute);
        controls.set_deadzone(float(settings.deadzone) / 100.0f);
        controls.set_steer_invert(settings.steer_invert);
    };
    auto commit_settings = [&](bool set_clocks) {
        if (!finish_sound()) return;
        perf_log.drain();
        int cpu_result = 0, gpu_result = 0;
        if (set_clocks) {
            cpu_result = scePowerSetArmClockFrequency(settings.cpu_clock);
            gpu_result = scePowerSetGpuClockFrequency(settings.gpu_clock);
        }
        if (cpu_result < 0 || gpu_result < 0)
            log.fault("GPU25: clock request failed: cpu=%d gpu=%d\n", cpu_result, gpu_result);
        apply_preferences();
        const bool saved = settings.save();
        if (!saved) log.fault("GPU25: settings save failed\n");
        char message[160];
        std::snprintf(message, sizeof message, "CPU %d/%d MHz  GPU %d/%d MHz  SETTINGS %s",
                      scePowerGetArmClockFrequency(), settings.cpu_clock,
                      scePowerGetGpuClockFrequency(), settings.gpu_clock, saved ? "SAVED" : "SAVE FAILED");
        status = message;
        log.log("GPU25 settings: requested_cpu=%d requested_gpu=%d actual_cpu=%d actual_gpu=%d set_cpu=%d set_gpu=%d volume=%d mute=%d deadzone=%d invert=%d native_audio=%d saved=%d\n",
                settings.cpu_clock, settings.gpu_clock, scePowerGetArmClockFrequency(),
                scePowerGetGpuClockFrequency(), cpu_result, gpu_result, settings.volume,
                int(settings.mute), settings.deadzone, int(settings.steer_invert), int(settings.native_audio), int(saved));
    };
    apply_preferences();

    auto save = [&] {
        if (!finish_sound()) return false;
        if (!game) return true;
        const bool a = save_nv("ioboard_eeprom.bin", game->board().io().eeprom);
        const bool b = save_nv("backup_ram.bin", game->board().backup_ram());
        if (!a || !b) perf_log.sync([&] { log.fault("GPU25: save failed\n"); });
        return a && b;
    };
    auto apply_mode = [&] {
        if (game) game->board().video().set_external_3d(true); // the GPU draws the 3D and the System 24 layers
        gpu.reset_materials();
        clock.reset();
    };
    // Geometrizer placement for the running game (settings.geo_mode). The
    // geometry thread belongs to the board's geometrizer and ends with it.
    auto apply_geometry = [&]() {
        rt::Geo *geo = geometry_of(game.get());
        if (!geo) return;
        const bool ok = geo->configure(rt::GeoMode(settings.geo_mode), kGeoCore, kProfile ? ticks_us : nullptr);
        geo_seen = nullptr; // re-baseline the per-loop statistics
        const rt::Geo::Worker &w = geo->worker();
        char line[256];
        const int n = std::snprintf(line, sizeof line,
            "geometry: mode=%s requested=%d thread=%d core=%d result=%d mask=0x%x priority=%d fpscr main=0x%08x worker=0x%08x\n",
            rt::geo_mode_name(geo->mode()), settings.geo_mode, int(w.threaded), w.core, w.create_result,
            unsigned(w.affinity_mask), w.priority, unsigned(w.fpscr_main), unsigned(w.fpscr_worker));
        if constexpr (kPerfLog) if (n > 0) perf_file.submit(line, size_t(std::min(n, int(sizeof line) - 1)));
        perf_log.sync([&] {
            if (ok) log.log("GPU25 %s", line);
            else log.fault("GPU25 %s", line); // thread unavailable: geometry stays on the main core
        });
    };
    auto start_game = [&]() -> bool {
        if (!finish_sound()) return false;
        save();
        active_sound = {};
        native_audio.close();
        audio.close();
        game.reset();
        gpu.reset_materials();
        status = "LOADING DAYTONA93...";
        gpu.prepare_frame();
        #if DAYTONA_VITA_GPU_GL
            gfx_begin(); draw_menu(false, false, 0, settings, status, display_fps); gfx_end();
        #else
            vita2d_start_drawing(); vita2d_clear_screen(); draw_menu(false, false, 0, settings, status, display_fps); vita2d_end_drawing(); vita2d_swap_buffers();
        #endif
        try {
            auto images = rt::import_rom_set(kRom);
            active_native_audio = settings.native_audio;
            if (active_native_audio) {
                sound_worker.close();
                auto engine = std::make_unique<snd::NativeSoundEngine>(
                    std::move(images.sound_program), std::move(images.pcm1), std::move(images.pcm2));
                if (!native_audio.open(std::move(engine), kDiagnostics ? ticks_us : nullptr))
                    throw std::runtime_error("Native audio output unavailable; select Reference audio and reset");
            } else {
                if (!sound_worker.threaded()) sound_worker.open();
                if (!audio.open()) perf_log.sync([&] { log.fault("GPU25 reference audio unavailable: %s\n", SDL_GetError()); });
            }
            game = std::make_unique<rt::GameLoop>(std::move(images), !active_native_audio);
            perf_log.sync([&] {
                log.log("GPU25 audio engine: backend=%s clock=%s reference_sound_board=%d\n",
                        active_native_audio ? "NATIVE_TEST" : "REFERENCE",
                        active_native_audio ? "AUDIO_DEVICE_48000" : "GAME_FRAME", int(game->sound() != nullptr));
            });
            game->set_profile_clock(kProfile ? ticks_us : nullptr);
            load_nv("ioboard_eeprom.bin", game->board().io().eeprom);
            load_nv("backup_ram.bin", game->board().backup_ram());
            game->board().video().set_profile_clock(kProfile ? ticks_us : nullptr);
            apply_mode();
            apply_geometry();
            controls = vita::Controls{};
            apply_preferences();
            status = "START+SELECT MENU. TRIANGLE VIEW 4. D-PAD UP/DOWN SHIFT.";
            wait_release = true;
            return true;
        } catch (const std::exception &e) {
            native_audio.close(); audio.pause(); game.reset();
            status = e.what();
            perf_log.sync([&] { log.fault("GPU25 start: %s\n", e.what()); });
            return false;
        }
    };

    while (running) {
#if DAYTONA_VITA_GPU_GL
        loop_profile = {};
#endif
        const uint64_t now = ticks_us();
        const double elapsed = double(now - last) / 1000000.0;
        last = now;
        const bool menu_at_start = menu;
        if constexpr (kPerfLog) core_profile.begin_loop(now);
        const vita::Pad pad = read_pad();
        uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (wait_release) {
            pressed = 0; controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }
        if (!menu && !wait_release && vita::menu_chord(pad.buttons)) {
            menu = true; options = false; selection = 0;
            finish_sound();
            audio.pause(); native_audio.pause(); save(); clock.reset(); wait_release = true;
        }
        if (menu && !wait_release) {
            if (options) {
                constexpr int kOptionCount = 14;
                if (pressed & vita::Up) selection = (selection + kOptionCount - 1) % kOptionCount;
                if (pressed & vita::Down) selection = (selection + 1) % kOptionCount;
                if (pressed & vita::Circle) { options = false; selection = 2; wait_release = true; }
                else {
                    const int direction = (pressed & vita::Left) ? -1 : ((pressed & vita::Right) ? 1 : 0);
                    const bool activate = (pressed & vita::Cross) != 0;
                    bool changed = false, clocks_changed = false;
                    static const int cpu_choices[] = {111, 222, 333, 444};
                    static const int gpu_choices[] = {41, 77, 111, 166};
                    if (selection == 0 && (direction || activate)) {
                        settings.cpu_clock = cycle_value(settings.cpu_clock, cpu_choices, 4, direction < 0 ? -1 : 1);
                        changed = clocks_changed = true;
                    } else if (selection == 1 && (direction || activate)) {
                        settings.gpu_clock = cycle_value(settings.gpu_clock, gpu_choices, 4, direction < 0 ? -1 : 1);
                        changed = clocks_changed = true;
                    } else if (selection == 2 && (direction || activate)) {
                        settings.volume = std::clamp(settings.volume + (direction < 0 ? -10 : 10), 0, 100);
                        changed = true;
                    } else if (selection == 3 && (direction || activate)) {
                        settings.mute = !settings.mute; changed = true;
                    } else if (selection == 4 && (direction || activate)) {
                        settings.deadzone = std::clamp(settings.deadzone + (direction < 0 ? -5 : 5), 0, 40);
                        changed = true;
                    } else if (selection == 5 && (direction || activate)) {
                        settings.steer_invert = !settings.steer_invert; changed = true;
                    } else if (selection == 6 && (direction || activate)) {
                        settings.native_audio = !settings.native_audio; changed = true;
                    } else if (selection == 7 && (direction || activate)) {
                        settings.geo_mode = settings.geo_mode ? 0 : 1;
                        changed = true;
                    } else if (selection == 8 && activate) {
                        status = DAYTONA_VITA_GPU_GL ? "RENDERER FIXED AT BUILD: VITAGL (--GPU-GL)." : "VITA RENDERER IS FIXED TO NATIVE GXM GPU FAST.";
                    } else if (selection == 9 && activate) {
                        status = "VITA OUTPUT IS FIXED FULLSCREEN AT 960 X 544.";
                    } else if (selection == 10 && activate) {
                        FILE *rom = std::fopen(kRom, "rb");
                        status = rom ? "ROM CHECK OK: UX0:DATA/DAYTONA93/DAYTONA93.ZIP"
                                     : "ROM MISSING: UX0:DATA/DAYTONA93/DAYTONA93.ZIP";
                        if (rom) std::fclose(rom);
                    } else if (selection == 11 && activate) {
                        status = "STEER=L-STICK  PEDALS=R-STICK/L/R  SHIFT=UP/DOWN";
                    } else if (selection == 12 && activate) {
                        settings.defaults(); changed = clocks_changed = true;
                    } else if (selection == 13 && activate) {
                        options = false; selection = 2; wait_release = true;
                    }
                    if (changed) {
                        commit_settings(clocks_changed);
                        if (selection == 6) status = "AUDIO ENGINE CHANGE SAVED. RESET GAME TO APPLY. NATIVE IS EXPERIMENTAL.";
                        if (selection == 7 || selection == 12) {
                            // Applied at once: the board joins the running parse first.
                            try {
                                apply_geometry();
                                if (selection == 7)
                                    status = settings.geo_mode != 0 && geometry_of(game.get()) && geometry_of(game.get())->mode() == rt::GeoMode::Sync
                                               ? "GEOMETRY THREAD UNAVAILABLE: MAIN CORE USED."
                                           : settings.geo_mode ? "GEOMETRY ON ITS OWN CORE, 3D ONE FRAME BEHIND THE 2D LAYERS."
                                                               : "GEOMETRY ON THE MAIN CORE, EXACT IMAGE.";
                            } catch (const std::exception &e) {
                                status = e.what();
                                perf_log.sync([&] { log.fault("GPU25 geometry: %s\n", e.what()); });
                            }
                        }
                    }
                }
            } else {
                if (pressed & vita::Up) selection = (selection + 3) % 4;
                if (pressed & vita::Down) selection = (selection + 1) % 4;
                if ((pressed & vita::Circle) && game) {
                    menu = false; clock.reset(); wait_release = true;
                } else if (pressed & vita::Cross) {
                    if (selection == 0) {
                        if (game || start_game()) { menu = false; clock.reset(); wait_release = true; }
                    } else if (selection == 1) {
                        if (start_game()) { menu = false; clock.reset(); wait_release = true; }
                    } else if (selection == 2) {
                        options = true; selection = 0; wait_release = true;
                    } else if (selection == 3) {
                        save(); settings.save(); running = false;
                    }
                }
            }
        }

        if constexpr (kPerfLog) core_profile.add(vita::CoreProfiler::Input, diagnostic_ticks_us() - now);
        bool simulated = false;
        if (game && !menu) {
            if (active_native_audio) native_audio.resume();
            const int frames = clock.advance(elapsed);
            for (int n = 0; n < frames; ++n) {
                // Work counters and geometrizer statistics before the frame (the
                // shared runtime itself is not instrumented beyond FrameProfile).
                const uint64_t i960_before = game->instructions();
                const uint64_t tgp_before = game->board().tgp().tgp_instructions();
                const rt::Geo *frame_geo = geometry_of(game.get());
                const rt::Geo::Stats geo_before = frame_geo ? frame_geo->stats() : rt::Geo::Stats{};
                const uint64_t begin = diagnostic_ticks_us();
                try {
                    auto next_sound = game->run_frame_sound_packet(
                        map_input(controls.sample(wait_release ? vita::Pad{} : pad)));
                    const uint64_t board_end = diagnostic_ticks_us();
                    perf_run_us += board_end - begin;
                    const auto &fp = game->last_profile();
                    const rt::Geo::Stats geo_frame = frame_geo ? geo_stats_delta(frame_geo->stats(), geo_before) : rt::Geo::Stats{};
                    if constexpr (kPerfLog) {
                        // Main-core board frame: game logic -> vblank start (geometry) ->
                        // vblank handler -> vblank end (2D video). FrameProfile gives the
                        // vblank start/end durations; the geometrizer's timestamps of its
                        // parse() call (vblank start) and of the board's read of the list
                        // (vblank end) split the i960+TGP time between the two phases.
                        using P = vita::CoreProfiler;
                        const rt::Geo::Stats &gs = frame_geo ? frame_geo->stats() : geo_before;
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
                        core_profile.add(P::Logic, logic);
                        core_profile.add(P::GeoMain, fp.geometry);
                        core_profile.add(P::Irq, irq);
                        core_profile.add(P::Video2D, fp.video);
                        core_profile.add(P::BoardOther, wall > known ? wall - known : 0);
                        core_profile.add(P::GeoWaitBoard, geo_frame.wait_ticks);
                        core_profile.add(P::Snapshot, geo_frame.snapshot_ticks);
                        auto &c = core_profile.counters;
                        ++c.board_frames;
                        c.i960_instructions += game->instructions() - i960_before;
                        c.tgp_instructions += game->board().tgp().tgp_instructions() - tgp_before;
                        c.geo_count_reads += geo_frame.count_reads;
                    }
                    perf_core_us += fp.core(); perf_geo_us += fp.geometry;
                    perf_video_us += fp.video;
#if DAYTONA_VITA_GPU_GL
                    ++loop_profile.board_frames;
                    loop_profile.board += diagnostic_ticks_us() - begin;
                    loop_profile.board_core += fp.core();
                    loop_profile.board_geometry += fp.geometry;
                    loop_profile.board_video += fp.video;
#endif
                    ++perf_frames;
                    const uint64_t sound_begin = diagnostic_ticks_us();
                    if (active_native_audio) {
                        // Only commands cross into audio. The callback sequences
                        // and mixes continuously, not once per graphics frame.
                        const auto bytes = game->board().take_sound_bytes();
                        const auto health = native_audio.stats();
                        if (health.failed || health.invalid || health.unsupported) {
                            char error[192];
                            std::snprintf(error, sizeof error,
                                "Native audio fault: callback=%u invalid=%u unsupported=%u. Reset with Reference audio.",
                                health.failed, health.invalid, health.unsupported);
                            throw std::runtime_error(error);
                        }
                        if (!native_audio.send(bytes.data(), bytes.size()))
                            throw std::runtime_error("Native audio queue overflow; use Reference audio and reset");
                    } else {
                        // Reference backend retains the bounded sound pipeline.
                        if (!finish_sound()) break;
                        active_sound = std::move(next_sound);
                        sound_worker.dispatch_packet(active_sound, audio, kProfile ? ticks_us : nullptr);
                        sound_in_flight = true;
                    }
                    if constexpr (kPerfLog) core_profile.add(vita::CoreProfiler::SoundSync, diagnostic_ticks_us() - sound_begin);
                    simulated = true;
                } catch (const std::exception &e) {
                    // A board/dispatch failure must not destroy a sound board
                    // still used by the previous job.
                    finish_sound();
                    status = e.what();
                    perf_log.sync([&] { log.fault("GPU25 runtime: %s\n", e.what()); });
                    menu = true; audio.pause(); native_audio.close(); active_sound = {}; game.reset(); break;
                }
            }
        } else clock.reset();

        const uint64_t gpu_begin = diagnostic_ticks_us();
        gpu.prepare_frame();
        const uint64_t gpu_ready = diagnostic_ticks_us();
        #if DAYTONA_VITA_GPU_GL
            gfx_begin();
        #else
            vita2d_start_drawing();
            vita2d_clear_screen();
        #endif
        const uint64_t gfx_begun = diagnostic_ticks_us();
        if (menu) draw_menu(bool(game), options, selection, settings, status, display_fps);
        else if (game) {
            gpu.draw(game->board().video()); // System 24 layers + polygons on the GPU (the only case)
        }
        const uint64_t drawn = diagnostic_ticks_us();
        #if DAYTONA_VITA_GPU_GL
            gfx_end();
        #else
            vita2d_end_drawing();
            vita2d_swap_buffers();
        #endif
        const uint64_t gfx_ended = diagnostic_ticks_us();
        // Geometry core work joined during this loop (folded in at each join).
        const rt::Geo::Stats geo_delta = take_geo_delta();
        (void)geo_delta; // unused without perf.log
        if constexpr (kPerfLog) {
            using P = vita::CoreProfiler;
            core_profile.add(P::GpuPrepare, gpu_ready - gpu_begin);
            core_profile.add(P::GfxBegin, gfx_begun - gpu_ready);
            const uint64_t draw_total = drawn - gfx_begun;
            if (!menu && game) {
                const uint64_t upload = gpu.last_upload_us(), layers = gpu.last_tile_us(), sort = gpu.last_sort_us();
                const uint64_t parts = upload + layers + sort;
                core_profile.add(P::Upload, upload);
                core_profile.add(P::Layers, layers);
                core_profile.add(P::Sort, sort);
                core_profile.add(P::Polygons, draw_total > parts ? draw_total - parts : 0);
#if DAYTONA_VITA_GPU_GL
                core_profile.add(P::TexBuild, gpu.last_texture_us());
#endif
            } else {
                core_profile.add(P::Upload, draw_total); // menu text or the exact CPU frame upload
            }
            core_profile.add(P::GfxEnd, gfx_ended - drawn);
        }
        if (!menu && game) {
            ++perf_presents;
            perf_wait_us += gpu_ready - gpu_begin;
            perf_encode_us += uint64_t(gpu.last_gpu_ms() * 1000.0);
            perf_sort_us += gpu.last_sort_us(); perf_polygon_us += gpu.last_polygon_us();
            perf_tile_us += gpu.last_tile_us(); perf_upload_us += gpu.last_upload_us();
            perf_gpu_us += diagnostic_ticks_us() - gpu_begin;
        }

#if DAYTONA_VITA_GPU_GL
        loop_profile.menu = menu;
        loop_profile.prepare = gpu_ready - gpu_begin;
        if constexpr (kDiagnostics) vita::gl_profile_loop(loop_profile);
#endif
        const uint64_t after = ticks_us();
        if (simulated) perf_frame_peak_us = std::max(perf_frame_peak_us, after - now);
        if (after - perf_start >= 2000000) {
            const double sec = double(after - perf_start) / 1000000.0;
            display_fps = sec > 0 ? double(perf_frames) / sec : 0.0;
            const double run_ms = perf_frames ? double(perf_run_us) / perf_frames / 1000.0 : 0.0;
            const double present_divisor = perf_presents ? double(perf_presents) : 1.0;
            const double gpu_ms = double(perf_gpu_us) / present_divisor / 1000.0;
            const double frame_divisor = perf_frames ? double(perf_frames) : 1.0;
            const double sound_divisor = perf_sound_frames ? double(perf_sound_frames) : 1.0;
            if (kDiagnostics && game) {
                const auto &vp = game->board().video().last_profile();
                const auto log_stats = perf_log.stats();
                const auto native_stats = native_audio.stats();
                const uint64_t log_begin = ticks_us();
                perf_log.try_log("gpu25: mode=%s menu=%d frames=%u presents=%u window_ms=%.2f log_enqueue_prev_ms=%.2f log_write_prev_ms=%.2f log_write_peak_ms=%.2f log_drops=%u log_failures=%u sound_frames=%u frame_peak_ms=%.2f sim_fps=%.2f run_ms=%.2f core_ms=%.2f geo_ms=%.2f video_ms=%.2f sound_ms=%.2f gpu_submit_ms=%.2f gpu_encode_ms=%.2f gpu_wait_ms=%.2f sound_wait_ms=%.2f audio_worker_queue_ms=%.2f sort_ms=%.2f poly_ms=%.2f tiles_ms=%.2f upload_ms=%.2f cpu_mhz=%d gpu_mhz=%d cpu_raster_ms=%.2f tile_cache_ms=%.2f tile_draw_ms=%.2f compose_ms=%.2f layers=%u tiles=%u chars=%u materials=%u sources=%u builds=%u defers=%u cache_mb=%.2f cache_reserved_mb=%.2f pool_free_kb=%u pool_drops=%u material_drops=%u subdiv_polys=%u vertices=%u tile_uploads=%u cache_resets=%u tile_quads=%u draws=%u/%u clips=%u polys=%u/%u shader_setups=%u state_reuses=%u draw_errors=%u checker_polys=%u textured_checker_polys=%u sys24_ctrl=%04x/%04x audio=%s native_frames=%u native_last_ms=%.3f native_peak_ms=%.3f native_queue=%u native_overflows=%u native_failed=%u native_unsupported=%u native_invalid=%u native_notes=%u native_voices=%u\n",
                        "GPU", int(menu), unsigned(perf_frames), unsigned(perf_presents),
                        sec * 1000.0, double(previous_log_us) / 1000.0,
                        double(log_stats.last_write_ticks) / 1000.0, double(log_stats.max_write_ticks) / 1000.0,
                        unsigned(log_stats.dropped), unsigned(log_stats.failures), unsigned(perf_sound_frames),
                        double(perf_frame_peak_us) / 1000.0, display_fps, run_ms,
                        double(perf_core_us)/frame_divisor/1000.0, double(perf_geo_us)/frame_divisor/1000.0,
                        double(perf_video_us)/frame_divisor/1000.0, double(perf_sound_us)/sound_divisor/1000.0, gpu_ms, double(perf_encode_us)/present_divisor/1000.0, double(perf_wait_us)/present_divisor/1000.0, double(perf_sound_wait_us)/frame_divisor/1000.0, double(perf_audio_queue_us)/sound_divisor/1000.0,
                        double(perf_sort_us)/present_divisor/1000.0, double(perf_polygon_us)/present_divisor/1000.0,
                        double(perf_tile_us)/present_divisor/1000.0, double(perf_upload_us)/present_divisor/1000.0,
                        scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(),
                        double(vp.raster)/1000.0, double(vp.tile_cache)/1000.0, double(vp.tile_draw)/1000.0,
                        double(vp.composite)/1000.0, unsigned(vp.layers_rebuilt), vp.tiles_rebuilt, vp.characters_changed,
                        unsigned(gpu.cached_materials()), unsigned(gpu.cached_sources()), gpu.material_builds(), gpu.material_defers(), double(gpu.cached_bytes())/(1024.0*1024.0),
                        double(gpu.reserved_bytes())/(1024.0*1024.0), gpu.min_pool_free()/1024u, gpu.pool_drops(), gpu.material_drops(), gpu.subdivided_polys(), unsigned(gpu.submitted_vertices()), gpu.system24_uploaded_tiles(), gpu.cache_resets(), gpu.system24_quads(), gpu.textured_draws(), gpu.solid_draws(), gpu.clip_changes(), gpu.textured_polys(), gpu.solid_polys(), gpu.shader_setups(), gpu.state_reuses(), gpu.draw_errors(), gpu.checker_polys(), gpu.textured_checker_polys(),
                        unsigned(game->board().video().system24_word(0x5004)),
                        unsigned(game->board().video().system24_word(0x5006)),
                        active_native_audio ? "NATIVE_TEST" : "REFERENCE", native_stats.frames,
                        double(native_stats.last_us)/1000.0, double(native_stats.peak_us)/1000.0,
                        native_stats.queued, native_stats.overflows, native_stats.failed,
                        native_stats.unsupported, native_stats.invalid, native_stats.notes, native_stats.voices);
                previous_log_us = ticks_us() - log_begin;
                if constexpr (kPerfLog) core_profile.add(vita::CoreProfiler::Log, previous_log_us);
            }
            perf_start = after; perf_frames = perf_run_us = perf_gpu_us = 0;
            perf_core_us = perf_geo_us = perf_video_us = perf_sound_us = 0;
            perf_presents = perf_wait_us = perf_encode_us = 0;
            perf_sound_wait_us = perf_audio_queue_us = perf_sound_frames = 0;
            perf_frame_peak_us = 0;
            perf_sort_us = perf_polygon_us = perf_tile_us = perf_upload_us = 0;
        }
        if (menu || !simulated) SDL_Delay(1);

        if constexpr (kPerfLog) {
            using P = vita::CoreProfiler;
            // Windows cover gameplay only: a loop that showed or left the menu
            // (ROM import, pause) restarts the window.
            if (!game || menu || menu_at_start) {
                core_profile.reset(ticks_us());
            } else {
                core_profile.add(P::GeoParse, geo_delta.worker_ticks);
                core_profile.add(P::Log, perf_log_pending_us);
                perf_log_pending_us = 0;
                auto &c = core_profile.counters;
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
                core_profile.end_loop(loop_end);
                if (core_profile.due(loop_end, 1000000u, kPerfWindowSeconds)) {
                    const auto native = native_audio.stats();
                    c.native_callback_last = native.last_us;
                    c.native_callback_peak = native.peak_us;
                    vita::CoreProfiler::Header h;
                    h.index = ++perf_index;
                    const rt::Geo *geo = geometry_of(game.get());
                    h.geo_mode = rt::geo_mode_name(geo ? geo->mode() : rt::GeoMode::Sync);
                    h.renderer = kRendererName;
                    h.cpu_mhz = scePowerGetArmClockFrequency();
                    h.gpu_mhz = scePowerGetGpuClockFrequency();
                    h.bus_mhz = scePowerGetBusClockFrequency();
                    h.main_core = kMainCore; h.geo_core = kGeoCore;
                    h.geo_threaded = geo && geo->worker().threaded && geo->mode() != rt::GeoMode::Sync;
                    h.sound_threaded = sound_worker.threaded();
                    h.native_audio = active_native_audio;
                    static char report[vita::PerfLog::kCapacity];
                    const size_t size = core_profile.format(report, sizeof report, h, 1000000u);
                    perf_file.submit(report, size);
                    const uint64_t reported = ticks_us();
                    perf_log_pending_us = reported - loop_end;
                    core_profile.reset(reported);
                }
            }
        }
    }

    finish_sound();
    sound_worker.close();
    save();
    settings.save();
    native_audio.close();
    audio.close();
    gpu.shutdown();
    restore_clocks();
    #if DAYTONA_VITA_GPU_GL
        gfx_fini();
    #else
        vita2d_fini();
    #endif
    perf_log.close(); // join file I/O before destroying SDL synchronization
    perf_file.close();
    SDL_Quit();
    log.literal("GPU25: clean exit\n");
    return 0;
}