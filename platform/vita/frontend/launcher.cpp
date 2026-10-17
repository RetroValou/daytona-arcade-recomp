#include "frontend/launcher.h"

#include "frontend/diagnostics.h"
#include "system/core_policy.h"

#include "imgui.h"
#include <psp2/power.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifndef DAYTONA_VITA_LINK
#define DAYTONA_VITA_LINK 1
#endif

namespace vita {

// A package holds one game: this set, and the game's companion set when
// bundled (Game::companion: Daytona's two sets), which the ROM option
// switches to. Other games are packages of their own (their own title IDs:
// scripts/games.py).
const rt::Game *companion() { return rt::game().companion ? rt::find_game(rt::game().companion) : nullptr; }

namespace {
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
} // namespace

void Launcher::draw(bool have_game, const VitaSettings &settings, const std::string &status, double fps) {
    ImGui::SetNextWindowPos({12, 12});
    ImGui::SetNextWindowSize({936, 520});
    ImGui::Begin(rt::game().title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::Text("%s | %.1f FPS | CPU %d MHz | GPU %d MHz | %s",
                rt::game().title, fps, scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(), kRendererName);
    if (ImGui::Button("Game")) { options_ = binds_ = false; selection_ = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Options")) { options_ = true; binds_ = false; selection_ = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Controls")) { binds_ = true; options_ = false; selection_ = 0; }
    ImGui::SameLine(); ImGui::TextDisabled("D-pad navigate | X select | O back");
    ImGui::Separator();
    static int previous_selection = -1;
    static bool previous_options = false, previous_binds = false;
    const bool scroll_to_selection = selection_ != previous_selection || options_ != previous_options ||
                                     binds_ != previous_binds;
    if (ImGui::BeginChild("settings", {0, -100}, ImGuiChildFlags_None)) {
        if (binds_) {
            char rows[kMaxBindRows][64];
            int actions[kMaxBindRows];
            const int bound = bind_rows(actions);
            for (int i = 0; i < bound; ++i) {
                const int a = actions[i];
                const char *label = a == -1 ? "RESET CONTROLS TO DEFAULTS" : "BACK";
                if (a >= 0) {
                    std::snprintf(rows[i], sizeof rows[i], "%s: %s", game_layout().name(a),
                                  capture_ == a ? "PRESS A BUTTON..." : vita::button_name(settings.bindings.button[a]));
                    label = rows[i];
                }
                ImGui::PushID(i);
                if (ImGui::Selectable(label, selection_ == i, 0, {760, 32})) { selection_ = i; touch_buttons_ = vita::Cross; }
                if (selection_ == i && scroll_to_selection) ImGui::SetScrollHereY();
                if (a >= 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("<")) { selection_ = i; touch_buttons_ = vita::Left; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(">")) { selection_ = i; touch_buttons_ = vita::Right; }
                }
                ImGui::PopID();
            }
        } else if (!options_) {
            const char *labels[] = {have_game ? "Resume / launch selected ROM" : "Start game", "Reset game", "Options",
                                    "Controls", "Quit"};
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(labels[i], selection_ == i, 0, {0, 36})) { selection_ = i; touch_buttons_ = vita::Cross; }
                if (selection_ == i && scroll_to_selection) ImGui::SetScrollHereY();
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
    values[OptTestHold]=test_hold_.armed() ? "HOLD TEST BUTTON: ARMED (START / RESUME)" : "HOLD TEST BUTTON: OFF";
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
                if (ImGui::Selectable(values[rows[i]], selection_ == i, 0, {760, 32})) { selection_ = i; touch_buttons_ = vita::Cross; }
                if (selection_ == i && scroll_to_selection) ImGui::SetScrollHereY();
                ImGui::SameLine();
                if (ImGui::SmallButton("<")) { selection_ = i; touch_buttons_ = vita::Left; }
                ImGui::SameLine();
                if (ImGui::SmallButton(">")) { selection_ = i; touch_buttons_ = vita::Right; }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    previous_selection = selection_; previous_options = options_; previous_binds = binds_;
    ImGui::Separator();
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::TextDisabled("START+SELECT menu | SELECT+TRIANGLE test | SELECT+SQUARE service");
    ImGui::End();
}


MenuAction Launcher::handle(uint32_t pressed, bool have_game, VitaSettings &settings, std::string &status,
                            bool &wait_release, const std::function<void(bool)> &commit) {
        const uint32_t pad_pressed = pressed;
        pressed |= touch_buttons_;
        touch_buttons_ = 0;
        if (binds_ && capture_ >= 0) {
            // Any pad button (touch ignored) becomes the action's button; pressing the
            // current one keeps it. An action that had it takes the old one (swap).
            if (pad_pressed) {
                const uint32_t button = pad_pressed & (~pad_pressed + 1);
                settings.bindings.bind(capture_, button);
                commit(false);
                status = std::string(game_layout().name(capture_)) + ": " + vita::button_name(button) +
                    ". START+SELECT MENU AND SELECT+TRIANGLE/SQUARE TEST/SERVICE STAY FIXED.";
                capture_ = -1; wait_release = true;
            }
        } else if (binds_) {
            int actions[kMaxBindRows];
            const int bound = bind_rows(actions);
            selection_ = std::clamp(selection_, 0, bound - 1);
            if (pressed & vita::Up) selection_ = (selection_ + bound - 1) % bound;
            if (pressed & vita::Down) selection_ = (selection_ + 1) % bound;
            const int action = actions[selection_];
            const int direction = (pressed & vita::Left) ? -1 : ((pressed & vita::Right) ? 1 : 0);
            if (pressed & vita::Circle) { binds_ = false; selection_ = 3; wait_release = true; }
            else if (action >= 0 && (pressed & vita::Cross)) {
                capture_ = action; wait_release = true;
                status = std::string("PRESS THE NEW BUTTON FOR ") + game_layout().name(action) +
                    " (ITS CURRENT BUTTON KEEPS IT).";
            } else if (action >= 0 && direction) {
                settings.bindings.cycle(action, direction);
                commit(false);
            } else if (action == -1 && (pressed & vita::Cross)) {
                settings.bindings = vita::Bindings::defaults(game_layout());
                commit(false);
                status = "CONTROLS RESET TO DEFAULTS. " + status;
            } else if (action == -2 && (pressed & vita::Cross)) {
                binds_ = false; selection_ = 3; wait_release = true;
            }
        } else if (options_) {
            Option rows[kOptions];
            const int listed = listed_options(settings, rows);
            selection_ = std::clamp(selection_, 0, listed - 1);
            if (pressed & vita::Up) selection_ = (selection_ + listed - 1) % listed;
            if (pressed & vita::Down) selection_ = (selection_ + 1) % listed;
            const Option option = rows[selection_];
            if (pressed & vita::Circle) { options_ = false; selection_ = 2; wait_release = true; }
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
                    if (step) { if (test_hold_.armed()) test_hold_.cancel(); else test_hold_.arm(); }
                    break;
                case OptDefaults:
                    if (activate) { test_hold_.cancel(); settings.defaults(); changed = clocks_changed = true; }
                    break;
                case OptBack:
                    if (activate) { options_ = false; selection_ = 2; wait_release = true; }
                    break;
                case kOptions:
                    break;
                }
                if (changed) {
                    commit(clocks_changed);
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
                    if (options_) {
                        const int now_listed = listed_options(settings, rows);
                        for (int i = 0; i < now_listed; ++i)
                            if (rows[i] == option) selection_ = i;
                    }
                }
            }
        } else {
            if (pressed & vita::Up) selection_ = (selection_ + 4) % 5;
            if (pressed & vita::Down) selection_ = (selection_ + 1) % 5;
            if ((pressed & vita::Circle) && have_game) return MenuAction::Resume;
            if (pressed & vita::Cross) {
                if (selection_ == 0) return MenuAction::Start;
                if (selection_ == 1) return MenuAction::Reset;
                if (selection_ == 2) { options_ = true; selection_ = 0; wait_release = true; }
                else if (selection_ == 3) {
                    binds_ = true; selection_ = 0; wait_release = true;
                    status = "CROSS: PRESS THE NEW BUTTON. LEFT/RIGHT: CYCLE. STICKS STAY FIXED.";
                } else if (selection_ == 4) return MenuAction::Quit;
            }
        }
    return MenuAction::None;
}

} // namespace vita
