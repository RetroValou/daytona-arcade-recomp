#include "frontend/input.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

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
    // The front touch screen, in screen pixels: the panel's coordinates are
    // not the screen's (about twice, not exactly), so they are scaled from the
    // panel's own display corners (sceTouchGetPanelInfo); 1920x1088 if unknown.
    static const SceTouchPanelInfo panel = [] {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        SceTouchPanelInfo info{};
        if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &info) < 0 || info.maxDispX <= info.minDispX || info.maxDispY <= info.minDispY)
            info.minDispX = 0, info.minDispY = 0, info.maxDispX = 1919, info.maxDispY = 1087;
        return info;
    }();
    SceTouchData touch{};
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum) {
        pad.touch = true;
        pad.tx = float(touch.report[0].x - panel.minDispX) * 960.0f / float(panel.maxDispX - panel.minDispX + 1);
        pad.ty = float(touch.report[0].y - panel.minDispY) * 544.0f / float(panel.maxDispY - panel.minDispY + 1);
    }
    return pad;
}

} // namespace vita
