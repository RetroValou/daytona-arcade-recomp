// The cabinet controls every game's record can list (rt::Game::control_list):
// what a frontend's Controls page shows for that game, the button each
// control gets by default, and how it reaches rt::Inputs (which the game's
// wiring, Game::io, turns into its own ports). No dependencies, so the
// frontends' host tests can include it on its own.
#pragma once

#include <cstdint>

namespace rt {

// How each reaches rt::Inputs:
//   Accelerate, Brake    accel, brake (fully pressed; an analog stick can press them part way)
//   SteerLeft/Right      steer at its ends (an analog stick steers in between)
//   ShiftUp/Down         the gear (1-4) in in1 0x70, one step a press
//   Start, Coin          in0 0x10, in0 0x01 (the coin on release)
//   View1-4              in0 0x20, 0x40, 0x80, in1 0x01
//   HandBrake            handbrake fully pulled (0xff)
//   Up/Down/Left/Right   a joystick: accel/brake/steer at their ends (as above)
//   Punch, Kick, Guard   in0 0x20, 0x40, 0x80 (the view buttons' bits: the
//                        fighting games' wiring reads them there)
//   Trigger              in1 0x01 (a light gun's trigger, player 1: Virtua Cop;
//                        the gun's aim is Inputs::gun_x/gun_y, from a pointer)
enum class Control : uint8_t {
    Accelerate, Brake, SteerLeft, SteerRight, ShiftUp, ShiftDown, Start, Coin,
    View1, View2, View3, View4, HandBrake, Up, Down, Left, Right, Punch, Kick, Guard, Trigger,
    Count
};
inline constexpr int kControlCount = int(Control::Count);

// A light gun aimed with a pointer (a touch, the mouse): (x, y) in the game's
// 496x384 picture; the gun's range is the game's (rt::GunRange: MAME's values
// at the screen's edges). Inside the picture the gun points there; outside it
// (the black margins) it points at the range's left edge, off the screen,
// where a shot reloads.
struct GunAim {
    uint16_t x, y;
    bool on_screen;
};
inline GunAim aim_gun(float x, float y, uint16_t x_min, uint16_t x_max, uint16_t y_min, uint16_t y_max) {
    constexpr float kW = 496.0f, kH = 384.0f; // the Model 2's picture (rt::Video::W, H)
    if (!(x >= 0.0f && x < kW && y >= 0.0f && y < kH)) return {x_min, uint16_t((y_min + y_max) / 2), false};
    return {uint16_t(x_min + x * float(x_max - x_min) / kW + 0.5f), uint16_t(y_min + y * float(y_max - y_min) / kH + 0.5f), true};
}

// A default button, on a generic pad (PlayStation layout: the Vita's buttons).
enum class PadButton : uint8_t { Cross, Circle, Square, Triangle, Up, Down, Left, Right, L, R, Start, Select };

// One of a game's controls: which, its name on the Controls page, its default button.
struct ControlDef {
    Control control;
    const char *name;
    PadButton button;
};

} // namespace rt
