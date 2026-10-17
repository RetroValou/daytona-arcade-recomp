// vitaGL renderer: vitaGL set-up, the frame level API and gl.log (render/renderer.h).

#include "render/gl_internal.h"

#include <psp2/power.h>

#include <cstdarg>
#include <cstdio>
#include <string>

// Waiting for the GPU (gl_begin_frame) uses vitaGL's internal 'gxm_context' symbol. Set to
// 0 if your libvitaGL does not export it (link error): glFinish() is used instead.
#ifndef DAYTONA_GL_GPU_TIMING
#define DAYTONA_GL_GPU_TIMING 1
#endif
#if DAYTONA_GL_GPU_TIMING
extern "C" SceGxmContext *gxm_context; // libvitaGL, gxm.c
#endif

namespace vita {
namespace gldraw {

State g;

void gl_log(const char *fmt, ...) {
    if (!kGlLog) return;
    std::FILE *f = std::fopen(kLogPath, "a");
    if (!f) return;
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fclose(f);
}

// Blocks until the GPU has finished every frame submitted so far.
void wait_gpu() {
#if DAYTONA_GL_GPU_TIMING
    sceGxmFinish(gxm_context);
#else
    glFinish();
#endif
}

void set_error(const char *what, const char *log) {
    std::snprintf(g.error, sizeof g.error, "%s%s%s", what, log && *log ? ": " : "", log ? log : "");
    gl_log("ERROR %s\n", g.error);
}

namespace {
double ms(uint64_t us, unsigned n = 1) { return n ? double(us) / 1000.0 / n : 0.0; }

// Text built in memory and written with one gl_log(): opening ux0: once per line made the
// report itself take ~100 ms.
struct Report {
    std::string text;
    void add(const char *fmt, ...) {
        char line[320];
        std::va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof line, fmt, args);
        va_end(args);
        text += line;
    }
};

// One report per kLogEvery frames: where the frame time goes, and what the renderer did.
// All times are averages per presented frame, in ms.
void log_report() {
    const FrameStats &s = g.stats;
    const Timing &t = g.timing;
    const unsigned n = t.frames, l = t.loops;
    const double frame = ms(t.interval, n);
    const double board = ms(t.board, l), sound_wait = ms(t.sound_wait, l), prepare = ms(t.prepare, l);
    const double record = ms(t.record, n), submit = ms(t.submit, n), swap = ms(t.swap, n);
    const double gpu_wait = ms(t.gpu_wait, n);
    const double other = frame - board - sound_wait - prepare - record - gpu_wait - submit - swap;
    Report r;
    r.add("---- frame %u: %u frames (%u in menu), CPU %d MHz, GPU %d MHz ----\n", g.frame, n, t.menu_loops,
           scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency());
    r.add("  FRAME     %6.2f ms = %.1f fps shown, %.1f fps emulated (%.2f board frames per shown frame), worst %.2f ms\n",
           frame, frame > 0.0 ? 1000.0 / frame : 0.0, t.interval ? t.board_frames * 1e6 / double(t.interval) : 0.0,
           l ? double(t.board_frames) / l : 0.0, ms(t.interval_max));
    r.add("  emulation %6.2f ms  (i960+TGP core %.2f, geometry %.2f, 2D video %.2f; worst board frame %.2f)\n", board,
           ms(t.board_core, l), ms(t.board_geometry, l), ms(t.board_video, l), ms(t.board_max));
    r.add("  sound     %6.2f ms  waiting for the sound thread (it runs %.2f ms per frame in parallel)\n", sound_wait,
           ms(t.sound_worker, l));
    r.add("  renderer  %6.2f ms  CPU: 2D worker wait %.2f, 2D quads %.2f, polygons %.2f (texture builds %.2f) + prepare %.2f\n",
           record + prepare, ms(t.rec_upload, n), ms(t.rec_layers, n), ms(t.rec_polygons, n), ms(t.rec_textures, n),
           prepare);
    r.add("  GPU wait  %6.2f ms  at frame start, before rewriting vertices and textures in place\n", gpu_wait);
    r.add("  GL submit %6.2f ms\n", submit);
    r.add("  swap      %6.2f ms  waiting in vglSwapBuffers (GPU still busy with an older frame, or vsync)\n", swap);
    r.add("  other     %6.2f ms  input, menu, SDL_Delay, logs, everything not measured above\n", other);
    r.add("frame %u: polys in=%u out=%u skipped: vcount=%u window=%u clip=%u invalid=%u texture=%u palette=%u renderer=%u\n",
           g.frame, s.polys_in, s.polys_out, s.skip_vcount, s.skip_window, s.skip_clip, s.skip_invalid, s.skip_texture,
           s.skip_palette, s.skip_renderer);
    r.add("frame %u: batches=%u cmds=%u draw_calls=%u verts=%u (dropped %u) | new textures=%u (deferred %u)"
          " new palettes=%u | free vram %u KB\n",
          g.frame, s.batches, unsigned(g.cmds.size()), s.draw_calls, unsigned(g.verts.size),
          s.dropped_vertices, s.new_sources, s.deferred_sources, s.new_palettes, vglMemFree(VGL_MEM_VRAM) / 1024);
    gl_log("%s", r.text.c_str());
}
} // namespace

} // namespace gldraw

using namespace gldraw;

// ---- Frame level API ----------------------------------------------------------------
const char *gl_error() { return g.error; }

bool gl_init() {
    g.error[0] = 0;
    if (kGlLog) std::remove(kLogPath);
    gl_log("gl_init: runtime shader compiler\n");
    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, GL_TRUE, GL_TRUE, GL_TRUE);
    // sceGxm buffer sizes, before vglInit*.
    constexpr uint32_t kParamBuffer = 16u * 1024u * 1024u;
    vglSetVDMBufferSize(2048u * 1024u);
    vglSetVertexBufferSize(8192u * 1024u);
    vglSetFragmentBufferSize(2048u * 1024u);
    vglSetUSSEBufferSize(128u * 1024u);
    vglSetParamBufferSize(kParamBuffer);
    const GLboolean fallback = vglInitExtended(0, int(kDisplayW), int(kDisplayH), kParamBuffer, SCE_GXM_MULTISAMPLE_NONE);
    gl_log("gl_init: vglInitExtended returned %d (1 = resolution fallback), free vram %u KB\n", int(fallback),
           vglMemFree(VGL_MEM_VRAM) / 1024);
    vglWaitVblankStart(GL_TRUE);

    glViewport(0, 0, int(kDisplayW), int(kDisplayH));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(0.0f);        // lowest priority = 0 (larger depth = in front)
    glDepthFunc(GL_GEQUAL);     // nearer = larger; on ties the later polygon wins
    glDepthRangef(-1.0f, 1.0f); // window depth = z / w as computed by the vertex shader
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);   // enabled per command (polygons only)
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (int i = 0; i < int(Prog::Count); ++i) {
        g.program[i] = build_program(kPrograms[i]);
        gl_log("gl_init: program %s = %u\n", kPrograms[i].name, g.program[i]);
    }

    if (!g.program[int(Prog::Flat)] || !g.program[int(Prog::Layer)] || !g.program[int(Prog::Model)] ||
        !g.program[int(Prog::ModelAlpha)] || !g.program[int(Prog::Ui)])
        return false;
    // Checker programs are optional: without them checker polygons are drawn solid.
    if (!g.program[int(Prog::FlatChecker)] || !g.program[int(Prog::ModelChecker)]) {
        gl_log("gl_init: checker shaders unavailable, checker polygons drawn solid\n");
        g.error[0] = 0;
        if (g.program[int(Prog::FlatChecker)]) glDeleteProgram(g.program[int(Prog::FlatChecker)]);
        if (g.program[int(Prog::ModelChecker)]) glDeleteProgram(g.program[int(Prog::ModelChecker)]);
        g.program[int(Prog::FlatChecker)] = g.program[int(Prog::Flat)];
        g.program[int(Prog::ModelChecker)] = g.program[int(Prog::ModelAlpha)];
    }
    glGenBuffers(1, &g.vbo);
    if (!g.vbo || !g.verts.grow(size_t(1) << 16)) {
        set_error("vertex memory allocation failed");
        return false;
    }
    g.cmds.reserve(1024);
    {
        // Static index buffer of the indexed quads: {4q, 4q+1, 4q+2, 4q, 4q+2, 4q+3}.
        std::vector<uint16_t> indices(kQuadIndices);
        for (size_t q = 0; q < kIndexedVertexLimit / 4; ++q) {
            const uint16_t v = uint16_t(q * 4);
            uint16_t *i = &indices[q * 6];
            i[0] = v; i[1] = uint16_t(v + 1); i[2] = uint16_t(v + 2);
            i[3] = v; i[4] = uint16_t(v + 2); i[5] = uint16_t(v + 3);
        }
        glGenBuffers(1, &g.quad_ibo);
        if (g.quad_ibo) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(uint16_t)), indices.data(),
                         GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        gl_log("gl_init: indexed quads %s\n", g.quad_ibo ? "on" : "unavailable, plain triangles");
    }
    g.ready = true;
    gl_log("gl_init: done\n");
    return true;
}

void gl_begin_frame() {
    ++g.frame;
    // The GPU may still read last frame's vertices and textures, rewritten in place below.
    const uint64_t wait_begin = now_us();
    wait_gpu();
    g.timing.gpu_wait += now_us() - wait_begin;
    g.verts.size = 0;
    g.cmds.clear();
    g.clips.clear();
    g.clip = -1;
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // glClear honours the mask
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gl_end_frame() {
    Timing &t = g.timing;
    uint64_t diagnostic = 0; // gl.log write, reported apart (gl_last_diagnostic_us)
    const uint64_t t0 = now_us();
    execute();
    const uint64_t t1 = now_us();
    vglSwapBuffers(GL_FALSE);
    const uint64_t t2 = now_us();
    const uint64_t end = now_us();

    const bool exclude = g.skip_worst; // includes the log write
    g.skip_worst = false;
    if (g.last_end) {
        const uint64_t interval = end - g.last_end;
        ++t.frames;
        t.interval += interval;
        if (!exclude) t.interval_max = std::max(t.interval_max, interval);
        t.record += g.record_us;
        t.submit += t1 - t0;
        t.swap += t2 - t1;
    }
    g.last_end = end;
    g.record_us = 0;
    if (kGlLog && (g.frame <= 3 || g.frame % kLogEvery == 0)) {
        const uint64_t l0 = now_us();
        log_report();
        diagnostic += now_us() - l0;
        t = {};
        g.skip_worst = true;
    }
    g.diagnostic_us = diagnostic;
    g.stats = {};
}

uint64_t gl_last_diagnostic_us() { return g.diagnostic_us; }

void gl_profile_loop(const GlLoopProfile &p) {
    Timing &t = g.timing;
    ++t.loops;
    if (p.menu) ++t.menu_loops;
    t.board_frames += p.board_frames;
    t.board += p.board;
    t.board_core += p.board_core;
    t.board_geometry += p.board_geometry;
    t.board_video += p.board_video;
    t.board_max = std::max(t.board_max, p.board_frames ? p.board / p.board_frames : 0);
    t.sound_wait += p.sound_wait;
    t.sound_worker += p.sound_worker;
    t.prepare += p.prepare;
}

void gl_fini() {
    if (!g.ready) return;
    glFinish();
    for (int i = 0; i < int(Prog::Count); ++i) {
        bool shared = false; // checker fallbacks reuse other handles
        for (int j = 0; j < i; ++j) shared = shared || g.program[j] == g.program[i];
        if (g.program[i] && !shared) glDeleteProgram(g.program[i]);
        g.program[i] = 0;
    }
    if (g.vbo) {
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        vglBufferData(GL_ARRAY_BUFFER, nullptr); // our memory: vitaGL must not free it
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &g.vbo);
    }
    g.vbo = 0;
    if (g.quad_ibo) glDeleteBuffers(1, &g.quad_ibo);
    g.quad_ibo = 0;
    g.verts.release();
    g.ready = false;
}

void gl_fill_rect(float x, float y, float w, float h, uint32_t rgba) {
    push_quad(Prog::Flat, 0, x, y, x + w, y + h, 0.0f, 0.0f, 0.0f, 0.0f, rgba);
}

} // namespace vita
