// vitaGL renderer: the ImGui menu's textures and triangles (render/imgui_backend.h).

#include "render/gl_internal.h"

namespace vita {
using namespace gldraw;

// ---- ImGui menu (render/imgui_backend.h) -----------------------------------------------------------
uint32_t gl_create_ui_texture(int w, int h, const void *rgba) {
    if (w <= 0 || h <= 0 || !rgba) return 0;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return texture;
}

void gl_delete_ui_texture(uint32_t texture) {
    if (!texture) return;
    glFinish(); // the GPU may still draw the last menu frame with it
    GLuint t = texture;
    glDeleteTextures(1, &t);
}

void gl_ui_triangles(uint32_t texture, const GlUiVertex *vertices, size_t count, int x0, int y0, int x1, int y1) {
    count -= count % 3;
    if (!texture || !count) return;
    x0 = std::clamp(x0, 0, int(kDisplayW)); x1 = std::clamp(x1, 0, int(kDisplayW));
    y0 = std::clamp(y0, 0, int(kDisplayH)); y1 = std::clamp(y1, 0, int(kDisplayH));
    if (x1 <= x0 || y1 <= y0) return;
    Vertex *dst = g.verts.append(count);
    if (!dst) { g.stats.dropped_vertices += unsigned(count); return; }
    const uint32_t first = uint32_t(dst - g.verts.data);
    for (size_t i = 0; i < count; ++i) {
        const GlUiVertex &v = vertices[i];
        dst[i] = {v.x, v.y, 1.0f, v.u, v.v, 0.0f, 0.0f, v.rgba};
    }
    const int clip = add_clip({x0, y0, x1, y1});
    if (!g.cmds.empty()) {
        Cmd &last = g.cmds.back();
        if (!last.quads && last.prog == Prog::Ui && last.texture == texture && last.clip == clip &&
            last.first + last.count == first) {
            last.count += uint32_t(count);
            return;
        }
    }
    g.cmds.push_back({Prog::Ui, texture, first, uint32_t(count), Depth::Off, clip});
}

} // namespace vita
