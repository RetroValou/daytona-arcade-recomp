// The Vita application: start-up (clocks, cores, SDL, vitaGL, ImGui, renderer,
// audio), then the loop: the pad, the launcher (frontend/launcher.h) or one
// emulated frame of the game (frontend/session.h), the picture, the measurements
// (frontend/diagnostics.h). Settings: frontend/settings.h.
#define SDL_MAIN_HANDLED
#include "audio/audio.h"
#include "audio/native_audio.h"
#include "frontend/controls.h"
#include "frontend/diagnostics.h"
#include "frontend/input.h"
#include "frontend/launcher.h"
#include "frontend/session.h"
#include "frontend/settings.h"
#include "frontend/storage.h"
#include "render/imgui_backend.h"
#include "render/renderer.h"
#include "runtime/board/game.h"
#include "runtime/board/game_loop.h"
#include "runtime/board/test_hold.h"
#include "runtime/sound/native/native_sound_engine.h"
#include "system/async_log.h"
#include "system/core_policy.h"
#include "system/diagnostic_log.h"

#include <SDL.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <cstdio>
#include <cstdlib>
#include <string>

// The multicore flow:
//   main thread      i960 + TGP, 2D video, sound hand-off, GPU recording   core 0
//   geometry thread  the geometrizer, always pipelined: the 3D of frame N-1 is drawn
//                    with frame N (overrides/runtime/video/geo.h)                core 1
//   sound worker     the reference (emulated) sound board                  core 2
//   vitaGL 2D worker System 24 uploads + layer rectangles (render/layers_2d.cpp)    core 2
// vitaGL draws the 2D layers of frame N-1 too (two texture sets, render/layers_2d.cpp): the picture
// is the exact one, one frame late, in phase with the 3D. -1 = unpinned: scripts/build_vita.py --free-core leaves every thread
// free, and the optional fourth core usable by all of them (core_policy.h). If the
// geometry thread cannot be created, Geo stays on the main core and the fault is logged.

// Read by newlib (not LTO code): kept by "used" in the release (LTO) build.
extern "C" { __attribute__((used)) unsigned int _newlib_heap_size_user = 192 * 1024 * 1024; }

namespace {
using vita::VitaSettings, vita::app_file, vita::kDirectory, vita::read_pad, vita::map_input, vita::game_layout;
using vita::kRendererName, vita::kDiagnostics, vita::kMainCore, vita::kPinnedCores, vita::ticks_us,
    vita::diagnostic_ticks_us;
rt::TestHold test_hold; // the launcher arms it, the session applies it
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
    bool running = true, menu = true, wait_release = true;
    vita::Launcher launcher(test_hold);
    uint32_t previous_buttons = 0;
    // Back in the game (resume, start, reset): the buttons still held from the menu are
    // ignored, each until it is released; the others and the sticks work at once (not
    // the whole pad held back until every button is up: a latency when resuming with
    // the accelerator or a direction held).
    uint32_t held_from_menu = 0;
    std::string status = "CROSS SELECT. OPTIONS: GAME, DISPLAY, AUDIO, PERFORMANCE. CONTROLS: BUTTONS, STICK, STEERING.";
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
        controls.set_steer_smoothing(settings.steer_smoothing);
        controls.set_layout(game_layout());
        controls.set_gun(rt::game().gun.x_min, rt::game().gun.x_max, rt::game().gun.y_min, rt::game().gun.y_max);
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
        if (settings.use_companion && vita::companion()) {
            // A two-set package: eboot.bin is one set, <companion>.self the
            // other. Running as eboot.bin, the companion is its .self; running
            // as the .self, the companion is eboot.bin. A one-set package has
            // neither and must not relaunch itself.
            const std::string self = std::string("app0:/") + vita::companion()->set + ".self";
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
        held_from_menu &= pad.buttons; // released: counts again
        if (wait_release) {
            pressed = 0; controls.latch(pad.buttons);
            if (!pad.buttons) wait_release = false;
        }
        if (!menu && !wait_release && vita::menu_chord(pad.buttons & ~held_from_menu)) {
            menu = true; launcher.open();
            sound_ok();
            audio.pause(); native_audio.pause(); save(); clock.reset(); wait_release = true;
        }
        if (menu && !wait_release) {
            switch (launcher.handle(pressed, bool(session), settings, status, wait_release, commit_settings)) {
            case vita::MenuAction::None: break;
            case vita::MenuAction::Resume:
                menu = false; clock.reset(); wait_release = true;
                break;
            case vita::MenuAction::Start: // resume the running game, unless another ROM was selected
                if ((session && !settings.use_companion) || start_game()) { menu = false; clock.reset(); wait_release = true; }
                break;
            case vita::MenuAction::Reset:
                if (start_game()) { menu = false; clock.reset(); wait_release = true; }
                break;
            case vita::MenuAction::Quit:
                save(); settings.save(); running = false;
                break;
            }
        }

        if (!menu && wait_release) { held_from_menu = pad.buttons; wait_release = false; }
        vita::Pad game_pad = pad;
        game_pad.buttons &= ~held_from_menu;

        diag.input_done(now);
        bool simulated = false;
        if (session && !menu) {
            if (session.native_audio()) native_audio.resume();
            const int frames = clock.advance(elapsed);
            for (int n = 0; n < frames; ++n) {
                const rt::Inputs inputs = map_input(controls.sample(game_pad));
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
                if (!launcher.on_options()) shown_status += session.link_status();
                ui.frame(float(elapsed));
                launcher.draw(game != nullptr, settings, shown_status);
                ui.render();
            } else if (game) {
                vita::gl_set_upgrade_z_depth(settings.upgrade_z_depth); // the option's shader set
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
