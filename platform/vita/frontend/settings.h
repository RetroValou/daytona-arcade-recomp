#pragma once

// The launcher's settings, saved in vita.cfg ("key=value" lines; keys a version does
// not know are skipped, so files of older and newer versions both load).

#include "frontend/input.h"
#include "frontend/storage.h"

#include <string>

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

namespace vita {

constexpr int kDefaultCpuClock = DAYTONA_VITA_DEFAULT_CPU_MHZ;
constexpr int kDefaultGpuClock = DAYTONA_VITA_DEFAULT_GPU_MHZ;

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
    Bindings bindings = Bindings::defaults(game_layout()); // Controls page of the launcher

    void defaults() { *this = VitaSettings{}; }
    void sanitize(); // every value back into its menu's range
    void load(const char *path = kConfig); // a missing file keeps the defaults
    bool save(const char *path = kConfig) const; // through path.tmp
};

} // namespace vita
