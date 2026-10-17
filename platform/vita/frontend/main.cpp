#define SDL_MAIN_HANDLED
// Link play compiled in (build_vita.py: the games it was tested on, scripts/games.py
// vita_link). Off: no SceNet code, no link rows, link_* settings kept in vita.cfg.
#ifndef DAYTONA_VITA_LINK
#define DAYTONA_VITA_LINK 1
#endif
#include "audio/audio.h"
#include "audio/native_audio.h"
#include "runtime/sound/native/native_sound_engine.h"
#include "audio/sound_worker.h"
#include "frontend/controls.h"
#include "frontend/diagnostics.h"
#include "frontend/input.h"
#include "frontend/session.h"
#include "frontend/settings.h"
#include "frontend/storage.h"
#include "system/diagnostic_log.h"
#include "system/async_log.h"
#include "system/core_policy.h"
#include "system/core_profile.h"
// The renderer: vitaGL (render/).
#include "render/renderer.h"
#include "render/imgui_backend.h"
#include "runtime/board/game_loop.h"
#include "runtime/cpu/i960_profile.h"
#include "runtime/tgp/tgp_profile.h"
#include "runtime/board/game.h"
#include "runtime/board/test_hold.h"
#include "runtime/rom/rom_import.h"

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

// Read by newlib (not LTO code): kept by "used" in the release (LTO) build.
extern "C" { __attribute__((used)) unsigned int _newlib_heap_size_user = 192 * 1024 * 1024; }

namespace {
using vita::VitaSettings;
using vita::app_file, vita::load_nv, vita::save_nv, vita::kDirectory, vita::kRom;
using vita::read_pad, vita::map_input, vita::game_layout;

using vita::kRendererName, vita::kDiagnostics, vita::kProfile, vita::kMainCore, vita::kGeoCore, vita::kSoundCore,
    vita::kPinnedCores, vita::ticks_us, vita::diagnostic_ticks_us, vita::geometry_of;
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
    OptDistance, OptSteerCurve, OptSkipLauncher, OptSaveBackup, OptFourthCore,
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
// listed: none fixed at build time (renderer, fullscreen), none the game has no
// use for (the native audio engine without
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
                rt::game().title, fps, scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(), kRendererName);
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
    static const char* distances[] = {"DISTANCE: SHORTEST", "DISTANCE: SHORTER", "DISTANCE: DEFAULT", "DISTANCE: FURTHER", "DISTANCE: FURTHEST"};
    values[OptDistance]=distances[settings.draw_distance + 2];
    values[OptTestHold]=test_hold.armed() ? "HOLD TEST BUTTON: ARMED (START / RESUME)" : "HOLD TEST BUTTON: OFF";
    values[OptDefaults]="RESET DEFAULTS"; values[OptBack]="BACK";
    values[OptFourthCore]=!settings.fourth_core ? "4TH CORE: OFF" :
        vita::fourth_core_active() ? "4TH CORE: ENABLED" : "4TH CORE: UNAVAILABLE (PLUGIN REQUIRED)";
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
    log.literal("GPU25: starting\n");
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
    if (!vita::gl_init()) {
        log.fault("GPU25: %s initialization failed: %s\n", kRendererName, vita::gl_error());
        SDL_Quit();
        restore_clocks();
        return 1;
    }
    log.literal("GPU25 stage: graphics init done; renderer textures begin\n");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vita::ImGuiVita ui;
    if (!ui.init()) {
        log.fault("ImGui font texture allocation failed\n");
        vita::gl_fini(); SDL_Quit(); restore_clocks(); return 1;
    }

    vita::GpuGlRenderer gpu;
    if (!gpu.ok()) {
        log.fault("GPU25: renderer texture allocation failed\n");
        gpu.shutdown(); ui.shutdown(); vita::gl_fini(); SDL_Quit(); restore_clocks(); return 1;
    }
    if (gpu.worker_2d_threaded())
        log.log("GPU25 2D worker: thread on core %d\n", gpu.worker_2d_core());
    else
        log.fault("GPU25 2D worker: thread unavailable (result=%d), 2D prepared on the main core\n",
                  gpu.worker_2d_result());
    log.literal("GPU25 stage: renderer ready; audio begin\n");
    vita::Audio audio;
    vita::NativeAudio<snd::NativeSoundEngine> native_audio;
    if (!audio.open()) log.fault("GPU25: audio unavailable: %s\n", SDL_GetError());
    audio.volume(float(settings.volume) / 100.0f);
    audio.mute(settings.mute);
    log.literal("GPU25 stage: audio done; main loop ready\n");

    vita::Diagnostics diag;
    vita::AsyncLog perf_log;
    if constexpr (kDiagnostics) perf_log.open(log, ticks_us);
    log.log("GPU25 periodic log worker: threaded=%d; unavailable worker drops periodic records only\n",
            int(perf_log.threaded()));
    // The game being played (frontend/session.h): board, link, sound worker.
    vita::Session session(audio, native_audio, gpu, diag, log, perf_log);
    diag.open(log, unsigned(main_affinity), main_affinity_result, session.worker());
    vita::Controls controls;
    vita::FrameClock clock(rt::GameLoop::kFrameHz, 1);
    bool running = true, menu = true, options = false, binds = false, wait_release = true;
    int selection = 0, capture = -1; // capture: action waiting for its new button (Controls page)
    uint32_t previous_buttons = 0;
    std::string status = "CROSS SELECT. OPTIONS INCLUDE CLOCKS, AUDIO, DISPLAY AND CONTROLS.";
    uint64_t last = ticks_us();

    // The sound worker joined; a fault stops the game and shows the menu with it.
    auto sound_ok = [&] {
        if (session.finish_sound()) return true;
        status = session.error();
        menu = true;
        return false;
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
    };
    auto commit_settings = [&](bool set_clocks) {
        if (!sound_ok()) return;
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

    auto save = [&] { return sound_ok() && session.save_nv(settings); };
    auto start_game = [&]() -> bool {
        if (!save()) { if (session) status = "SAVE FAILED; ROM SWITCH CANCELLED."; return false; }
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
            native_audio.close(); audio.close(); session.release_all();
            glFinish();
            sceGxmDisplayQueueFinish();
            const int rc = sceAppMgrLoadExec(executable.c_str(), nullptr, nullptr);
            status = "ROM SWITCH FAILED: " + std::to_string(rc);
            return false;
        }
        session.stop();
        status = std::string("LOADING ") + M2_ROMSET + "...";
        gpu.prepare_frame();
        vita::gl_begin_frame();
        ui.frame(1.f / 60);
        ImGui::SetNextWindowPos({24, 24}); ImGui::SetNextWindowSize({912, 496});
        ImGui::Begin("Starting", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End(); ui.render();
        vita::gl_end_frame();
        if (!session.start(settings, test_hold)) { status = session.error(); return false; }
        clock.reset();
        controls = vita::Controls{};
        apply_preferences();
        // The menu chord, then the game's controls that are not its usual ones
        // (the shift, hand brake, view and fighting buttons), with their buttons.
        status = "START+SELECT MENU.";
        const vita::Layout layout = game_layout();
        for (int i = 0; i < layout.count; ++i) {
            const rt::Control c = layout.defs[i].control;
            if (c == rt::Control::Accelerate || c == rt::Control::Brake || c == rt::Control::Start ||
                c == rt::Control::Coin || c == rt::Control::SteerLeft || c == rt::Control::SteerRight ||
                c == rt::Control::Up || c == rt::Control::Down || c == rt::Control::Left || c == rt::Control::Right)
                continue;
            status += std::string(" ") + layout.defs[i].name + " " + vita::button_name(settings.bindings.button[int(c)]) + ".";
        }
        wait_release = true;
        return true;
    };

    while (running) {
        const uint64_t now = ticks_us();
        const double elapsed = double(now - last) / 1000000.0;
        last = now;
        const bool menu_at_start = menu;
        diag.begin_loop(now);
        const vita::Pad pad = read_pad();
        uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (wait_release) {
            pressed = 0; controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }
        if (!menu && !wait_release && vita::menu_chord(pad.buttons)) {
            menu = true; options = binds = false; selection = 0; capture = -1;
            sound_ok();
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
                    case OptDistance:
                        if (step) {
                            settings.draw_distance = std::clamp(settings.draw_distance + (direction < 0 ? -1 : 1), -2, 2);
                            changed = true;
                        }
                        break;
                    case OptSteerCurve:
                        if (step) { settings.steer_curve = (settings.steer_curve + (direction < 0 ? 2 : 1)) % 3; changed = true; }
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
                if ((pressed & vita::Circle) && session) {
                    menu = false; clock.reset(); wait_release = true;
                } else if (pressed & vita::Cross) {
                    if (selection == 0) {
                        const bool same_rom = !settings.use_companion;
                        if ((session && same_rom) || start_game()) { menu = false; clock.reset(); wait_release = true; }
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

        diag.input_done(now);
        bool simulated = false;
        if (session && !menu) {
            if (session.native_audio()) native_audio.resume();
            const int frames = clock.advance(elapsed);
            for (int n = 0; n < frames; ++n) {
                const rt::Inputs inputs = map_input(controls.sample(wait_release ? vita::Pad{} : pad));
                if (!session.run_frame(inputs, test_hold, settings)) {
                    status = session.error();
                    menu = true;
                    break;
                }
                simulated = true;
            }
        } else clock.reset();

        session.poll_link();
        // A loop without a new emulated frame (the host is ahead of the board's 57.52 Hz
        // clock) draws nothing: the display keeps the last picture, which is the same
        // image, and the rendering time goes to the next emulated frame instead. Menus
        // and the loop that ran a frame draw as before.
        rt::GameLoop *const game = session.game();
        const bool present = menu || !game || simulated;
        uint64_t gpu_begin = 0, gpu_ready = 0;
        if (present) {
            gpu_begin = diagnostic_ticks_us();
            gpu.prepare_frame();
            gpu_ready = diagnostic_ticks_us();
            vita::gl_begin_frame();
            const uint64_t gfx_begun = diagnostic_ticks_us();
            if (menu) {
                std::string shown_status = status;
                if (!options) shown_status += session.link_status();
                ui.frame(float(elapsed));
                draw_menu(bool(game), options, binds, selection, capture, settings, shown_status, diag.display_fps());
                ui.render();
            } else if (game) {
                gpu.draw(game->board().video()); // System 24 layers + polygons on the GPU (the only case)
            }
            const uint64_t drawn = diagnostic_ticks_us();
            vita::gl_end_frame();
            const uint64_t gfx_ended = diagnostic_ticks_us();
            diag.presented(gpu, {gpu_begin, gpu_ready, gfx_begun, drawn, gfx_ended, !menu && game != nullptr});
        }
        const vita::Diagnostics::LoopEnd loop_end{now, gpu_ready - gpu_begin, menu, menu_at_start, simulated,
                                                  session.native_audio()};
        diag.loop_done(loop_end, game, gpu, perf_log, native_audio);
        if (menu || !simulated) SDL_Delay(1);
        diag.window_done(loop_end, game, audio, native_audio, session.worker());
    }

    session.finish_sound();
    session.close_worker();
    save();
    settings.save();
    native_audio.close();
    audio.close();
    gpu.shutdown();
    ui.shutdown();
    restore_clocks();
    vita::gl_fini();
    perf_log.close(); // join file I/O before destroying SDL synchronization
    diag.close();
    SDL_Quit();
    log.literal("GPU25: clean exit\n");
    return 0;
}
