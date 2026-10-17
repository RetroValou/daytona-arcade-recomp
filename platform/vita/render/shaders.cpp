// vitaGL renderer: the shader source and its program variants.

#include "render/gl_internal.h"
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
    gl_Position = vec4(a_pos.x * (2.0 / SCREEN_W) - w, w - a_pos.y * (2.0 / SCREEN_H), a_uvpd.w * w, w);
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

GLuint build_program(const ProgramDef &def) {
    // Constants shared by every shader.
    char common[320];
    std::snprintf(common, sizeof common,
                  "#version 140\nprecision lowp int;\nprecision highp float;\n"
                  "#define SCREEN_W %.1f\n#define SCREEN_H %.1f\n",
                  double(kDisplayW), double(kDisplayH));
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
