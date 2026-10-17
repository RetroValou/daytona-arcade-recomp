#define SDL_MAIN_HANDLED
// Link play compiled in (build_vita.py: the games it was tested on, scripts/games.py
// vita_link). Off: no SceNet code, no link rows, link_* settings kept in vita.cfg.
#ifndef DAYTONA_VITA_LINK
#define DAYTONA_VITA_LINK 1
#endif
#include "audio/audio.h"
#include "audio/native_audio.h"
#include "runtime/native_sound_engine.h"
#include "audio/sound_worker.h"
#if DAYTONA_VITA_LINK
#include "net/link.h"
#endif
#include "frontend/controls.h"
#include "system/diagnostic_log.h"
#include "system/async_log.h"
#include "system/core_policy.h"
#include "system/core_profile.h"
// The renderer: vitaGL (render/renderer.cpp), the only one since 2026-10-16 (the CPU
// frontend main.cpp and the vita2d/GXM gpu_fast.cpp were removed).
#include "render/renderer.h"
#include "render/text.h"
#include "render/imgui_backend.h"
#include "runtime/game_loop.h"
#include "runtime/i960_profile.h"
#include "runtime/tgp_profile.h"
#include "runtime/game.h"
#include "runtime/test_hold.h"
#include "runtime/rom_import.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <cctype>
#include <string>
#include <stdexcept>
#include <vector>
#include <utility>

#ifndef DAYTONA_VITA_DIAGNOSTICS
#define DAYTONA_VITA_DIAGNOSTICS 0
#endif
// The multicore flow:
//   main thread      i960 + TGP, 2D video, sound hand-off, GPU recording   core 0
//   geometry thread  the geometrizer, always pipelined: the 3D of frame N-1 is drawn
//                    with frame N (platform/vita/overrides/runtime/geo.h)        core 1
//   sound worker     the reference (emulated) sound board                  core 2
//   vitaGL 2D worker System 24 uploads + layer rectangles (render/layers_2d.cpp)    core 2
// vitaGL draws the 2D layers of frame N-1 too (two texture sets, render/layers_2d.cpp): the picture
// is the exact one, one frame late, in phase with the 3D. -1 = unpinned: scripts/build_vita.py --free-core leaves every thread
// free, and the optional fourth core usable by all of them (core_policy.h). If the
// geometry thread cannot be created, Geo stays on the main core and the fault is logged.
#ifndef DAYTONA_VITA_MAIN_CORE
#define DAYTONA_VITA_MAIN_CORE 0
#endif
#ifndef DAYTONA_VITA_GEO_CORE
#define DAYTONA_VITA_GEO_CORE 1
#endif
#ifndef DAYTONA_VITA_SOUND_CORE
#define DAYTONA_VITA_SOUND_CORE 2
#endif

// Read by newlib (not LTO code): kept by "used" in the release (LTO) build.
extern "C" { __attribute__((used)) unsigned int _newlib_heap_size_user = 192 * 1024 * 1024; }

namespace {
// ---- Graphics backend glue (vitaGL) ----
using Renderer = vita::GpuGlRenderer;
constexpr const char *kGfxApi = "VITAGL";
constexpr const char *kRendererName = "VITAGL";
// Widescreen (aspect, HUD at the edges, stretched backdrop) was drawn by the removed GXM
// renderer only: the vitaGL renderer keeps the original 4:3 picture.
constexpr bool kWidescreen = false;
bool gfx_init(const char *&error) {
    if (!vita::gl_init()) { error = vita::gl_error(); return false; }
    return true;
}
void gfx_begin() { vita::gl_begin_frame(); }   // clears colour + depth
void gfx_end() { vita::gl_end_frame(); }       // replays the draw list and swaps
void gfx_idle() { glFinish(); }
void gfx_fini() { vita::gl_fini(); }

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
constexpr const char *kDirectory = "ux0:data/" M2_ROMSET;
// A package holds one game: this set, and the game's companion set when
// bundled (Game::companion: Daytona's two sets), which the ROM option
// switches to. Other games are packages of their own (their own title IDs:
// scripts/games.py).
const rt::Game *companion() { return rt::game().companion ? rt::find_game(rt::game().companion) : nullptr; }
// The set the ROM option selects.
const rt::Game &selected_game(bool use_companion) { return use_companion && companion() ? *companion() : rt::game(); }
std::string rom_label(const rt::Game &g) {
    std::string s = std::string("ROM: ") + g.set + " (" + g.label + ")";
    for (char &c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool app_file(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}
constexpr const char *kRom = "ux0:data/" M2_ROMSET "/" M2_ROMSET ".zip";
constexpr const char *kConfig = "ux0:data/" M2_ROMSET "/vita.cfg";
constexpr const char *kConfigTemp = "ux0:data/" M2_ROMSET "/vita.cfg.tmp";

vita::Pad read_pad() {
    SceCtrlData native{};
    vita::Pad pad;
    if (sceCtrlPeekBufferPositive(0, &native, 1) <= 0) return pad;
    pad.lx = native.lx; pad.ry = native.ry; pad.ly = native.ly;
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
    out.handbrake = in.handbrake;
    return out;
}

// The game's controls (rt::Game::control_list): what the Controls page lists and binds.
vita::Layout game_layout() { return {rt::game().control_list.data(), int(rt::game().control_list.size())}; }

// Microseconds. SDL2's Vita performance counter is this same call at 1 MHz; calling it
// directly skips SDL and a 64-bit division per read (measured 1.39 us per read through SDL,
// 0.70 us direct): the profiling clocks of a diagnostics build cost half as much.
uint64_t ticks_us() { return sceKernelGetProcessTimeWide(); }

// Measurement only; never use this clock to pace game or audio playback.
uint64_t diagnostic_ticks_us() {
    return kProfile ? ticks_us() : 0;
}

// Clocks of a first launch (no saved settings) and of "defaults": CPU 444 / GPU 166 MHz.
// The options menu
// changes them, and the saved choice wins on the next launches. Override at build time
// with -DDAYTONA_VITA_DEFAULT_CPU_MHZ=... / -DDAYTONA_VITA_DEFAULT_GPU_MHZ=...
#ifndef DAYTONA_VITA_DEFAULT_CPU_MHZ
#define DAYTONA_VITA_DEFAULT_CPU_MHZ 444
#endif
#ifndef DAYTONA_VITA_DEFAULT_GPU_MHZ
#define DAYTONA_VITA_DEFAULT_GPU_MHZ 166
#endif
constexpr int kDefaultCpuClock = DAYTONA_VITA_DEFAULT_CPU_MHZ;
constexpr int kDefaultGpuClock = DAYTONA_VITA_DEFAULT_GPU_MHZ;
// vita.cfg keys of the controls (rt::Control order); value = button bit. The first
// twelve are the keys every earlier version saved.
constexpr const char *kBindKeys[vita::ControlCount] = {"bind_accelerate", "bind_brake",
    "bind_steer_left", "bind_steer_right", "bind_shift_up", "bind_shift_down", "bind_start",
    "bind_coin", "bind_view1", "bind_view2", "bind_view3", "bind_view4", "bind_handbrake",
    "bind_up", "bind_down", "bind_left", "bind_right", "bind_punch", "bind_kick", "bind_guard"};

struct VitaSettings {
    bool use_companion = false; // the ROM option: the game's companion set (not saved)
    bool link_enabled = false, link_sync = false;
    int link_port = 15112, next_port = 15112;
    int next_ip[4] = {192, 168, 1, 2};
    std::string next_address() const {
        return std::to_string(next_ip[0]) + "." + std::to_string(next_ip[1]) + "." +
            std::to_string(next_ip[2]) + "." + std::to_string(next_ip[3]);
    }
    int cpu_clock = kDefaultCpuClock;
    int gpu_clock = kDefaultGpuClock;
    int volume = 80;
    // Balance between the music and the effects (engine, skids, crashes), as the
    // desktop launcher's; 100/100 is the arcade's mix. The defaults are the game's
    // (rt::Game::defaults: Daytona 100/20, Sega Rally 100/85).
    int music_volume = rt::game().defaults.music_volume, effects_volume = rt::game().defaults.effects_volume;
    int deadzone = 12;
    int aspect = 0, draw_distance = 0, steer_curve = 0;
    bool hud_edges = false;

    bool fourth_core = false;
    bool stretch_backdrop = false, skip_launcher = false;
    // The game's battery-backed RAM (backup_ram.bin: records, bookkeeping, and the
    // coins not used yet, which a cabinet keeps through a power cut: with them a
    // game can start a race on its own at the next launch). Off: neither loaded
    // nor saved, every start is a cabinet switched on afresh. The settings EEPROM
    // (ioboard_eeprom.bin: test mode settings) is always kept.
    bool save_backup = false;
    bool mute = false;
    bool native_audio = false; // explicitly selected while native fidelity is validated
    bool steer_invert = false;
    vita::Bindings bindings = vita::Bindings::defaults(game_layout()); // Controls page of the launcher

    void defaults() { *this = VitaSettings{}; }
    void sanitize() {
        link_port = std::clamp(link_port, 1, 65535);
        next_port = std::clamp(next_port, 1, 65535);
        for (int &octet : next_ip) octet = std::clamp(octet, 0, 255);
        const auto valid = [](int value, const int *choices, int count, int fallback) {
            for (int i = 0; i < count; ++i) if (value == choices[i]) return value;
            return fallback;
        };
        static const int cpus[] = {111, 222, 333, 444, 500};
        static const int gpus[] = {41, 77, 111, 166};
        cpu_clock = valid(cpu_clock, cpus, 5, kDefaultCpuClock);
        gpu_clock = valid(gpu_clock, gpus, 4, kDefaultGpuClock);
        static_assert(kDefaultCpuClock == 111 || kDefaultCpuClock == 222 || kDefaultCpuClock == 333 ||
                      kDefaultCpuClock == 444 || kDefaultCpuClock == 500, "CPU default must be a menu choice");
        static_assert(kDefaultGpuClock == 41 || kDefaultGpuClock == 77 || kDefaultGpuClock == 111 ||
                      kDefaultGpuClock == 166, "GPU default must be a menu choice");
        volume = std::clamp(volume, 0, 100);
        music_volume = std::clamp(music_volume, 0, 100);
        effects_volume = std::clamp(effects_volume, 0, 100);
        deadzone = std::clamp(deadzone, 0, 40);
        aspect = std::clamp(aspect, 0, 3);
        steer_curve = std::clamp(steer_curve, 0, 2);
        draw_distance = std::clamp(draw_distance, -2, 2);
        bindings.sanitize(game_layout());
    }
    void load() {
        FILE *f = std::fopen(kConfig, "r");
        if (!f) return;
        char line[96], key[40]; int value = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "%39[^=]=%d", key, &value) != 2) continue;
            if (!std::strcmp(key, "fourth_core")) fourth_core = value != 0;
            else if (!std::strcmp(key, "link_enabled")) link_enabled = value != 0;
            else if (!std::strcmp(key, "link_sync")) link_sync = value != 0;
            else if (!std::strcmp(key, "link_port")) link_port = value;
            else if (!std::strcmp(key, "next_port")) next_port = value;
            else if (!std::strcmp(key, "next_ip0")) next_ip[0] = value;
            else if (!std::strcmp(key, "next_ip1")) next_ip[1] = value;
            else if (!std::strcmp(key, "next_ip2")) next_ip[2] = value;
            else if (!std::strcmp(key, "next_ip3")) next_ip[3] = value;
            else if (!std::strcmp(key, "stretch_backdrop")) stretch_backdrop = value != 0;
            else if (!std::strcmp(key, "skip_launcher")) skip_launcher = value != 0;
            else if (!std::strcmp(key, "save_backup")) save_backup = value != 0;
            else if (!std::strcmp(key, "steer_curve")) steer_curve = value;
            else if (!std::strcmp(key, "aspect")) aspect = value;
            else if (!std::strcmp(key, "draw_distance")) draw_distance = value;
            else if (!std::strcmp(key, "hud_edges")) hud_edges = value != 0;
            else if (!std::strcmp(key, "cpu_clock")) cpu_clock = value;
            else if (!std::strcmp(key, "gpu_clock")) gpu_clock = value;
            else if (!std::strcmp(key, "volume")) volume = value;
            else if (!std::strcmp(key, "music_volume")) music_volume = value;
            else if (!std::strcmp(key, "effects_volume")) effects_volume = value;
            else if (!std::strcmp(key, "mute")) mute = value != 0;
            else if (!std::strcmp(key, "native_audio")) native_audio = value != 0;
            else if (!std::strcmp(key, "deadzone")) deadzone = value;
            else if (!std::strcmp(key, "steer_invert")) steer_invert = value != 0;
            else for (int a = 0; a < vita::ControlCount; ++a)
                if (!std::strcmp(key, kBindKeys[a])) bindings.button[a] = uint32_t(value);
        }
        std::fclose(f); sanitize();
    }
    bool save() const {
        FILE *f = std::fopen(kConfigTemp, "w");
        if (!f) return false;
        std::fprintf(f, "cpu_clock=%d\ngpu_clock=%d\nvolume=%d\nmute=%d\nnative_audio=%d\ndeadzone=%d\nsteer_invert=%d\n",
                     cpu_clock, gpu_clock, volume, int(mute), int(native_audio), deadzone, int(steer_invert));
        std::fprintf(f, "music_volume=%d\neffects_volume=%d\n", music_volume, effects_volume);
        std::fprintf(f, "stretch_backdrop=%d\nskip_launcher=%d\nsave_backup=%d\n", int(stretch_backdrop), int(skip_launcher),
                     int(save_backup));
        std::fprintf(f, "fourth_core=%d\n", int(fourth_core));
        std::fprintf(f, "link_enabled=%d\nlink_sync=%d\nlink_port=%d\nnext_port=%d\n",
            int(link_enabled), int(link_sync), link_port, next_port);
        for (int i = 0; i < 4; ++i) std::fprintf(f, "next_ip%d=%d\n", i, next_ip[i]);
        std::fprintf(f, "steer_curve=%d\n", steer_curve);
        std::fprintf(f, "aspect=%d\ndraw_distance=%d\nhud_edges=%d\n", aspect, draw_distance, int(hud_edges));
        for (int a = 0; a < vita::ControlCount; ++a)
            if (bindings.button[a]) std::fprintf(f, "%s=%u\n", kBindKeys[a], unsigned(bindings.button[a]));
        bool ok = std::fflush(f) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (!ok) { std::remove(kConfigTemp); return false; }
        std::remove(kConfig);
        return std::rename(kConfigTemp, kConfig) == 0;
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

// The game's geometrizer (platform/vita/overrides/runtime/geo.h), found from its
// board's TGP buffer RAM: M2Board itself is the unmodified shared runtime.
rt::Geo *geometry_of(rt::GameLoop *game) {
    return game ? rt::Geo::find(game->board().tgp().buffer_data()) : nullptr;
}

int cycle_value(int value, const int *choices, int count, int direction) {
    int index = 0;
    for (int i = 0; i < count; ++i) if (choices[i] == value) index = i;
    return choices[(index + (direction < 0 ? count - 1 : 1)) % count];
}

uint32_t ui_buttons = 0;
rt::TestHold test_hold;
// Rows of the Controls page: the game's controls in its own order (rt::Game::
// control_list), then reset (-1) and back (-2).
constexpr int kMaxBindRows = vita::ControlCount + 2;
int bind_rows(int (&rows)[kMaxBindRows]) {
    const vita::Layout layout = game_layout();
    int n = 0;
    for (int i = 0; i < layout.count && n < vita::ControlCount; ++i) rows[n++] = int(layout.defs[i].control);
    rows[n++] = -1;
    rows[n++] = -2;
    return n;
}
bool has_steering() {
    const vita::Layout layout = game_layout();
    return layout.has(rt::Control::SteerLeft) || layout.has(rt::Control::SteerRight);
}

// The Options page's rows, in display order.
enum Option {
    OptCpu, OptGpu, OptVolume, OptMute, OptMusic, OptEffects, OptDeadzone, OptInvert, OptAudioEngine, OptRom,
    OptAspect, OptHud, OptDistance, OptSteerCurve, OptStretch, OptSkipLauncher, OptSaveBackup, OptFourthCore,
    OptLink, OptIp0, OptIp1, OptIp2, OptIp3, OptListenPort, OptNextPort, OptLinkSync,
    OptTestHold, OptDefaults, OptBack, kOptions
};
// The package holds the companion set's executable (the ROM option can switch).
bool rom_switch_packaged() {
    static const bool packaged = companion() &&
        (app_file(std::string("app0:/") + companion()->set + ".self") ||
         app_file(std::string("app0:/") + rt::game().set + ".self"));
    return packaged;
}
// Only the options that can change something in this build, for this game, are
// listed: none fixed at build time (renderer, fullscreen), none the game or the
// renderer has no use for (widescreen on vitaGL, the native audio engine without
// the game's driver tables, the music and effects volumes when neither the native
// engine nor the sound board can tell them apart, the draw distance without its hook,
// link play without the communication board, the ROM switch without a second
// executable).
// Link play: compiled in, and the set has the communication board.
bool link_play() { return DAYTONA_VITA_LINK && rt::game().link; }

bool option_available(Option o, const VitaSettings &s) {
    const bool native = rt::game().native_sound != nullptr;
    switch (o) {
    // the native engine's own mix, or the sound board's (its driver's voices known)
    case OptMusic: case OptEffects: return (native && s.native_audio) || rt::has_music_effects(rt::game());
    case OptAudioEngine: return native;
    case OptRom: return rom_switch_packaged();
    case OptAspect: case OptHud: case OptStretch: return kWidescreen;
    case OptDistance: return rt::game().draw_distance;
    case OptLink: return link_play();
    case OptIp0: case OptIp1: case OptIp2: case OptIp3: case OptListenPort: case OptNextPort: case OptLinkSync:
        return link_play() && s.link_enabled;
    case OptInvert: case OptSteerCurve: return has_steering(); // a wheel to invert or shape
    default: return true;
    }
}
// The listed rows; returns their count.
int listed_options(const VitaSettings &s, Option (&rows)[kOptions]) {
    int n = 0;
    for (int o = 0; o < kOptions; ++o)
        if (option_available(Option(o), s)) rows[n++] = Option(o);
    return n;
}
void draw_menu(bool have_game, bool &options, bool &binds, int &selection, int capture,
               const VitaSettings &settings, const std::string &status, double fps) {
    ImGui::SetNextWindowPos({12, 12});
    ImGui::SetNextWindowSize({936, 520});
    ImGui::Begin(rt::game().title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("%s | %.1f FPS | CPU %d MHz | GPU %d MHz | %s",
                rt::game().title, fps, scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(), kGfxApi);
    if (ImGui::Button("Game")) { options = binds = false; selection = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Options")) { options = true; binds = false; selection = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Controls")) { binds = true; options = false; selection = 0; }
    ImGui::SameLine(); ImGui::TextDisabled("D-pad navigate | X select | O back");
    ImGui::Separator();
    static int previous_selection = -1;
    static bool previous_options = false, previous_binds = false;
    const bool scroll_to_selection = selection != previous_selection || options != previous_options ||
                                     binds != previous_binds;
    if (ImGui::BeginChild("settings", {0, -100}, ImGuiChildFlags_None)) {
        if (binds) {
            char rows[kMaxBindRows][64];
            int actions[kMaxBindRows];
            const int bound = bind_rows(actions);
            for (int i = 0; i < bound; ++i) {
                const int a = actions[i];
                const char *label = a == -1 ? "RESET CONTROLS TO DEFAULTS" : "BACK";
                if (a >= 0) {
                    std::snprintf(rows[i], sizeof rows[i], "%s: %s", game_layout().name(a),
                                  capture == a ? "PRESS A BUTTON..." : vita::button_name(settings.bindings.button[a]));
                    label = rows[i];
                }
                ImGui::PushID(i);
                if (ImGui::Selectable(label, selection == i, 0, {760, 32})) { selection = i; ui_buttons = vita::Cross; }
                if (selection == i && scroll_to_selection) ImGui::SetScrollHereY();
                if (a >= 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("<")) { selection = i; ui_buttons = vita::Left; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(">")) { selection = i; ui_buttons = vita::Right; }
                }
                ImGui::PopID();
            }
        } else if (!options) {
            const char *labels[] = {have_game ? "Resume / launch selected ROM" : "Start game", "Reset game", "Options",
                                    "Controls", "Quit"};
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(labels[i], selection == i, 0, {0, 36})) { selection = i; ui_buttons = vita::Cross; }
                if (selection == i && scroll_to_selection) ImGui::SetScrollHereY();
            }
        } else {
    const char *values[kOptions];
    char link_fields[6][64];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(link_fields[i], sizeof link_fields[i], "NEXT CABINET IP OCTET %d: %d", i + 1, settings.next_ip[i]);
        values[OptIp0 + i] = link_fields[i];
    }
    std::snprintf(link_fields[4], sizeof link_fields[4], "LISTEN PORT: %d", settings.link_port);
    std::snprintf(link_fields[5], sizeof link_fields[5], "NEXT CABINET PORT: %d", settings.next_port);
    values[OptListenPort] = link_fields[4]; values[OptNextPort] = link_fields[5];
    values[OptLink] = settings.link_enabled ? "LINK PLAY: ON" : "LINK PLAY: OFF";
    values[OptLinkSync] = settings.link_sync ? "LINK FRAME SYNC: ON" : "LINK FRAME SYNC: OFF";
    char cpu[64], gpu[32], volume[32], mute[32], music[32], effects[32], deadzone[32], invert[32];
    std::snprintf(cpu, sizeof cpu, "CPU CLOCK: %d MHz (ACTUAL %d)", settings.cpu_clock,
                  scePowerGetArmClockFrequency());
    std::snprintf(gpu, sizeof gpu, "GPU CLOCK: %d MHz", settings.gpu_clock);
    std::snprintf(volume, sizeof volume, "VOLUME: %d%%", settings.volume);
    std::snprintf(mute, sizeof mute, "MUTE: %s", settings.mute ? "ON" : "OFF");
    std::snprintf(music, sizeof music, "MUSIC: %d%%", settings.music_volume);
    std::snprintf(effects, sizeof effects, "EFFECTS: %d%%", settings.effects_volume);
    std::snprintf(deadzone, sizeof deadzone, "DEAD ZONE: %d%%", settings.deadzone);
    std::snprintf(invert, sizeof invert, "INVERT STEERING: %s", settings.steer_invert ? "ON" : "OFF");
    values[OptCpu]=cpu; values[OptGpu]=gpu; values[OptVolume]=volume; values[OptMute]=mute;
    values[OptMusic]=music; values[OptEffects]=effects; values[OptDeadzone]=deadzone; values[OptInvert]=invert;
    values[OptAudioEngine]=settings.native_audio ? "AUDIO ENGINE: NATIVE (TEST)" : "AUDIO ENGINE: REFERENCE";
    static std::string rom;
    rom = rom_label(selected_game(settings.use_companion));
    values[OptRom]=rom.c_str();
    static const char* aspects[] = {"ASPECT: ORIGINAL", "ASPECT: 16:10", "ASPECT: 16:9", "ASPECT: 21:9"};
    static const char* distances[] = {"DISTANCE: SHORTEST", "DISTANCE: SHORTER", "DISTANCE: DEFAULT", "DISTANCE: FURTHER", "DISTANCE: FURTHEST"};
    values[OptAspect]=aspects[settings.aspect]; values[OptHud]=settings.hud_edges ? "HUD: SCREEN EDGES" : "HUD: CENTRED";
    values[OptDistance]=distances[settings.draw_distance + 2];
    values[OptTestHold]=test_hold.armed() ? "HOLD TEST BUTTON: ARMED (START / RESUME)" : "HOLD TEST BUTTON: OFF";
    values[OptDefaults]="RESET DEFAULTS"; values[OptBack]="BACK";
    values[OptFourthCore]=!settings.fourth_core ? "4TH CORE: OFF" :
        vita::fourth_core_active() ? "4TH CORE: ENABLED" : "4TH CORE: UNAVAILABLE (PLUGIN REQUIRED)";
    values[OptStretch]=settings.stretch_backdrop ? "STRETCH TILE BACKGROUND: ON" : "STRETCH TILE BACKGROUND: OFF";
    values[OptSkipLauncher]=settings.skip_launcher ? "SKIP LAUNCHER: ON" : "SKIP LAUNCHER: OFF";
    values[OptSaveBackup]=settings.save_backup ? "SAVE RECORDS (BACKUP RAM): ON" : "SAVE RECORDS (BACKUP RAM): OFF";
    static const char* curves[] = {"STEERING CURVE: LINEAR", "STEERING CURVE: SOFT", "STEERING CURVE: EXTRA SOFT"};
    values[OptSteerCurve]=curves[settings.steer_curve];
    Option rows[kOptions];
    const int listed = listed_options(settings, rows);

            for (int i = 0; i < listed; ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(values[rows[i]], selection == i, 0, {760, 32})) { selection = i; ui_buttons = vita::Cross; }
                if (selection == i && scroll_to_selection) ImGui::SetScrollHereY();
                ImGui::SameLine();
                if (ImGui::SmallButton("<")) { selection = i; ui_buttons = vita::Left; }
                ImGui::SameLine();
                if (ImGui::SmallButton(">")) { selection = i; ui_buttons = vita::Right; }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    previous_selection = selection; previous_options = options; previous_binds = binds;
    ImGui::Separator();
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::TextDisabled("START+SELECT menu | SELECT+TRIANGLE test | SELECT+SQUARE service");
    ImGui::End();
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
    const int cpu_clock_result = vita::set_cpu_clock(settings.cpu_clock);
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
    const int main_home_mask = vita::core_pin_mask(kMainCore);
    const int main_affinity_before = sceKernelGetThreadCpuAffinityMask(main_thread);
    const int main_affinity_result = main_home_mask
        ? sceKernelChangeThreadCpuAffinityMask(main_thread, main_home_mask) : 0;
    const int main_affinity = sceKernelGetThreadCpuAffinityMask(main_thread);
    log.log("GPU25 cores: %s main requested=%d before=0x%x result=%d mask=0x%x priority=%d\n",
            kPinnedCores ? "pinned" : "free", kMainCore, unsigned(main_affinity_before), main_affinity_result,
            unsigned(main_affinity), sceKernelGetThreadCurrentPriority());
    log.literal("GPU25 stage: SDL timer/audio init begin\n");
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        log.fault("GPU25: SDL timer/audio init failed: %s\n", SDL_GetError());
        restore_clocks();
        return 1;
    }
    log.literal("GPU25 stage: SDL init done; graphics init begin\n");
    const char *gfx_error = "";
    if (!gfx_init(gfx_error)) {
        log.fault("GPU25: %s initialization failed: %s\n", kGfxApi, gfx_error);
        SDL_Quit();
        restore_clocks();
        return 1;
    }
    log.literal("GPU25 stage: graphics init done; framebuffer textures begin\n");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita::ImGuiVita ui;
    if (!ui.init()) {
        log.fault("ImGui font texture allocation failed\n");
        gfx_fini(); SDL_Quit(); restore_clocks(); return 1;
    }

    Renderer gpu;
    if (!gpu.ok()) {
        log.fault("GPU25: framebuffer texture allocation failed\n");
        gpu.shutdown(); ui.shutdown(); gfx_fini(); SDL_Quit(); restore_clocks(); return 1;
    }
    if (gpu.worker_2d_threaded())
        log.log("GPU25 2D worker: thread on core %d\n", gpu.worker_2d_core());
    else
        log.fault("GPU25 2D worker: thread unavailable (result=%d), 2D prepared on the main core\n",
                  gpu.worker_2d_result());
    log.log("GPU25 stage: GPU texture arenas ready: reserved_mb=%.2f; audio begin\n", double(gpu.reserved_bytes()) / (1024.0 * 1024.0));
    vita::Audio audio;
    vita::NativeAudio<snd::NativeSoundEngine> native_audio;
    bool active_native_audio = false;
    if (!audio.open()) log.fault("GPU25: audio unavailable: %s\n", SDL_GetError());
    audio.volume(float(settings.volume) / 100.0f);
    audio.mute(settings.mute);
    log.literal("GPU25 stage: audio done; main loop ready\n");

#if DAYTONA_VITA_LINK
    std::unique_ptr<vita::TcpLink> link; // outlives the board's transport pointer
#else
    std::unique_ptr<rt::LinkTransport> link; // no link play in this build: always empty
#endif
    std::unique_ptr<rt::GameLoop> game;
    // Destruction order keeps the detached packet, game and audio alive until
    // the worker has drained. Only one sound packet may be in flight.
    rt::GameLoop::SoundPacket active_sound;
    vita::SoundWorker sound_worker;
    // Reference sound board on its own core (0: unpinned, the fourth-core policy).
    sound_worker.set_cpu_mask(vita::core_pin_mask(kSoundCore));
    sound_worker.open();
    log.log("GPU25 sound worker: threaded=%d affinity_result=%d affinity_mask=0x%x\n",
            int(sound_worker.threaded()), sound_worker.affinity_result(), sound_worker.affinity_mask());

    // Per-core profiling (perf.log, diagnostics builds). Reports cover gameplay only.
    vita::CoreProfiler core_profile;
    double sound_clock_us = 0.0; // one ticks_us read (sound probe, M2_SOUND_PROFILE)
#ifdef M2_TGP_PROFILE
    // TGP profiling (build_vita.py --tgp-profile): a TGP section in each perf.log report.
    rt::TgpBench tgp_bench;
    rt::TgpProfileReport tgp_report;
#endif
#ifdef M2_I960_PROFILE
    // i960 profiling (build_vita.py --i960-profile): an i960 section in each perf.log report.
    rt::I960ProfileReport i960_report;
#endif
    vita::PerfLog perf_file;
    unsigned perf_index = 0;
    unsigned audio_gaps_seen = 0; // reference audio underruns already reported
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
                "start: renderer=%s cores=%s profile_window=%.0fs | main core=%d mask=0x%x result=%d | geometry core=%d | "
                "sound core=%d threaded=%d mask=0x%x result=%d\n",
                kRendererName, kPinnedCores ? "pinned" : "free", kPerfWindowSeconds, kMainCore, unsigned(main_affinity),
                main_affinity_result, kGeoCore, kSoundCore, int(sound_worker.threaded()),
                unsigned(sound_worker.affinity_mask()), sound_worker.affinity_result());
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
    bool running = true, menu = true, options = false, binds = false, wait_release = true;
    int selection = 0, capture = -1; // capture: action waiting for its new button (Controls page)
    uint32_t previous_buttons = 0;
    std::string status = "CROSS SELECT. OPTIONS INCLUDE CLOCKS, AUDIO, DISPLAY AND CONTROLS.";
    uint64_t last = ticks_us(), perf_start = last, perf_frames = 0, perf_run_us = 0, perf_gpu_us = 0;
    uint64_t perf_core_us = 0, perf_geo_us = 0, perf_video_us = 0, perf_sound_us = 0;
    uint64_t perf_presents = 0, perf_wait_us = 0, perf_encode_us = 0;
    uint64_t perf_sound_wait_us = 0, perf_audio_queue_us = 0, perf_sound_frames = 0;
    uint64_t perf_sort_us = 0, perf_polygon_us = 0, perf_tile_us = 0, perf_upload_us = 0;
    uint64_t previous_log_us = 0, perf_frame_peak_us = 0;
    bool sound_in_flight = false;
    bool native_effect_warned = false;
    double display_fps = 0.0;
    vita::GlLoopProfile loop_profile; // this iteration, sent to gl.log at its end

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
#ifdef M2_SOUND_PROFILE
        // The sound frame is joined: its counters are read and cleared before the next one.
        if constexpr (kPerfLog) core_profile.counters.add_sound(snd::sound_profile());
        snd::sound_profile().clear();
#endif
        perf_sound_us += sound_worker.last_sound_ticks();
        perf_audio_queue_us += sound_worker.last_audio_ticks();
        ++perf_sound_frames;
        if constexpr (kPerfLog) {
            core_profile.add(vita::CoreProfiler::SoundBoard, sound_worker.last_sound_ticks());
            core_profile.add(vita::CoreProfiler::SoundQueue, sound_worker.last_audio_ticks());
            ++core_profile.counters.sound_frames;
        }
        loop_profile.sound_wait += diagnostic_ticks_us() - begin;
        loop_profile.sound_worker += sound_worker.last_sound_ticks();
        return true;
    };

    int applied_core_setting = -1;
    auto apply_preferences = [&] {
        if (applied_core_setting != int(settings.fourth_core)) {
            // Probed on the main thread, which then goes back to its own core when pinned.
            if (!vita::configure_fourth_core(settings.fourth_core, main_home_mask))
                log.fault("CPU affinity request rejected; using ordinary application cores\n");
            applied_core_setting = int(settings.fourth_core);
        }
        audio.volume(float(settings.volume) / 100.0f);
        audio.gain(rt::game().output_gain);
        audio.mute(settings.mute);
        native_audio.volume(float(settings.volume) / 100.0f);
        native_audio.volumes(float(settings.music_volume) / 100.0f, float(settings.effects_volume) / 100.0f);
        native_audio.mute(settings.mute);
        controls.set_deadzone(float(settings.deadzone) / 100.0f);
        controls.set_steer_invert(settings.steer_invert);
        controls.set_steer_curve(settings.steer_curve);
        controls.set_layout(game_layout());
        controls.set_bindings(settings.bindings);
        rt::GameLoop::set_draw_distance(settings.draw_distance);
        if (game) {
            static constexpr double aspects[] = {0, 16.0/10, 16.0/9, 21.0/9};
            game->set_aspect(kWidescreen ? aspects[settings.aspect] : 0);
            game->set_hud_edges(kWidescreen && settings.hud_edges);
            game->set_stretch_backdrop(kWidescreen && settings.stretch_backdrop);
        }
    };
    auto commit_settings = [&](bool set_clocks) {
        if (!finish_sound()) return;
        perf_log.drain();
        int cpu_result = 0, gpu_result = 0;
        if (set_clocks) {
            cpu_result = vita::set_cpu_clock(settings.cpu_clock);
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
        const bool b = !settings.save_backup || save_nv("backup_ram.bin", game->board().backup_ram());
        if (!a || !b) perf_log.sync([&] { log.fault("GPU25: save failed\n"); });
        return a && b;
    };
    auto apply_mode = [&] {
        if (game) {
            // The GPU draws the 3D and the System 24 layers (the only Vita mode: the
            // pipelined geometrizer leaves no exact CPU picture to switch to).
            game->board().video().set_external_3d(true);
            // Hardware reports show the wide GPU tile path slower overall.
            // Keep original-aspect GPU tiles; use CPU wide layers until total
            // device frame time, not CPU composition alone, justifies it.
            game->board().video().set_gpu_background(false);
        }
        gpu.reset_materials();
        clock.reset();
    };
    // Geometrizer on its own core, pipelined (the only Vita GPU mode). The
    // geometry thread belongs to the board's geometrizer and ends with it.
    auto apply_geometry = [&]() {
        rt::Geo *geo = geometry_of(game.get());
        if (!geo) return;
        const bool ok = geo->configure(rt::GeoMode::Pipelined, kGeoCore, kProfile ? ticks_us : nullptr);
        geo_seen = nullptr; // re-baseline the per-loop statistics
        const rt::Geo::Worker &w = geo->worker();
        char line[256];
        const int n = std::snprintf(line, sizeof line,
            "geometry: mode=%s thread=%d core=%d result=%d mask=0x%x priority=%d fpscr main=0x%08x worker=0x%08x\n",
            rt::geo_mode_name(geo->mode()), int(w.threaded), w.core, w.create_result,
            unsigned(w.affinity_mask), w.priority, unsigned(w.fpscr_main), unsigned(w.fpscr_worker));
        if constexpr (kPerfLog) if (n > 0) perf_file.submit(line, size_t(std::min(n, int(sizeof line) - 1)));
        perf_log.sync([&] {
            if (ok) log.log("GPU25 %s", line);
            else log.fault("GPU25 %s", line); // thread unavailable: geometry stays on the main core
        });
    };
    auto start_game = [&]() -> bool {
        if (!finish_sound()) return false;
        if (!save()) { status = "SAVE FAILED; ROM SWITCH CANCELLED."; return false; }
        if (settings.use_companion && companion()) {
            // A two-set package: eboot.bin is one set, <companion>.self the
            // other. Running as eboot.bin, the companion is its .self; running
            // as the .self, the companion is eboot.bin. A one-set package has
            // neither and must not relaunch itself.
            const std::string self = std::string("app0:/") + companion()->set + ".self";
            const std::string own = std::string("app0:/") + rt::game().set + ".self";
            std::string executable = app_file(self) ? self : app_file(own) ? "app0:/eboot.bin" : "";
            if (executable.empty()) { status = "SELECTED ROM EXECUTABLE NOT PACKAGED. INSTALL THE TWO-ROM VPK."; return false; }
            settings.use_companion = false; // the other executable starts on its own set
            settings.save();
            native_audio.close(); audio.close(); sound_worker.close();
            game.reset(); link.reset();
            gfx_idle();
            sceGxmDisplayQueueFinish();
            const int rc = sceAppMgrLoadExec(executable.c_str(), nullptr, nullptr);
            status = "ROM SWITCH FAILED: " + std::to_string(rc);
            return false;
        }
        active_sound = {};
        native_audio.close();
        audio.close();
        game.reset();
        link.reset();
        gpu.reset_materials();
        status = std::string("LOADING ") + M2_ROMSET + "...";
        gpu.prepare_frame();
        gfx_begin();
        ui.frame(1.f / 60);
        ImGui::SetNextWindowPos({24, 24}); ImGui::SetNextWindowSize({912, 496});
        ImGui::Begin("Starting", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End(); ui.render();
        gfx_end();
        try {
            if (DAYTONA_VITA_LINK && settings.link_enabled && !rt::game().link)
                throw std::runtime_error(std::string("LINK PLAY: ") + rt::game().set +
                                         " HAS NO COMMUNICATION BOARD. SELECT ANOTHER ROM OR TURN LINK OFF.");
            auto images = rt::import_rom_set(kRom);
            active_native_audio = settings.native_audio && rt::game().native_sound; // the game's driver tables known
            native_effect_warned = false;
            if (active_native_audio) {
                sound_worker.close();
                auto engine = std::make_unique<snd::NativeSoundEngine>(
                    std::move(images.sound_program), std::move(images.pcm1), std::move(images.pcm2), *rt::game().native_sound);
                if (!native_audio.open(std::move(engine), kDiagnostics ? ticks_us : nullptr))
                    throw std::runtime_error("Native audio output unavailable; select Reference audio and reset");
            } else {
                if (!sound_worker.threaded()) sound_worker.open();
                if (!audio.open()) perf_log.sync([&] { log.fault("GPU25 reference audio unavailable: %s\n", SDL_GetError()); });
            }
            game = std::make_unique<rt::GameLoop>(std::move(images), !active_native_audio);
            if (test_hold.armed()) test_hold.arm();
            perf_log.sync([&] {
                log.log("GPU25 audio engine: backend=%s clock=%s reference_sound_board=%d\n",
                        active_native_audio ? "NATIVE_TEST" : "REFERENCE",
                        active_native_audio ? "AUDIO_DEVICE_48000" : "GAME_FRAME", int(game->sound() != nullptr));
            });
            game->set_profile_clock(kProfile ? ticks_us : nullptr);
#ifdef M2_TGP_PROFILE
            rt::tgp_profile.clock = ticks_us;
            rt::tgp_profile.ticks_per_second = 1000000u;
            tgp_bench = rt::tgp_microbench(game->board().tgp()); // about 0.1 s, once per game start
            tgp_report.start(game->board().tgp().tgp_instructions());
#endif
#ifdef M2_I960_PROFILE
            rt::i960_profile.clock = ticks_us;
            rt::i960_profile.ticks_per_second = 1000000u;
            i960_report.start(game->instructions(), uint64_t(game->interrupts()));
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
                sound_clock_us = double(ticks_us() - t0) / kReads;
            }
#endif
            load_nv("ioboard_eeprom.bin", game->board().io().eeprom);
            if (settings.save_backup) load_nv("backup_ram.bin", game->board().backup_ram());
#if DAYTONA_VITA_LINK
            if (settings.link_enabled) {
                link = std::make_unique<vita::TcpLink>(settings.link_port, settings.next_address(), settings.next_port);
                if (!link->error().empty()) throw std::runtime_error("LINK: " + link->error());
                game->board().set_link(link.get(), settings.link_sync);
            }
#endif
            game->board().video().set_profile_clock(kProfile ? ticks_us : nullptr);
            apply_mode();
            apply_geometry();
            controls = vita::Controls{};
            apply_preferences();
            // The menu chord, then the game's controls that are not its usual ones
            // (the shift, hand brake, view and fighting buttons), with their buttons.
            status = "START+SELECT MENU.";
            {
                const vita::Layout layout = game_layout();
                for (int i = 0; i < layout.count; ++i) {
                    const rt::Control c = layout.defs[i].control;
                    if (c == rt::Control::Accelerate || c == rt::Control::Brake || c == rt::Control::Start ||
                        c == rt::Control::Coin || c == rt::Control::SteerLeft || c == rt::Control::SteerRight ||
                        c == rt::Control::Up || c == rt::Control::Down || c == rt::Control::Left || c == rt::Control::Right)
                        continue;
                    status += std::string(" ") + layout.defs[i].name + " " + vita::button_name(settings.bindings.button[int(c)]) + ".";
                }
            }
            wait_release = true;
            return true;
        } catch (const std::exception &e) {
            native_audio.close(); audio.pause(); game.reset(); link.reset();
            status = e.what();
            perf_log.sync([&] { log.fault("GPU25 start: %s\n", e.what()); });
            return false;
        }
    };

    while (running) {
        loop_profile = {};
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
            menu = true; options = binds = false; selection = 0; capture = -1;
            finish_sound();
            audio.pause(); native_audio.pause(); save(); clock.reset(); wait_release = true;
        }
        if (menu && !wait_release) {
            const uint32_t pad_pressed = pressed;
            pressed |= ui_buttons;
            ui_buttons = 0;
            if (binds && capture >= 0) {
                // Any pad button (touch ignored) becomes the action's button; pressing the
                // current one keeps it. An action that had it takes the old one (swap).
                if (pad_pressed) {
                    const uint32_t button = pad_pressed & (~pad_pressed + 1);
                    settings.bindings.bind(capture, button);
                    commit_settings(false);
                    status = std::string(game_layout().name(capture)) + ": " + vita::button_name(button) +
                        ". START+SELECT MENU AND SELECT+TRIANGLE/SQUARE TEST/SERVICE STAY FIXED.";
                    capture = -1; wait_release = true;
                }
            } else if (binds) {
                int actions[kMaxBindRows];
                const int bound = bind_rows(actions);
                selection = std::clamp(selection, 0, bound - 1);
                if (pressed & vita::Up) selection = (selection + bound - 1) % bound;
                if (pressed & vita::Down) selection = (selection + 1) % bound;
                const int action = actions[selection];
                const int direction = (pressed & vita::Left) ? -1 : ((pressed & vita::Right) ? 1 : 0);
                if (pressed & vita::Circle) { binds = false; selection = 3; wait_release = true; }
                else if (action >= 0 && (pressed & vita::Cross)) {
                    capture = action; wait_release = true;
                    status = std::string("PRESS THE NEW BUTTON FOR ") + game_layout().name(action) +
                        " (ITS CURRENT BUTTON KEEPS IT).";
                } else if (action >= 0 && direction) {
                    settings.bindings.cycle(action, direction);
                    commit_settings(false);
                } else if (action == -1 && (pressed & vita::Cross)) {
                    settings.bindings = vita::Bindings::defaults(game_layout());
                    commit_settings(false);
                    status = "CONTROLS RESET TO DEFAULTS. " + status;
                } else if (action == -2 && (pressed & vita::Cross)) {
                    binds = false; selection = 3; wait_release = true;
                }
            } else if (options) {
                Option rows[kOptions];
                const int listed = listed_options(settings, rows);
                selection = std::clamp(selection, 0, listed - 1);
                if (pressed & vita::Up) selection = (selection + listed - 1) % listed;
                if (pressed & vita::Down) selection = (selection + 1) % listed;
                const Option option = rows[selection];
                if (pressed & vita::Circle) { options = false; selection = 2; wait_release = true; }
                else {
                    const int direction = (pressed & vita::Left) ? -1 : ((pressed & vita::Right) ? 1 : 0);
                    const bool activate = (pressed & vita::Cross) != 0;
                    const bool step = direction || activate;
                    bool changed = false, clocks_changed = false;
                    static const int cpu_choices[] = {111, 222, 333, 444, 500};
                    static const int gpu_choices[] = {41, 77, 111, 166};
                    switch (option) {
                    case OptCpu:
                        if (step) {
                            settings.cpu_clock = cycle_value(settings.cpu_clock, cpu_choices, 5, direction < 0 ? -1 : 1);
                            changed = clocks_changed = true;
                        }
                        break;
                    case OptGpu:
                        if (step) {
                            settings.gpu_clock = cycle_value(settings.gpu_clock, gpu_choices, 4, direction < 0 ? -1 : 1);
                            changed = clocks_changed = true;
                        }
                        break;
                    case OptVolume:
                        if (step) { settings.volume = std::clamp(settings.volume + (direction < 0 ? -10 : 10), 0, 100); changed = true; }
                        break;
                    case OptMute:
                        if (step) { settings.mute = !settings.mute; changed = true; }
                        break;
                    case OptMusic: case OptEffects:
                        if (step) {
                            int &level = option == OptMusic ? settings.music_volume : settings.effects_volume;
                            level = std::clamp(level + (direction < 0 ? -5 : 5), 0, 100);
                            changed = true;
                        }
                        break;
                    case OptDeadzone:
                        if (step) { settings.deadzone = std::clamp(settings.deadzone + (direction < 0 ? -5 : 5), 0, 40); changed = true; }
                        break;
                    case OptInvert:
                        if (step) { settings.steer_invert = !settings.steer_invert; changed = true; }
                        break;
                    case OptAudioEngine:
                        if (step) { settings.native_audio = !settings.native_audio; changed = true; }
                        break;
                    case OptRom:
                        if (step) {
                            settings.use_companion = !settings.use_companion;
                            status = "ROM SELECTED. CHOOSE START/RESET TO SWITCH. SAVES ARE SEPARATE.";
                        }
                        break;
                    case OptAspect:
                        if (step) { settings.aspect = (settings.aspect + (direction < 0 ? 3 : 1)) % 4; changed = true; }
                        break;
                    case OptHud:
                        if (step) { settings.hud_edges = !settings.hud_edges; changed = true; }
                        break;
                    case OptDistance:
                        if (step) {
                            settings.draw_distance = std::clamp(settings.draw_distance + (direction < 0 ? -1 : 1), -2, 2);
                            changed = true;
                        }
                        break;
                    case OptSteerCurve:
                        if (step) { settings.steer_curve = (settings.steer_curve + (direction < 0 ? 2 : 1)) % 3; changed = true; }
                        break;
                    case OptStretch:
                        if (step) { settings.stretch_backdrop = !settings.stretch_backdrop; changed = true; }
                        break;
                    case OptSkipLauncher:
                        if (step) { settings.skip_launcher = !settings.skip_launcher; changed = true; }
                        break;
                    case OptSaveBackup:
                        if (step) { settings.save_backup = !settings.save_backup; changed = true; }
                        break;
                    case OptFourthCore:
                        if (step) { settings.fourth_core = !settings.fourth_core; changed = true; }
                        break;
                    case OptLink:
                        if (step) { settings.link_enabled = !settings.link_enabled; changed = true; }
                        break;
                    case OptIp0: case OptIp1: case OptIp2: case OptIp3:
                        if (step) {
                            int &octet = settings.next_ip[option - OptIp0];
                            octet = (octet + (direction < 0 ? 255 : 1)) % 256; changed = true;
                        }
                        break;
                    case OptListenPort: case OptNextPort:
                        if (step) {
                            int &port = option == OptListenPort ? settings.link_port : settings.next_port;
                            port = std::clamp(port + (direction < 0 ? -1 : 1), 1, 65535); changed = true;
                        }
                        break;
                    case OptLinkSync:
                        if (step) { settings.link_sync = !settings.link_sync; changed = true; }
                        break;
                    case OptTestHold:
                        if (step) { if (test_hold.armed()) test_hold.cancel(); else test_hold.arm(); }
                        break;
                    case OptDefaults:
                        if (activate) { test_hold.cancel(); settings.defaults(); changed = clocks_changed = true; }
                        break;
                    case OptBack:
                        if (activate) { options = false; selection = 2; wait_release = true; }
                        break;
                    case kOptions:
                        break;
                    }
                    if (changed) {
                        commit_settings(clocks_changed);
                        if (option >= OptLink && option <= OptLinkSync)
                            status = "LINK SETTINGS SAVED. RESET GAME TO APPLY. NEXT: " +
                                settings.next_address() + ":" + std::to_string(settings.next_port);
                        if (option == OptFourthCore) status = !settings.fourth_core
                            ? "4TH CORE OFF: ORDINARY THREE-CORE SCHEDULING."
                            : !vita::fourth_core_active()
                                ? "4TH CORE REJECTED. REQUIRES A WORKING CORE-UNLOCK PLUGIN."
                                : kPinnedCores
                                    ? "4TH CORE ALLOWED FOR THE AUDIO CALLBACKS. GAME THREADS STAY PINNED (BUILD WITH --FREE-CORE)."
                                    : "4TH CORE ALLOWED FOR GAME, GEOMETRY AND AUDIO THREADS (FREE CORES).";
                        if (option == OptCpu && settings.cpu_clock == 500 && scePowerGetArmClockFrequency() != 500)
                            status += " 500 NOT ACTIVE: CHECK OVERCLOCK PLUGIN/PROFILE.";
                        if (option == OptDistance) status = "FURTHER DISTANCES ADD SCENERY AND MAY REDUCE FPS.";
                        if (option == OptSaveBackup) status = settings.save_backup
                            ? "RECORDS KEPT FROM THE NEXT START. UNUSED COINS ARE KEPT TOO, AS ON A CABINET."
                            : "RECORDS NOT SAVED: EACH START IS A FRESH CABINET. TEST MODE SETTINGS STAY SAVED.";
                        if (option == OptAudioEngine) status = "AUDIO ENGINE CHANGE SAVED. RESET GAME TO APPLY. NATIVE IS EXPERIMENTAL.";
                        // A change can list or unlist rows (link play's, the native engine's volumes):
                        // the cursor stays on the option just changed.
                        if (options) {
                            const int now_listed = listed_options(settings, rows);
                            for (int i = 0; i < now_listed; ++i)
                                if (rows[i] == option) selection = i;
                        }
                    }
                }
            } else {
                if (pressed & vita::Up) selection = (selection + 4) % 5;
                if (pressed & vita::Down) selection = (selection + 1) % 5;
                if ((pressed & vita::Circle) && game) {
                    menu = false; clock.reset(); wait_release = true;
                } else if (pressed & vita::Cross) {
                    if (selection == 0) {
                        const bool same_rom = !settings.use_companion;
                        if ((game && same_rom) || start_game()) { menu = false; clock.reset(); wait_release = true; }
                    } else if (selection == 1) {
                        if (start_game()) { menu = false; clock.reset(); wait_release = true; }
                    } else if (selection == 2) {
                        options = true; selection = 0; wait_release = true;
                    } else if (selection == 3) {
                        binds = true; selection = 0; wait_release = true;
                        status = "CROSS: PRESS THE NEW BUTTON. LEFT/RIGHT: CYCLE. STICKS STAY FIXED.";
                    } else if (selection == 4) {
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
#ifdef M2_SPIN_SKIP
                const uint64_t skipped_before = game->board().spin_skipped();
#endif
                const uint64_t tgp_before = game->board().tgp().tgp_instructions();
                const rt::Geo *frame_geo = geometry_of(game.get());
                const rt::Geo::Stats geo_before = frame_geo ? frame_geo->stats() : rt::Geo::Stats{};
                const uint64_t begin = diagnostic_ticks_us();
                try {
                    auto inputs = map_input(controls.sample(wait_release ? vita::Pad{} : pad));
                    test_hold.apply(game->frames(), inputs.in0);
                    auto next_sound = game->run_frame_sound_packet(inputs);
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
                        {
                            const auto &vp = game->board().video().last_profile();
                            core_profile.add(P::TileCache2D, vp.tile_cache);
                            core_profile.add(P::Palette2D, vp.palette);
                        }
                        core_profile.add(P::BoardOther, wall > known ? wall - known : 0);
                        core_profile.add(P::GeoWaitBoard, geo_frame.wait_ticks);
                        core_profile.add(P::Snapshot, geo_frame.snapshot_ticks);
                        auto &c = core_profile.counters;
                        ++c.board_frames;
                        c.i960_instructions += game->instructions() - i960_before;
#ifdef M2_SPIN_SKIP
                        c.i960_skipped += game->board().spin_skipped() - skipped_before;
#endif
                        c.tgp_instructions += game->board().tgp().tgp_instructions() - tgp_before;
                        auto &note = core_profile.note();
                        ++note.board_frames;
                        note.i960_instructions += uint32_t(game->instructions() - i960_before);
                        note.tgp_instructions += uint32_t(game->board().tgp().tgp_instructions() - tgp_before);
                        c.geo_count_reads += geo_frame.count_reads;
                        {
                            const auto &vp = game->board().video().last_profile();
                            c.tiles_rebuilt += vp.tiles_rebuilt;
                            c.characters_changed += vp.characters_changed;
                            c.tile_frames += (vp.tiles_rebuilt || vp.characters_changed) ? 1 : 0;
                        }
                    }
                    perf_core_us += fp.core(); perf_geo_us += fp.geometry;
                    perf_video_us += fp.video;
                    ++loop_profile.board_frames;
                    loop_profile.board += diagnostic_ticks_us() - begin;
                    loop_profile.board_core += fp.core();
                    loop_profile.board_geometry += fp.geometry;
                    loop_profile.board_video += fp.video;
                    ++perf_frames;
                    const uint64_t sound_begin = diagnostic_ticks_us();
                    if (active_native_audio) {
                        // Only commands cross into audio. The callback sequences
                        // and mixes continuously, not once per graphics frame.
                        const auto bytes = game->board().take_sound_bytes();
                        const auto health = native_audio.stats();
                        if (health.unsupported && !native_effect_warned) {
                            native_effect_warned = true;
                            log.log("Native audio: unsupported effect omitted; playback continues\n");
                        }
                        if (health.failed || health.invalid) {
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
                        // The sound worker is idle until the dispatch below: the
                        // MultiPCMs' music/effects balance changes between its frames.
                        if (auto *sound = game->sound())
                            sound->set_volumes(float(settings.music_volume) / 100.0f,
                                               float(settings.effects_volume) / 100.0f);
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

        if (link) link->poll();
        // A loop without a new emulated frame (the host is ahead of the board's 57.52 Hz
        // clock) draws nothing: the display keeps the last picture, which is the same
        // image, and the rendering time goes to the next emulated frame instead. Menus
        // and the loop that ran a frame draw as before.
        const bool present = menu || !game || simulated;
        uint64_t gpu_begin = 0, gpu_ready = 0;
        if (present) {
            gpu_begin = diagnostic_ticks_us();
            gpu.prepare_frame();
            gpu_ready = diagnostic_ticks_us();
            gfx_begin();
            const uint64_t gfx_begun = diagnostic_ticks_us();
            if (menu) {
                std::string shown_status = status;
#if DAYTONA_VITA_LINK
                if (link && game && !options) {
                    const auto *comm = game->board().comm_board();
                    shown_status += "\nIP " + link->local_ip() + " RX " + (link->rx_open() ? "YES" : "WAIT") +
                        " TX " + (link->tx_open() ? "YES" : "WAIT");
                    if (comm) shown_status += " CABINET " + std::to_string(comm->id()) + "/" +
                        std::to_string(comm->count()) + (comm->link() == rt::CommBoard::Link::Lost ? " LOST" : "");
                }
#endif
                ui.frame(float(elapsed));
                draw_menu(bool(game), options, binds, selection, capture, settings, shown_status, display_fps);
                ui.render();
            } else if (game) {
                gpu.draw(game->board().video()); // System 24 layers + polygons on the GPU (the only case)
            }
            const uint64_t drawn = diagnostic_ticks_us();
            gfx_end();
            const uint64_t gfx_ended = diagnostic_ticks_us();
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
                    core_profile.add(P::TexBuild, gpu.last_texture_us());
                    {
                        const auto tp = gpu.take_texture_probe();
                        core_profile.add(P::TexDecode, tp.decode_us);
                        core_profile.add(P::TexUpload, tp.upload_us);
                        core_profile.add(P::TexCheck, tp.check_us);
                        auto &c = core_profile.counters;
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
                        auto &note = core_profile.note();
                        note.tex_builds += tp.builds;
                        note.tex_texels += uint32_t(tp.texels);
                        note.tex_changed += tp.changed;
                        note.tex_defers += gpu.material_defers();
                        static unsigned resets_seen = 0;
                        if (gpu.cache_resets() >= resets_seen) c.tex_resets += gpu.cache_resets() - resets_seen;
                        resets_seen = gpu.cache_resets();
                    }
                    core_profile.add(P::VertexWrite, gpu.last_vertex_us());
                    core_profile.add(P::Worker2D, gpu.last_2d_worker_us()); // core 2, in parallel
                } else {
                    core_profile.add(P::Upload, draw_total); // the menu
                }
                // The gl.log write is diagnostics, not frame end (render/gl_context.cpp).
                const uint64_t gl_diagnostic = std::min<uint64_t>(vita::gl_last_diagnostic_us(), gfx_ended - drawn);
                core_profile.add(P::GfxEnd, gfx_ended - drawn - gl_diagnostic);
                core_profile.add(P::Log, gl_diagnostic);
            }
            if (!menu && game) {
                ++perf_presents;
                perf_wait_us += gpu_ready - gpu_begin;
                perf_encode_us += uint64_t(gpu.last_gpu_ms() * 1000.0);
                perf_sort_us += gpu.last_sort_us(); perf_polygon_us += gpu.last_polygon_us();
                perf_tile_us += gpu.last_tile_us(); perf_upload_us += gpu.last_upload_us();
                perf_gpu_us += diagnostic_ticks_us() - gpu_begin;
            }
        }
        // Geometry core work joined during this loop (folded in at each join).
        const rt::Geo::Stats geo_delta = take_geo_delta();
        (void)geo_delta; // unused without perf.log
        loop_profile.menu = menu;
        loop_profile.prepare = gpu_ready - gpu_begin;
        if constexpr (kDiagnostics) vita::gl_profile_loop(loop_profile);
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
                        kRendererName, int(menu), unsigned(perf_frames), unsigned(perf_presents),
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
#ifdef M2_TGP_PROFILE
                if (game) tgp_report.start(game->board().tgp().tgp_instructions());
#endif
#ifdef M2_I960_PROFILE
                if (game) i960_report.start(game->instructions(), uint64_t(game->interrupts()));
#endif
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
                    const auto pacing = audio.stats();
                    c.audio_gaps = pacing.underruns - std::min(audio_gaps_seen, pacing.underruns);
                    audio_gaps_seen = pacing.underruns;
                    c.audio_speed = pacing.ratio;
                    c.audio_queue_ms = pacing.queued_ms;
                    c.sound_clock_us = sound_clock_us;
                    vita::CoreProfiler::Header h;
                    h.index = ++perf_index;
                    const rt::Geo *geo = geometry_of(game.get());
                    h.geo_mode = rt::geo_mode_name(geo ? geo->mode() : rt::GeoMode::Sync);
                    h.renderer = kRendererName;
                    h.cpu_mhz = scePowerGetArmClockFrequency();
                    h.gpu_mhz = scePowerGetGpuClockFrequency();
                    h.bus_mhz = scePowerGetBusClockFrequency();
                    h.main_core = kMainCore; h.geo_core = kGeoCore;
                    h.sound_core = kSoundCore;
                    h.geo_threaded = geo && geo->worker().threaded && geo->mode() != rt::GeoMode::Sync;
                    h.sound_threaded = sound_worker.threaded();
                    h.native_audio = active_native_audio;
                    static char report[vita::PerfLog::kCapacity];
                    size_t size = core_profile.format(report, sizeof report, h, 1000000u);
#ifdef M2_I960_PROFILE
                    {
                        const double frames = double(c.board_frames ? c.board_frames : 1);
                        const double logic_ms = double(core_profile.total(P::Logic) + core_profile.total(P::Irq)) / 1000.0 / frames;
#ifdef M2_TGP_PROFILE
                        const double tgp_ms = tgp_report.ms_per_frame(c.board_frames, tgp_bench);
#else
                        const double tgp_ms = -1.0;
#endif
                        size += i960_report.format(report + size, sizeof report - size, c.board_frames, game->instructions(),
                                                   uint64_t(game->interrupts()), h.cpu_mhz, tgp_ms, logic_ms,
                                                   "the i960+TGP lines (game logic + vblank handler)");
                        i960_report.start(game->instructions(), uint64_t(game->interrupts()));
                    }
#endif
#ifdef M2_TGP_PROFILE
                    {
                        const double frames = double(c.board_frames ? c.board_frames : 1);
                        const double logic_ms = double(core_profile.total(P::Logic) + core_profile.total(P::Irq)) / 1000.0 / frames;
                        size += tgp_report.format(report + size, sizeof report - size, c.board_frames,
                                                  game->board().tgp().tgp_instructions(), h.cpu_mhz, logic_ms,
                                                  "the i960+TGP lines (game logic + vblank handler)", tgp_bench);
                        tgp_report.start(game->board().tgp().tgp_instructions());
                    }
#endif
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
    ui.shutdown();
    restore_clocks();
    gfx_fini();
    perf_log.close(); // join file I/O before destroying SDL synchronization
    perf_file.close();
    SDL_Quit();
    log.literal("GPU25: clean exit\n");
    return 0;
}
