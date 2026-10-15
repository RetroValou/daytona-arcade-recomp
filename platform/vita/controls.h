// Vita input mapping, independent of SDL and VitaSDK so it can be host-tested.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../../src/runtime/cabinet_controls.h"

namespace vita {
enum Button : uint32_t {
    Cross = 1u << 0, Circle = 1u << 1, Square = 1u << 2, Triangle = 1u << 3,
    Up = 1u << 4, Down = 1u << 5, Left = 1u << 6, Right = 1u << 7,
    L = 1u << 8, R = 1u << 9, Start = 1u << 10, Select = 1u << 11
};
struct Pad { uint32_t buttons = 0; uint8_t lx = 128, ry = 128, ly = 128; }; // ly: a joystick's up/down
struct Input {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20;
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;
    uint8_t handbrake = 0; // rt::Inputs::handbrake (0xff: pulled)
};
inline bool menu_chord(uint32_t buttons) {
    return (buttons & (Start | Select)) == (Start | Select);
}
inline float axis(uint8_t value, float deadzone = 0.12f) {
    deadzone = std::clamp(deadzone, 0.0f, 0.4f);
    const float x = value < 128 ? (int(value) - 128) / 128.f : (int(value) - 128) / 127.f;
    const float magnitude = std::abs(x);
    if (magnitude <= deadzone) return 0.f;
    return std::copysign((magnitude - deadzone) / (1.f - deadzone), x);
}

// The game's controls (rt::Game::control_list; src/runtime/cabinet_controls.h), each
// bound to one button and listed on the Controls page; the indices are rt::Control's
// (the first twelve in the order vita.cfg has always saved them). The analog sticks
// and the system chords (Start+Select menu, Select+Triangle test, Select+Square
// service) stay fixed: the left stick steers (or is the joystick), the right stick
// works the pedals, when the game has them.
using rt::Control;
inline constexpr int ControlCount = rt::kControlCount;
inline constexpr uint32_t kButtonMask = 0xfffu;
// rt::PadButton is in Button's bit order.
inline uint32_t pad_bit(rt::PadButton button) { return 1u << unsigned(button); }

struct Layout {
    const rt::ControlDef *defs = nullptr;
    int count = 0;
    const rt::ControlDef *find(Control c) const {
        for (int i = 0; i < count; ++i)
            if (defs[i].control == c) return &defs[i];
        return nullptr;
    }
    bool has(Control c) const { return find(c) != nullptr; }
    const char *name(int control) const {
        const rt::ControlDef *d = control >= 0 && control < ControlCount ? find(Control(control)) : nullptr;
        return d ? d->name : "?";
    }
};

inline bool single_button(uint32_t button) {
    return button && !(button & ~kButtonMask) && !(button & (button - 1));
}
inline const char *button_name(uint32_t button) {
    static const char *names[] = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "UP", "DOWN",
        "LEFT", "RIGHT", "L", "R", "START", "SELECT"};
    if (!single_button(button)) return "NONE";
    int bit = 0;
    while (!(button & (1u << bit))) ++bit;
    return names[bit];
}
struct Bindings {
    // One button per control the game uses; 0 for the others.
    uint32_t button[ControlCount] = {};
    // The game's defaults (each control's PadButton).
    static Bindings defaults(const Layout &layout) {
        Bindings b;
        for (int i = 0; i < layout.count; ++i) b.button[int(layout.defs[i].control)] = pad_bit(layout.defs[i].button);
        return b;
    }
    // Gives `control` the button `value`; a control that had it takes the old one
    // instead, so no control the game uses is ever left without a button.
    void bind(int control, uint32_t value) {
        if (control < 0 || control >= ControlCount || !button[control] || !single_button(value)) return;
        for (uint32_t &other : button)
            if (other == value) other = button[control];
        button[control] = value;
    }
    // Next/previous button in the fixed order above (Left/Right on the Controls page).
    void cycle(int control, int direction) {
        if (control < 0 || control >= ControlCount || !button[control]) return;
        int bit = 0;
        while (bit < 11 && !(button[control] & (1u << bit))) ++bit;
        bind(control, 1u << ((bit + (direction < 0 ? 11 : 1)) % 12));
    }
    // Saved files may be edited by hand, or written for another layout: any control
    // the game uses without a valid, unshared button resets all to the defaults; the
    // others are cleared.
    void sanitize(const Layout &layout) {
        uint32_t seen = 0;
        for (int c = 0; c < ControlCount; ++c) {
            if (!layout.has(Control(c))) { button[c] = 0; continue; }
            if (!single_button(button[c]) || (seen & button[c])) { *this = defaults(layout); return; }
            seen |= button[c];
        }
    }
};

class Controls {
public:
    // The game's controls: only these are read, with their bindings.
    void set_layout(const Layout &layout) {
        layout_ = layout;
        steering_ = layout.has(Control::SteerLeft) || layout.has(Control::SteerRight);
        pedals_ = layout.has(Control::Accelerate) || layout.has(Control::Brake);
        joystick_ = layout.has(Control::Up) || layout.has(Control::Down) || layout.has(Control::Left) ||
                    layout.has(Control::Right);
        bindings_.sanitize(layout_);
    }
    const Layout &layout() const { return layout_; }
    bool steering() const { return steering_; }

    Input sample(Pad pad) {
        Input in;
        const auto &b = bindings_.button;
        auto held = [&](Control c) { return (pad.buttons & b[int(c)]) != 0; }; // 0: not used
        const uint32_t previous = held_;
        const uint32_t pressed = pad.buttons & ~held_;
        if (pressed & b[int(Control::Coin)]) coin_used_ = false;
        held_ = pad.buttons;
        // The menu chord must not inject a coin or start a race.
        if (menu_chord(pad.buttons)) { coin_used_ = true; set_gear(in); return in; }
        // Cabinet controls consume the modifier and view buttons.
        if (pad.buttons & Select) {
            const bool test = (pad.buttons & Triangle) != 0;
            const bool service = (pad.buttons & Square) != 0;
            if (test || service) {
                coin_used_ = true;
                if (test) in.in0 &= uint8_t(~0x04);
                if (service) in.in0 &= uint8_t(~0x08);
                set_gear(in);
                return in;
            }
        }
        float steer = 0.f, accel = 0.f, brake = 0.f;
        if (steering_) {
            steer = axis(pad.lx, deadzone_);
            if (steer_curve_ == 1) steer *= std::abs(steer);
            else if (steer_curve_ == 2) steer = steer * steer * steer;
            if (held(Control::SteerLeft) || held(Control::SteerRight))
                steer = float(held(Control::SteerRight)) - float(held(Control::SteerLeft));
            if (steer_invert_) steer = -steer;
        }
        if (pedals_) {
            const float pedal = axis(pad.ry, deadzone_);
            accel = held(Control::Accelerate) ? 1.f : std::max(-pedal, 0.f);
            brake = held(Control::Brake) ? 1.f : std::max(pedal, 0.f);
        }
        in.steer = uint8_t(std::lround(128.f + 96.f * steer));
        in.accel = uint8_t(std::lround(32.f + 192.f * accel));
        in.brake = uint8_t(std::lround(32.f + 192.f * brake));
        if (joystick_) {
            // The joystick: its directions as the wheel and pedals at their ends
            // (runtime/cabinet_controls.h), from the buttons or the left stick.
            const float x = axis(pad.lx, deadzone_), y = axis(pad.ly, deadzone_);
            const bool left = held(Control::Left) || x < -0.5f, right = held(Control::Right) || x > 0.5f;
            const bool up = held(Control::Up) || y < -0.5f, down = held(Control::Down) || y > 0.5f;
            in.steer = left == right ? 0x80 : left ? 0x20 : 0xe0;
            in.accel = up && !down ? 0xe0 : 0x20;
            in.brake = down && !up ? 0xe0 : 0x20;
        }
        // Shift only on edges, including when several board frames are run.
        const int shift = int(bool(pressed & b[int(Control::ShiftUp)])) - int(bool(pressed & b[int(Control::ShiftDown)]));
        gear_ = std::clamp(gear_ + shift, 1, 4);
        set_gear(in);
        // Coin on release leaves time to distinguish Select-based chords.
        if ((previous & b[int(Control::Coin)]) && !(pad.buttons & b[int(Control::Coin)]) && !coin_used_) in.in0 &= uint8_t(~0x01);
        if (held(Control::Start)) in.in0 &= uint8_t(~0x10);
        if (held(Control::View1) || held(Control::Punch)) in.in0 &= uint8_t(~0x20);
        if (held(Control::View2) || held(Control::Kick)) in.in0 &= uint8_t(~0x40);
        if (held(Control::View3) || held(Control::Guard)) in.in0 &= uint8_t(~0x80);
        if (held(Control::View4)) in.in1 &= uint8_t(~0x01);
        if (held(Control::HandBrake)) in.handbrake = 0xff;
        return in;
    }
    void latch(uint32_t buttons) { held_ = buttons; if (buttons & bindings_.button[int(Control::Coin)]) coin_used_ = true; }
    int gear() const { return gear_; }
    void set_deadzone(float value) { deadzone_ = std::clamp(value, 0.0f, 0.4f); }
    void set_steer_curve(int value) { steer_curve_ = std::clamp(value, 0, 2); }
    void set_steer_invert(bool value) { steer_invert_ = value; }
    void set_bindings(const Bindings &value) { bindings_ = value; bindings_.sanitize(layout_); }
    const Bindings &bindings() const { return bindings_; }
private:
    void set_gear(Input &in) const {
        // Same gearbox encoding as src/app/controls.cpp.
        constexpr uint8_t codes[] = {0, 2, 1, 6, 5};
        in.in1 = uint8_t((in.in1 & ~0x70) | (codes[gear_] << 4));
    }
    Layout layout_;
    Bindings bindings_;
    uint32_t held_ = 0;
    int gear_ = 1, steer_curve_ = 0;
    float deadzone_ = 0.12f;
    bool steer_invert_ = false, coin_used_ = false, steering_ = false, pedals_ = false, joystick_ = false;
};

// Host time only; each simulation step still advances at GameLoop::kFrameHz.
class FrameClock {
public:
    explicit FrameClock(double hz, int max_steps = 4)
        : step_(1.0 / hz), max_steps_(std::clamp(max_steps, 1, 4)) {}
    int advance(double seconds) {
        if (!std::isfinite(seconds) || seconds < 0) { reset(); return 0; }
        pending_ = std::min(pending_ + seconds, step_ * 4);
        int count = 0;
        while (pending_ >= step_ && count < max_steps_) { pending_ -= step_; ++count; }
        // Under load, present each completed frame instead of computing up to
        // four complete rasters and displaying only the last. Discard overdue
        // host-time debt, not guest instructions; preserve fractional time so
        // 57.524 Hz on a 60 Hz display does not accidentally become 30 Hz.
        if (pending_ >= step_) pending_ = std::fmod(pending_, step_);
        return count;
    }
    void reset() { pending_ = 0; }
private:
    double step_, pending_ = 0;
    int max_steps_;
};
} // namespace vita
