// vitaGL renderer: the shader source and its program variants.

#include "render/gl_internal.h"
#include "runtime/board/game.h"
#include <algorithm>
#include <cstdio>

namespace vita::gldraw {

// ---- Shaders -------------------------------------------------------------------------
// One source for every program; #defines select the variant (see kPrograms).
//   TEXTURED    sample s_tex (unit 0)
//   MODEL       s_tex holds Model 2 indices; colour from s_palette (unit 1), row v_uvp.z
//   ALPHA_TEST  discard transparent texels
//   CHECKER     Model 2 checker: discard one native pixel out of two
//   PALETTE24   s_tex holds System 24 pen numbers (column r, row g); colour from s_pal24 (unit 2)
//   (TEXTURED alone: texture * vertex colour, the ImGui menu, blended)
const char *const kVertexShader = R"(
attribute vec3 a_pos;    // x * w, y * w (display pixels, top-left origin), w
attribute vec4 a_uvpd;   // texture u, v (1.0 = one texture width), palette row coordinate, depth
attribute vec4 a_color;
varying vec3 v_uvp;
varying vec4 v_color;
#ifdef CHECKER
varying vec3 v_pos;
#endif
void main() {
    float w = a_pos.z;
#ifdef upgrade_z_depth
    // The hybrid depth (the Vita option POLYGON ORDER). Model 2 polygons only,
    // coded by the CPU as 1 + their window's band + their rank inside it (in
    // (0, 1), larger in front; model2_polygons.cpp); the 2D layers sit at
    // exactly 0 (background) or 0.99999994 (HUD, w 1) and keep it. Each window
    // keeps its band of the range, as the Model 2 draws a window over the ones
    // before it (its UI stays in front); inside the band:
    //  - macro order: the rank, weighted update_z_depth_RANK_WEIGHT;
    //  - local slope: the vertex's nearness from its distance w on a log scale,
    //    weighted update_z_depth_LOCAL_WEIGHT: nearer (smaller w) in front,
    //    coplanar polygons (same w) still ordered by rank.
    // update_z_depth_LOCAL_WEIGHT: the one setting (0: the Model 2 order alone,
    // larger: closer to a true z-buffer), the game's
    // (rt::Game::update_z_depth_local_weight, defined by build_program); the
    // rank gets the rest, 0.9999 - update_z_depth_LOCAL_WEIGHT. The result stays
    // inside its band, below the HUD and above the background.
    const float update_z_depth_RANK_WEIGHT = 0.9999 - update_z_depth_LOCAL_WEIGHT;
    float hybrid_depth = a_uvpd.w;
    if (hybrid_depth >= 1.0) {
        float band = floor(hybrid_depth);
        float rank = hybrid_depth - band;
        // 1 at w 1, 0 at the game's farthest distance (update_z_depth_SCALE: its log)
        float nearness = clamp(1.0 - log(max(w, 1.0)) * (1.0 / update_z_depth_SCALE), 0.0, 1.0);
        float local = clamp(rank * update_z_depth_RANK_WEIGHT + nearness * update_z_depth_LOCAL_WEIGHT, 0.001, 0.999);
        hybrid_depth = (band - 1.0 + local) * (0.9999 / update_z_depth_BANDS);
    }
    gl_Position = vec4(a_pos.x * (2.0 / SCREEN_W) - w, w - a_pos.y * (2.0 / SCREEN_H), hybrid_depth * w, w);
#else
    gl_Position = vec4(a_pos.x * (2.0 / SCREEN_W) - w, w - a_pos.y * (2.0 / SCREEN_H), a_uvpd.w * w, w);
#endif
    v_uvp = a_uvpd.xyz;
    v_color = a_color;
#ifdef CHECKER
    v_pos = a_pos;
#endif
}
)";

const char *const kFragmentShader = R"(
varying vec3 v_uvp;
varying vec4 v_color;
#ifdef CHECKER
varying vec3 v_pos;
#endif
#ifdef TEXTURED
uniform sampler2D s_tex;
#endif
#ifdef MODEL
uniform sampler2D s_palette;
#endif
#ifdef PALETTE24
uniform sampler2D s_pal24;
#endif
void main() {
#ifdef CHECKER
    vec2 p = floor(v_pos.xy / v_pos.z);
    float s = p.x + p.y;
    if (s - 2.0 * floor(s * 0.5) < 0.5) { discard; }
#endif
#ifdef TEXTURED
    vec4 t = texture2D(s_tex, v_uvp.xy);
#ifdef ALPHA_TEST
    if (t.a < 0.5) { discard; }
#endif
#ifdef MODEL
    float k = floor(t.r * 127.5 + 0.01);
    gl_FragColor = texture2D(s_palette, vec2((k + 0.5) * (1.0 / 128.0), v_uvp.z)) * v_color;
#else
#ifdef PALETTE24
    float k = floor(t.r * 127.5 + 0.01);
    float j = floor(t.g * 63.75 + 0.01);
    gl_FragColor = texture2D(s_pal24, vec2((k + 0.5) * (1.0 / 128.0), (j + 0.5) * (1.0 / 64.0))) * v_color;
#else
    gl_FragColor = t * v_color;
#endif
#endif
#else
    gl_FragColor = v_color;
#endif
}
)";

const ProgramDef kPrograms[int(Prog::Count)] = {
    {"Flat", ""},
    {"Layer", "#define TEXTURED\n#define ALPHA_TEST\n#define PALETTE24\n"},
    {"Model", "#define TEXTURED\n#define MODEL\n"},
    {"ModelAlpha", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n"},
    {"FlatChecker", "#define CHECKER\n"},
    {"ModelChecker", "#define TEXTURED\n#define MODEL\n#define ALPHA_TEST\n#define CHECKER\n"},
    {"Ui", "#define TEXTURED\n"}, // ImGui menu (render/imgui_backend.h): RGBA texture * vertex colour, blended
};

namespace {
GLuint compile(GLenum type, const char *common, const char *defines, const char *body, const char *name) {
    const GLuint shader = glCreateShader(type);
    const char *sources[] = {common, defines, body};
    glShaderSource(shader, 3, sources, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[400] = {};
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    set_error(name, log);
    glDeleteShader(shader);
    return 0;
}
} // namespace

GLuint build_program(const ProgramDef &def, bool upgrade_z_depth) {
    // Constants shared by every shader.
    // update_z_depth_LOCAL_WEIGHT: the game's hybrid depth setting (rt::Game::update_z_depth_local_weight),
    // kept inside [0, 0.9999] so the rank's weight is never negative.
    const double local_weight = std::clamp(double(rt::game().update_z_depth_local_weight), 0.0, 0.9999);
    // update_z_depth_SCALE: the game's distance scale (rt::Game::update_z_depth_scale), above 0.
    const double scale = std::max(double(rt::game().update_z_depth_scale), 0.1);
    char common[384];
    std::snprintf(common, sizeof common,
                  "#version 140\nprecision lowp int;\nprecision highp float;\n"
                  "%s" // the hybrid depth variant (the Vita option, gl_set_upgrade_z_depth)
                  "#define update_z_depth_LOCAL_WEIGHT %.4f\n"
                  "#define update_z_depth_SCALE %.4f\n"
                  "#define update_z_depth_BANDS %.1f\n" // kZDepthBands: one per Model 2 window
                  "#define SCREEN_W %.1f\n#define SCREEN_H %.1f\n",
                  upgrade_z_depth ? "#define upgrade_z_depth\n" : "", local_weight, scale, double(kZDepthBands), double(kDisplayW), double(kDisplayH));
    const GLuint vs = compile(GL_VERTEX_SHADER, common, def.defines, kVertexShader, def.name);
    const GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, common, def.defines, kFragmentShader, def.name) : 0;
    GLuint program = 0;
    if (vs && fs) {
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "a_pos");
        glBindAttribLocation(program, 1, "a_uvpd");
        glBindAttribLocation(program, 2, "a_color");
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok == GL_TRUE) {
            glUseProgram(program);
            if (GLint loc = glGetUniformLocation(program, "s_tex"); loc >= 0) glUniform1i(loc, 0);
            if (GLint loc = glGetUniformLocation(program, "s_palette"); loc >= 0) glUniform1i(loc, 1);
            if (GLint loc = glGetUniformLocation(program, "s_pal24"); loc >= 0) glUniform1i(loc, 2);
            glUseProgram(0);
        } else {
            char log[400] = {};
            glGetProgramInfoLog(program, sizeof log, nullptr, log);
            set_error(def.name, log);
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return program;
}

} // namespace vita::gldraw
