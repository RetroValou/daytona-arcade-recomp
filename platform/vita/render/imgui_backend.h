#pragma once
#include "imgui.h"
#include <psp2/touch.h>
// The menu through vitaGL (render/renderer.h): its triangles join the frame's draw list, blended.
#include "render/renderer.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>

namespace vita {
// Menu-only ImGui renderer on the existing GXM context. No SDL video device,
// extra display buffers or game shader changes are needed.
class ImGuiVita {
    uint32_t atlas_ = 0;              // GLuint
    std::vector<GlUiVertex> vertices_; // one command's triangles, reused
public:
    bool init() {
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendRendererName = "daytona_vitagl";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        ImFontConfig font; font.SizePixels = 20;
        io.Fonts->AddFontDefault(&font);
        unsigned char *pixels; int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        atlas_ = gl_create_ui_texture(w, h, pixels);
        if (!atlas_) { ImGui::DestroyContext(); return false; }
        io.Fonts->SetTexID(ImTextureID(uintptr_t(atlas_)));
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.FramePadding = {10, 7}; style.ItemSpacing = {10, 8};
        style.ScrollbarSize = 24;
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
        return true;
    }
    void frame(float dt) {
        auto &io = ImGui::GetIO(); io.DisplaySize = {960, 544};
        io.DeltaTime = std::clamp(dt, .001f, .1f);
        SceTouchData touch{};
        if (sceTouchPeek( SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum) {
            io.AddMousePosEvent(touch.report[0].x * .5f, touch.report[0].y * .5f);
            io.AddMouseButtonEvent(0, true);
        } else io.AddMouseButtonEvent(0, false);
        ImGui::NewFrame();
    }
    // Between gl_begin_frame() and gl_end_frame(), like the game image.
    void render() {
        ImGui::Render();
        const auto *data = ImGui::GetDrawData();
        for (const auto *list : data->CmdLists) for (const auto &cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            const int x0 = int(cmd.ClipRect.x), y0 = int(cmd.ClipRect.y);
            const int x1 = int(cmd.ClipRect.z), y1 = int(cmd.ClipRect.w);
            if (x1 <= x0 || y1 <= y0 || !cmd.ElemCount) continue;
            vertices_.resize(cmd.ElemCount);
            for (unsigned i = 0; i < cmd.ElemCount; ++i) {
                const auto &in = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i]];
                vertices_[i] = {in.pos.x, in.pos.y, in.uv.x, in.uv.y, in.col}; // IM_COL32: bytes r, g, b, a
            }
            gl_ui_triangles(uint32_t(uintptr_t(cmd.GetTexID())), vertices_.data(), vertices_.size(), x0, y0, x1, y1);
        }
    }
    void shutdown() {
        gl_delete_ui_texture(atlas_);
        atlas_ = 0; ImGui::DestroyContext();
    }
};
} // namespace vita
