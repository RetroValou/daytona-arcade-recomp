#include "frontend/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vita {
namespace {
// vita.cfg keys of the controls (rt::Control order); value = button bit. The first
// twelve are the keys every earlier version saved.
constexpr const char *kBindKeys[ControlCount] = {"bind_accelerate", "bind_brake",
    "bind_steer_left", "bind_steer_right", "bind_shift_up", "bind_shift_down", "bind_start",
    "bind_coin", "bind_view1", "bind_view2", "bind_view3", "bind_view4", "bind_handbrake",
    "bind_up", "bind_down", "bind_left", "bind_right", "bind_punch", "bind_kick", "bind_guard"};
} // namespace

void VitaSettings::sanitize() {
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
    steer_curve = std::clamp(steer_curve, 0, 2);
    draw_distance = std::clamp(draw_distance, -2, 2);
    bindings.sanitize(game_layout());
}

void VitaSettings::load(const char *path) {
    FILE *f = std::fopen(path, "r");
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
        else if (!std::strcmp(key, "skip_launcher")) skip_launcher = value != 0;
        else if (!std::strcmp(key, "save_backup")) save_backup = value != 0;
        else if (!std::strcmp(key, "steer_curve")) steer_curve = value;
        else if (!std::strcmp(key, "draw_distance")) draw_distance = value;
        else if (!std::strcmp(key, "cpu_clock")) cpu_clock = value;
        else if (!std::strcmp(key, "gpu_clock")) gpu_clock = value;
        else if (!std::strcmp(key, "volume")) volume = value;
        else if (!std::strcmp(key, "music_volume")) music_volume = value;
        else if (!std::strcmp(key, "effects_volume")) effects_volume = value;
        else if (!std::strcmp(key, "mute")) mute = value != 0;
        else if (!std::strcmp(key, "native_audio")) native_audio = value != 0;
        else if (!std::strcmp(key, "deadzone")) deadzone = value;
        else if (!std::strcmp(key, "steer_invert")) steer_invert = value != 0;
        else for (int a = 0; a < ControlCount; ++a)
            if (!std::strcmp(key, kBindKeys[a])) bindings.button[a] = uint32_t(value);
    }
    std::fclose(f); sanitize();
}

bool VitaSettings::save(const char *path) const {
    const std::string temp = std::string(path) + ".tmp";
    FILE *f = std::fopen(temp.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "cpu_clock=%d\ngpu_clock=%d\nvolume=%d\nmute=%d\nnative_audio=%d\ndeadzone=%d\nsteer_invert=%d\n",
                 cpu_clock, gpu_clock, volume, int(mute), int(native_audio), deadzone, int(steer_invert));
    std::fprintf(f, "music_volume=%d\neffects_volume=%d\n", music_volume, effects_volume);
    std::fprintf(f, "skip_launcher=%d\nsave_backup=%d\n", int(skip_launcher), int(save_backup));
    std::fprintf(f, "fourth_core=%d\n", int(fourth_core));
    std::fprintf(f, "link_enabled=%d\nlink_sync=%d\nlink_port=%d\nnext_port=%d\n",
        int(link_enabled), int(link_sync), link_port, next_port);
    for (int i = 0; i < 4; ++i) std::fprintf(f, "next_ip%d=%d\n", i, next_ip[i]);
    std::fprintf(f, "steer_curve=%d\n", steer_curve);
    std::fprintf(f, "draw_distance=%d\n", draw_distance);
    for (int a = 0; a < ControlCount; ++a)
        if (bindings.button[a]) std::fprintf(f, "%s=%u\n", kBindKeys[a], unsigned(bindings.button[a]));
    bool ok = std::fflush(f) == 0;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { std::remove(temp.c_str()); return false; }
    std::remove(path);
    return std::rename(temp.c_str(), path) == 0;
}

} // namespace vita
