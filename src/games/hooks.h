// Every game's hooks into its recompiled code. seeds/<set>_hooks.txt lists
// "ADDRESS name" lines (m2recomp --hooks); just before the instruction at a
// hook's address the generated code calls rt::hook_<name>(c). Each hook is
// defined in its game's file in src/games/ and does nothing while the option
// it serves (runtime/enhance.h) is off, so the game runs exactly as
// recompiled (rules.md: parity runs that way).
#pragma once

#include "runtime/cpu.h"

namespace rt {

// Daytona USA (daytona.cpp), 0x17078 (daytona93; 0x17508 in Revision A, the
// same code and RAM), after the game has built its draw list of course
// cells: the count at 0x5016c0 and the cell numbers from 0x5016c1, chosen
// from the 5x5 cells around the car's (r8) in a grid 16 cells wide
// (cell = x + 16 y). Enhance::draw_distance.
void hook_draw_list(Cpu &c);

} // namespace rt
