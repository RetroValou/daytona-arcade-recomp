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
enum class Control : uint8_t {
    Accelerate, Brake, SteerLeft, SteerRight, ShiftUp, ShiftDown, Start, Coin,
    View1, View2, View3, View4, HandBrake, Up, Down, Left, Right, Punch, Kick, Guard,
    Count
};
inline constexpr int kControlCount = int(Control::Count);

// A default button, on a generic pad (PlayStation layout: the Vita's buttons).
enum class PadButton : uint8_t { Cross, Circle, Square, Triangle, Up, Down, Left, Right, L, R, Start, Select };

// One of a game's controls: which, its name on the Controls page, its default button.
struct ControlDef {
    Control control;
    const char *name;
    PadButton button;
};

} // namespace rt
