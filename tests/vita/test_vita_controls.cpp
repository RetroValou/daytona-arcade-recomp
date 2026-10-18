#include "../../platform/vita/frontend/controls.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)
using rt::Control;
using rt::PadButton;
// The games' layouts as their records list them (src/games/: daytona.cpp, srallyc.cpp, vf2.cpp).
constexpr rt::ControlDef kDaytona[] = {
    {Control::Accelerate, "ACCELERATE", PadButton::R},   {Control::Brake, "BRAKE", PadButton::L},
    {Control::SteerLeft, "STEER LEFT", PadButton::Left}, {Control::SteerRight, "STEER RIGHT", PadButton::Right},
    {Control::ShiftUp, "SHIFT UP", PadButton::Up},       {Control::ShiftDown, "SHIFT DOWN", PadButton::Down},
    {Control::Start, "START", PadButton::Start},         {Control::Coin, "COIN", PadButton::Select},
    {Control::View1, "VIEW 1", PadButton::Cross},        {Control::View2, "VIEW 2", PadButton::Circle},
    {Control::View3, "VIEW 3", PadButton::Square},       {Control::View4, "VIEW 4", PadButton::Triangle},
};
constexpr rt::ControlDef kRally[] = {
    {Control::Accelerate, "ACCELERATE", PadButton::R},   {Control::Brake, "BRAKE", PadButton::L},
    {Control::SteerLeft, "STEER LEFT", PadButton::Left}, {Control::SteerRight, "STEER RIGHT", PadButton::Right},
    {Control::ShiftUp, "SHIFT UP", PadButton::Up},       {Control::ShiftDown, "SHIFT DOWN", PadButton::Down},
    {Control::HandBrake, "HAND BRAKE", PadButton::Cross}, {Control::View1, "VIEW", PadButton::Triangle},
    {Control::Start, "START", PadButton::Start},         {Control::Coin, "COIN", PadButton::Select},
};
constexpr rt::ControlDef kFighter[] = {
    {Control::Up, "UP", PadButton::Up},           {Control::Down, "DOWN", PadButton::Down},
    {Control::Left, "LEFT", PadButton::Left},     {Control::Right, "RIGHT", PadButton::Right},
    {Control::Punch, "PUNCH", PadButton::Cross},  {Control::Kick, "KICK", PadButton::Circle},
    {Control::Guard, "GUARD", PadButton::Square}, {Control::Start, "START", PadButton::Start},
    {Control::Coin, "COIN", PadButton::Select},
};
constexpr vita::Layout kDaytonaLayout{kDaytona, int(std::size(kDaytona))};
constexpr vita::Layout kRallyLayout{kRally, int(std::size(kRally))};
constexpr vita::Layout kFighterLayout{kFighter, int(std::size(kFighter))};
// Virtua Cop (vcop.cpp): one player's light gun, the trigger on Cross.
constexpr rt::ControlDef kGun[] = {
    {Control::Trigger, "TRIGGER", PadButton::Cross},
    {Control::Start, "START", PadButton::Start},
    {Control::Coin, "COIN", PadButton::Select},
};
constexpr vita::Layout kGunLayout{kGun, int(std::size(kGun))};
constexpr int ix(Control c) { return int(c); }
vita::Controls driving() {
    vita::Controls c;
    c.set_layout(kDaytonaLayout);
    c.set_bindings(vita::Bindings::defaults(kDaytonaLayout));
    return c;
}
int main() {
    vita::Controls controls = driving();
    auto in = controls.sample({});
    CHECK(in.steer == 0x80 && in.accel == 0x20 && in.brake == 0x20);
    CHECK(in.in0 == 0xff && in.in1 == 0xaf && in.in2 == 0xff);
    int last = 0;
    for (int v = 0; v <= 255; ++v) {
        in = controls.sample({0, uint8_t(v), 128});
        CHECK(in.steer >= 0x20 && in.steer <= 0xe0 && in.steer >= last);
        last = in.steer;
    }
    CHECK(controls.sample({0, 0, 128}).steer == 0x20);
    CHECK(controls.sample({0, 255, 128}).steer == 0xe0);
    CHECK(controls.sample({0, 128, 0}).accel == 0xe0);
    CHECK(controls.sample({0, 128, 255}).brake == 0xe0);
    CHECK(controls.sample({vita::L | vita::R}).accel == 0xe0);
    CHECK(controls.sample({vita::L | vita::R}).brake == 0xe0);
    CHECK(controls.sample({vita::Left}).steer == 0x20);
    CHECK(controls.sample({vita::Right}).steer == 0xe0);
    CHECK(controls.sample({vita::Left | vita::Right}).steer == 0x80);
    controls.sample({});
    controls.sample({vita::Up}); CHECK(controls.gear() == 2);
    for (int i = 0; i < 10; ++i) controls.sample({vita::Up});
    CHECK(controls.gear() == 2);
    for (int i = 0; i < 10; ++i) { controls.sample({}); controls.sample({vita::Up}); }
    CHECK(controls.gear() == 4);
    CHECK((controls.sample({}).in1 & 0x70) == 0x50);
    for (int i = 0; i < 10; ++i) { controls.sample({}); controls.sample({vita::Down}); }
    CHECK(controls.gear() == 1);
    in = controls.sample({vita::Cross | vita::Circle | vita::Square | vita::Triangle});
    CHECK((in.in0 & 0xe0) == 0 && (in.in1 & 1) == 0);
    CHECK(controls.sample({vita::Select}).in0 == 0xff);
    CHECK((controls.sample({}).in0 & 1) == 0);
    CHECK((controls.sample({vita::Start}).in0 & 0x10) == 0);
    CHECK(controls.sample({vita::Start | vita::Select}).in0 == 0xff);
    CHECK(vita::menu_chord(vita::Start | vita::Select));
    CHECK(!vita::menu_chord(vita::Start));
    in = controls.sample({vita::Select | vita::Triangle});
    CHECK(in.in0 == 0xfb && (in.in1 & 1) != 0); // Test, no coin/view.
    in = controls.sample({vita::Select | vita::Square});
    CHECK(in.in0 == 0xf7); // Service, no coin/view.
    CHECK(controls.sample({}).in0 == 0xff);
    controls.sample({vita::Select});
    CHECK(controls.sample({vita::Select | vita::Triangle}).in0 == 0xfb);
    CHECK(controls.sample({}).in0 == 0xff);
    CHECK(controls.sample({vita::Start | vita::Select | vita::Triangle}).in0 == 0xff);
    // Test every digital combination; no ADC or active-low port overflow.
    for (uint32_t buttons = 0; buttons < 4096; ++buttons) {
        in = controls.sample({buttons});
        CHECK(in.steer >= 32 && in.steer <= 224);
        CHECK(in.accel >= 32 && in.accel <= 224);
        CHECK(in.brake >= 32 && in.brake <= 224);
        CHECK(controls.gear() >= 1 && controls.gear() <= 4);
        if (vita::menu_chord(buttons)) CHECK(in.in0 == 0xff);
    }
    for (int curve = -1; curve <= 3; ++curve) {
        vita::Controls curved = driving();
        curved.set_steer_curve(curve);
        int previous = 0;
        for (int v = 0; v < 256; ++v) {
            const int steer = curved.sample({0, uint8_t(v), 128}).steer;
            CHECK(steer >= previous && steer >= 32 && steer <= 224);
            previous = steer;
        }
        CHECK(curved.sample({0, 0, 128}).steer == 32);
        CHECK(curved.sample({0, 128, 128}).steer == 128);
        CHECK(curved.sample({0, 255, 128}).steer == 224);
        CHECK(curved.sample({vita::Left, 255, 128}).steer == 32);
        CHECK(curved.sample({vita::Right, 0, 128}).steer == 224);
        const int right = curved.sample({0, 192, 128}).steer;
        CHECK(right <= (curve <= 0 ? 171 : curve == 1 ? 147 : 136));
        curved.set_steer_invert(true);
        CHECK(curved.sample({0, 192, 128}).steer == 256 - right);
    }
    // Rebinding: one button per control; taking a used button swaps the two.
    {
        const vita::Bindings defaults = vita::Bindings::defaults(kDaytonaLayout);
        vita::Bindings b = defaults;
        b.bind(ix(Control::Accelerate), vita::Cross);
        CHECK(b.button[ix(Control::Accelerate)] == vita::Cross && b.button[ix(Control::View1)] == vita::R);
        b.bind(ix(Control::ShiftUp), vita::Triangle);
        CHECK(b.button[ix(Control::ShiftUp)] == vita::Triangle && b.button[ix(Control::View4)] == vita::Up);
        b.bind(ix(Control::Brake), vita::Cross | vita::Circle); // not a single button: ignored
        b.bind(vita::ControlCount, vita::Cross);
        b.bind(ix(Control::HandBrake), vita::Cross); // a control Daytona does not have: ignored
        CHECK(b.button[ix(Control::Brake)] == vita::L && b.button[ix(Control::HandBrake)] == 0);
        vita::Controls remapped = driving();
        remapped.set_bindings(b);
        CHECK(remapped.sample({vita::Cross}).accel == 0xe0);
        CHECK(remapped.sample({vita::R}).accel == 0x20);
        in = remapped.sample({vita::R});
        CHECK((in.in0 & 0x20) == 0);
        remapped.sample({});
        remapped.sample({vita::Triangle}); CHECK(remapped.gear() == 2);
        remapped.sample({});
        remapped.sample({vita::Up}); CHECK(remapped.gear() == 2);
        in = remapped.sample({vita::Up});
        CHECK((in.in1 & 1) == 0);
        // Coin on another button still fires on release; Select chords stay fixed.
        b = defaults;
        b.bind(ix(Control::Coin), vita::L);
        CHECK(b.button[ix(Control::Brake)] == vita::Select);
        remapped.set_bindings(b);
        remapped.sample({});
        CHECK(remapped.sample({vita::L}).in0 == 0xff);
        CHECK((remapped.sample({}).in0 & 1) == 0);
        CHECK(remapped.sample({vita::Select | vita::Triangle}).in0 == 0xfb);
        CHECK(remapped.sample({vita::Start | vita::Select}).in0 == 0xff);
        // Cycling visits all twelve buttons and keeps every binding unique.
        b = defaults;
        for (int i = 0; i < 12; ++i) {
            b.cycle(ix(Control::Coin), 1);
            uint32_t seen = 0;
            for (uint32_t v : b.button) {
                if (!v) continue;
                CHECK(vita::single_button(v) && !(seen & v));
                seen |= v;
            }
        }
        CHECK(b.button[ix(Control::Coin)] == vita::Select);
        b = defaults;
        b.cycle(ix(Control::Coin), -1);
        CHECK(b.button[ix(Control::Coin)] == vita::Start && b.button[ix(Control::Start)] == vita::Select);
        // Hand-edited duplicates reset the whole set.
        b.button[ix(Control::View1)] = b.button[ix(Control::View2)];
        b.sanitize(kDaytonaLayout);
        CHECK(b.button[ix(Control::View1)] == vita::Cross && b.button[ix(Control::Coin)] == vita::Select);
        // A file saved for another layout: Daytona's views have no hand brake: the
        // rally's defaults; its own extra entries are cleared.
        b = defaults;
        b.sanitize(kRallyLayout);
        CHECK(b.button[ix(Control::HandBrake)] == vita::Cross && b.button[ix(Control::View1)] == vita::Triangle);
        CHECK(b.button[ix(Control::View2)] == 0 && b.button[ix(Control::View4)] == 0);
        CHECK(!std::strcmp(vita::button_name(vita::Select), "SELECT"));
        CHECK(!std::strcmp(vita::button_name(0), "NONE"));
        CHECK(!std::strcmp(kRallyLayout.name(ix(Control::HandBrake)), "HAND BRAKE"));
    }
    { // Sega Rally: the hand brake, one view button, no View 2-4
        vita::Controls r;
        r.set_layout(kRallyLayout);
        r.set_bindings(vita::Bindings::defaults(kRallyLayout));
        CHECK(r.sample({}).handbrake == 0);
        CHECK(r.sample({vita::Cross}).handbrake == 0xff);
        in = r.sample({vita::Triangle});
        CHECK((in.in0 & 0x20) == 0 && (in.in0 & 0xc0) == 0xc0);
        in = r.sample({vita::Circle | vita::Square});             // nothing bound there
        CHECK(in.in0 == 0xff && (in.in1 & 1) && in.handbrake == 0);
        CHECK(r.sample({0, 0, 128}).steer == 0x20);              // the wheel on the left stick
        r.sample({}); r.sample({vita::Up}); CHECK(r.gear() == 2); // the gearbox
    }
    { // Virtua Fighter 2: a joystick and three buttons
        vita::Controls f;
        f.set_layout(kFighterLayout);
        f.set_bindings(vita::Bindings::defaults(kFighterLayout));
        auto i = f.sample({});
        CHECK(i.steer == 0x80 && i.accel == 0x20 && i.brake == 0x20 && i.in0 == 0xff);
        CHECK(f.sample({vita::Left}).steer == 0x20 && f.sample({vita::Right}).steer == 0xe0);
        CHECK(f.sample({vita::Up}).accel == 0xe0 && f.sample({vita::Down}).brake == 0xe0);
        CHECK(f.sample({vita::Up | vita::Down}).accel == 0x20); // opposite directions cancel
        CHECK(f.gear() == 1);                                     // up/down do not shift
        CHECK(f.sample({0, 0, 128, 128}).steer == 0x20);          // left stick
        CHECK(f.sample({0, 128, 128, 0}).accel == 0xe0);          // left stick up
        CHECK(f.sample({0, 128, 0, 128}).accel == 0x20);          // not the right stick
        i = f.sample({vita::Cross | vita::Circle | vita::Square});
        CHECK((i.in0 & 0xe0) == 0);                               // punch, kick, guard
        CHECK((f.sample({vita::R | vita::L | vita::Triangle}).in0) == 0xff); // nothing bound there
        f.sample({vita::Select}); CHECK((f.sample({}).in0 & 0x01) == 0); // coin on release
        CHECK((f.sample({vita::Start}).in0 & 0x10) == 0);
        CHECK(!std::strcmp(kFighterLayout.name(ix(Control::Punch)), "PUNCH"));
    }
    vita::FrameClock clock(16000000.0 / (656.0 * 424.0));
    int frames = 0;
    for (int i = 0; i < 6000; ++i) frames += clock.advance(1.0 / 60.0);
    CHECK(frames == 5752);
    // Shipping GXM loop permits only one simulation step per presentation.
    // Fractional carry must still avoid an accidental half-refresh cap.
    vita::FrameClock presentation_clock(16000000.0 / (656.0 * 424.0), 1);
    int presented_frames = 0;
    for (int i = 0; i < 6000; ++i) {
        const int steps = presentation_clock.advance(1.0 / 60.0);
        CHECK(steps >= 0 && steps <= 1);
        presented_frames += steps;
    }
    CHECK(presented_frames == 5752);
    CHECK(clock.advance(20.0) <= 4);
    clock.reset(); CHECK(clock.advance(0) == 0);
    CHECK(clock.advance(-1) == 0);
    CHECK(clock.advance(std::numeric_limits<double>::infinity()) == 0);
    CHECK(clock.advance(std::numeric_limits<double>::quiet_NaN()) == 0);
    {   // The light gun (vcop's range): a touch on the picture aims there and fires;
        // on a black margin it points off the screen (the range's left edge) and
        // fires; without a touch the last aim on the picture stays, unfired.
        vita::Controls gun;
        gun.set_layout(kGunLayout);
        gun.set_bindings(vita::Bindings::defaults(kGunLayout));
        gun.set_gun(0x083, 0x276, 0x024, 0x1a9);
        vita::Pad centre;
        centre.touch = true, centre.tx = 480.f, centre.ty = 272.f; // the screen's centre: the picture's
        vita::Input in = gun.sample(centre);
        CHECK(!(in.in1 & 0x01));
        CHECK(in.gun_x == uint16_t(0x083 + 248.f * float(0x276 - 0x083) / 496.f + 0.5f));
        CHECK(in.gun_y == uint16_t(0x024 + 192.f * float(0x1a9 - 0x024) / 384.f + 0.5f));
        const uint16_t aim_x = in.gun_x, aim_y = in.gun_y;
        vita::Pad margin;
        margin.touch = true, margin.tx = 40.f, margin.ty = 272.f; // left of the picture (it starts at ~128.6)
        in = gun.sample(margin);
        CHECK(!(in.in1 & 0x01) && in.gun_x == 0x083);
        margin.tx = 940.f; // right of it
        in = gun.sample(margin);
        CHECK(!(in.in1 & 0x01) && in.gun_x == 0x083);
        in = gun.sample(vita::Pad{});
        CHECK((in.in1 & 0x01) && in.gun_x == aim_x && in.gun_y == aim_y);
        vita::Pad cross;
        cross.buttons = vita::Cross;
        in = gun.sample(cross);
        CHECK(!(in.in1 & 0x01) && in.gun_x == aim_x);
        // The picture's corners map to the range's ends.
        const rt::GunAim lo = rt::aim_gun(0.f, 0.f, 0x083, 0x276, 0x024, 0x1a9);
        CHECK(lo.on_screen && lo.x == 0x083 && lo.y == 0x024);
        CHECK(!rt::aim_gun(496.f, 10.f, 0x083, 0x276, 0x024, 0x1a9).on_screen);
    }
    std::puts("Vita input and timing tests passed (all 256 axis values, 4096 button combinations, 100 seconds of pacing, the light gun)");
}
