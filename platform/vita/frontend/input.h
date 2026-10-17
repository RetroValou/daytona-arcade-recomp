#pragma once

// The Vita's buttons and sticks as the frontend's portable pad (frontend/controls.h),
// and the controls' result as the board's inputs.

#include "frontend/controls.h"
#include "runtime/board/game.h"

namespace vita {

Pad read_pad(); // this instant's buttons and sticks (sceCtrl)

inline rt::Inputs map_input(const Input &in) {
    rt::Inputs out;
    out.steer = in.steer; out.accel = in.accel; out.brake = in.brake;
    out.in0 = in.in0; out.in1 = in.in1; out.in2 = in.in2;
    out.handbrake = in.handbrake;
    return out;
}

// The game's controls (rt::Game::control_list): what the Controls page lists and binds.
inline Layout game_layout() { return {rt::game().control_list.data(), int(rt::game().control_list.size())}; }

} // namespace vita
