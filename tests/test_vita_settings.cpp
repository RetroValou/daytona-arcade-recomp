// The Vita launcher's settings (platform/vita/frontend/settings.h): vita.cfg written and
// read back, values out of range brought back into the menus' ranges, unknown keys
// and a missing file harmless, the file replaced whole through its temporary.
#include "frontend/settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed: %s (line %d)\n", #x, __LINE__); std::abort(); } } while (0)

namespace {
void write(const std::string &path, const char *text) {
    FILE *f = std::fopen(path.c_str(), "w");
    CHECK(f);
    std::fputs(text, f);
    std::fclose(f);
}
} // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string path = dir + "/test_vita_settings.cfg";
    std::remove(path.c_str());

    // A missing file keeps the defaults.
    vita::VitaSettings missing;
    missing.load(path.c_str());
    CHECK(missing.cpu_clock == vita::kDefaultCpuClock && missing.volume == 80 && !missing.link_enabled);

    // Round trip.
    vita::VitaSettings a;
    a.cpu_clock = 333; a.gpu_clock = 111; a.volume = 55; a.mute = true; a.deadzone = 20;
    a.link_enabled = true; a.link_port = 2000; a.next_ip[3] = 77; a.draw_distance = -1; a.save_backup = true;
    a.bindings.bind(int(vita::game_layout().defs[0].control), vita::Square); // the others swap as in the menu
    CHECK(a.save(path.c_str()));
    FILE *temp = std::fopen((path + ".tmp").c_str(), "r");
    CHECK(!temp); // renamed over vita.cfg
    vita::VitaSettings b;
    b.load(path.c_str());
    CHECK(b.cpu_clock == 333 && b.gpu_clock == 111 && b.volume == 55 && b.mute && b.deadzone == 20);
    CHECK(b.link_enabled && b.link_port == 2000 && b.next_ip[3] == 77 && b.next_address() == "192.168.1.77");
    CHECK(b.draw_distance == -1 && b.save_backup);
    for (int i = 0; i < vita::ControlCount; ++i) CHECK(b.bindings.button[i] == a.bindings.button[i]);
    CHECK(!b.use_companion); // not saved

    // Out of range values and unknown keys.
    write(path, "cpu_clock=123\ngpu_clock=166\nvolume=250\ndeadzone=-3\nlink_port=0\nnext_ip0=999\n"
                "aspect=2\nsteer_curve=-1\ndraw_distance=7\nfuture_option=1\nnot a line\n");
    vita::VitaSettings c;
    c.load(path.c_str());
    CHECK(c.cpu_clock == vita::kDefaultCpuClock && c.gpu_clock == 166); // not a menu choice: the default
    CHECK(c.volume == 100 && c.deadzone == 0 && c.link_port == 1 && c.next_ip[0] == 255);
    CHECK(c.steer_curve == 0 && c.draw_distance == 2); // aspect=: an old key, skipped

    std::remove(path.c_str());
    std::puts("vita settings: ok");
    return 0;
}
