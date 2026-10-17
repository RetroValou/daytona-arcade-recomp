// vitaGL renderer: the polygon batches and the draw list's GL calls.

#include "render/gl_internal.h"
#include <cmath>

namespace vita::gldraw {

// Set to 1 ONLY if libvitaGL was built with DRAW_SPEEDHACK=1: that build ignores the
// 'first' argument of glDrawArrays. A default vitaGL build needs 0. Not supported with
// the indexed quads (draw_quads after a draw_range would use moved attribute pointers):
// scripts/setup_vitagl.py refuses DRAW_SPEEDHACK=1.
#ifndef DAYTONA_GL_VGL_DRAW_SPEEDHACK
#define DAYTONA_GL_VGL_DRAW_SPEEDHACK 0
#endif

// Moves the batches into the draw list, in order of first appearance. The order does not
// change the image: every polygon has its own priority depth and nothing is blended.
void flush_batches() {
    for (size_t bi = 0; bi < g.batch_count; ++bi) {
        Batch *b = &g.batches[bi];
        g.clip = b->clip;
        b->valid_polys = 0;
        b->valid_vertices = 0;
        if (b->poly_refs.empty()) continue;

        // Indexed quads when they fit under the 16-bit index limit (see kIndexedVertexLimit).
        size_t quads = 0;
        for (const PolyRef &ref : b->poly_refs) quads += size_t(ref.poly->num_vertices - 1) / 2;
        Vertex *quad_dst = append_quads(quads);
        const bool indexed = quad_dst != nullptr;
        const uint32_t first = indexed ? uint32_t(quad_dst - g.verts.data) : uint32_t(g.verts.size);
        uint32_t written = 0;
        for (const PolyRef &ref : b->poly_refs) {
            const rt::GeoPoly &poly = *ref.poly;
            const int n = poly.num_vertices;

            // 1. Vertices in homogeneous form; the GPU does the division by w.
            Vertex v[8];
            bool valid = true;
            for (int i = 0; i < n && valid; ++i) {
                const float w = poly.v[i].p[0];
                v[i] = {ref.base_x * w + poly.v[i].x * kScale, ref.base_y * w - poly.v[i].y * kScale, w,
                        poly.v[i].p[1] * ref.u_scale, poly.v[i].p[2] * ref.v_scale, ref.palette, ref.depth,
                        ref.color};
                valid = w > 0.0f && std::isfinite(w) && std::isfinite(v[i].x) && std::isfinite(v[i].y) &&
                        std::isfinite(v[i].u) && std::isfinite(v[i].v);
            }
            if (!valid) { ++g.stats.skip_invalid; continue; }

            // 2. Written once, straight into the frame's GPU vertex memory (vglForceAlloc:
            //    uncached RAM). The CPU only writes there, never reads back.
            if (indexed) { // quads in the slots reserved above (invalid polygons leave theirs unused)
                written += uint32_t(write_fan_quads(quad_dst + written, v, n));
                ++b->valid_polys;
                continue;
            }
            const size_t count = size_t(n - 2) * 3;
            Vertex *dst = g.verts.append(count);
            if (!dst) { g.stats.dropped_vertices += unsigned(count); continue; }
            for (int i = 1; i + 1 < n; ++i) {
                *dst++ = v[0];
                *dst++ = v[i];
                *dst++ = v[i + 1];
            }
            written += uint32_t(count);
            ++b->valid_polys;
        }
        b->valid_vertices = written;
        // Slots reserved for invalid polygons: give them back (they are at the end).
        if (indexed) g.verts.size = first + written;
        // One command per batch: batches have distinct states, so push() would not merge them.
        if (written) g.cmds.push_back({b->prog, b->texture, first, written, Depth::TestWrite, g.clip, indexed});
    }
    g.clip = -1;
    g.stats.batches = unsigned(g.batch_count);
}

namespace {
// The 3 attributes are interleaved in the VBO. Their pointers stay at the small constant
// offsets 0/12/28 and each draw selects its vertices with glDrawArrays(first, ...).
// Never put a large byte offset in these pointers: vitaGL stores it in the 16-bit
// SceGxmVertexAttribute::offset (wrong vertices beyond 64 KB) and re-patches the vertex
// program for every new offset.
void set_attributes(size_t base) {
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(base + 0));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(base + 12));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<const void *>(base + 28));
}

void draw_range(uint32_t first, uint32_t count) {
    for (uint32_t done = 0; done < count;) {
        const uint32_t n = uint32_t(std::min<size_t>(kMaxVerticesPerDraw, count - done));
#if DAYTONA_GL_VGL_DRAW_SPEEDHACK
        set_attributes(size_t(first + done) * sizeof(Vertex)); // this build adds it to the stream address
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(n));
#else
        glDrawArrays(GL_TRIANGLES, GLint(first + done), GLsizei(n));
#endif
        ++g.stats.draw_calls;
        done += n;
    }
}

// Indexed quads (see kIndexedVertexLimit): count / 4 quads from vertex `first`, both
// multiples of 4, with the static index buffer bound.
void draw_quads(uint32_t first, uint32_t count) {
    const size_t offset = size_t(first / 4) * 6 * sizeof(uint16_t);
    glDrawElements(GL_TRIANGLES, GLsizei(count / 4 * 6), GL_UNSIGNED_SHORT, reinterpret_cast<const void *>(offset));
    ++g.stats.draw_calls;
}
} // namespace

// Runs the recorded draw list.
void execute() {
    if (g.cmds.empty()) return;
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    vglBufferData(GL_ARRAY_BUFFER, g.verts.data); // the VBO uses our GPU memory as is
    if (g.quad_ibo) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.quad_ibo);
    for (GLuint i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    set_attributes(0);
    // Palette textures for the whole frame (unit 1 Model 2, unit 2 System 24); per-command
    // textures on unit 0.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g.palette_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g.s24_palette_texture);
    glActiveTexture(GL_TEXTURE0);

    // Current GL state, to skip redundant calls. -1 / 0 = unknown.
    // gl_begin_frame() left glDepthMask at GL_TRUE.
    GLuint program = 0, texture = 0;
    int blend = -1, depth_test = -1, depth_write = 1, clip = -2;
    for (const Cmd &c : g.cmds) {
        if (g.program[int(c.prog)] != program) {
            program = g.program[int(c.prog)];
            glUseProgram(program);
        }
        // Blending only for the menu / text rectangles; the game image is never blended.
        const int want_blend = ((c.prog == Prog::Flat || c.prog == Prog::Ui) && c.depth == Depth::Off) ? 1 : 0;
        if (want_blend != blend) {
            if (want_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            blend = want_blend;
        }
        const int want_test = c.depth != Depth::Off ? 1 : 0;
        if (want_test != depth_test) {
            if (want_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            depth_test = want_test;
        }
        const int want_write = c.depth != Depth::Test ? 1 : 0; // no effect while the test is off
        if (want_test && want_write != depth_write) {
            glDepthMask(want_write ? GL_TRUE : GL_FALSE);
            depth_write = want_write;
        }
        if (textured(c.prog) && c.texture != texture) {
            glBindTexture(GL_TEXTURE_2D, c.texture);
            texture = c.texture;
        }
        if (c.clip != clip) {
            if (c.clip < 0) {
                glDisable(GL_SCISSOR_TEST);
            } else {
                const std::array<int, 4> &r = g.clips[size_t(c.clip)];
                glEnable(GL_SCISSOR_TEST);
                glScissor(r[0], int(kDisplayH) - r[3], r[2] - r[0], r[3] - r[1]); // GL: bottom-left origin
            }
            clip = c.clip;
        }
        if (c.quads) draw_quads(c.first, c.count);
        else draw_range(c.first, c.count);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    if (depth_write != 1) glDepthMask(GL_TRUE);
    if (g.quad_ibo) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

} // namespace vita::gldraw
