#include "frontend/input.h"

#include <psp2/ctrl.h>

namespace vita {

Pad read_pad() {
    SceCtrlData native{};
    Pad pad;
    if (sceCtrlPeekBufferPositive(0, &native, 1) <= 0) return pad;
    pad.lx = native.lx; pad.ry = native.ry; pad.ly = native.ly;
    const struct { uint32_t native, portable; } map[] = {
        {SCE_CTRL_CROSS, Cross}, {SCE_CTRL_CIRCLE, Circle},
        {SCE_CTRL_SQUARE, Square}, {SCE_CTRL_TRIANGLE, Triangle},
        {SCE_CTRL_UP, Up}, {SCE_CTRL_DOWN, Down},
        {SCE_CTRL_LEFT, Left}, {SCE_CTRL_RIGHT, Right},
        {SCE_CTRL_LTRIGGER, L}, {SCE_CTRL_RTRIGGER, R},
        {SCE_CTRL_START, Start}, {SCE_CTRL_SELECT, Select}
    };
    for (auto e : map) if (native.buttons & e.native) pad.buttons |= e.portable;
    return pad;
}

} // namespace vita
