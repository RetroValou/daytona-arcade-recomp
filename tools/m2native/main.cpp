// m2native: the M1 lockstep harness for recompiled code. Same checks as
// m2replay (every device access, every sample's RAM hashes and registers,
// every interrupt against MAME), but the i960 code is the native C++ that
// m2recomp generated. There is no fallback: control reaching an address with
// no recompiled code is a hard error naming it.
//
//   m2native IMAGES_DIR TRACE.m2tr IRQ.log
//
// Model 2B: the same with the recompiled SHARC (TgpBoard runs it); M2NATIVE_M1
// set: M1 instead, the copro, buffer RAM and the geometrizer answered from the
// trace like every other device.

#include "games/game.h"
#include "runtime/cpu/i960_gen_support.h"
#include "runtime/cpu/lockstep.h"
#include "runtime/cpu/replay_bus.h"
#include "runtime/tgp/tgp_board.h"
#include "geocheck.h"

#include <memory>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: m2native IMAGES_DIR TRACE.m2tr IRQ.log [GEO.log]\n");
        return 2;
    }
    const std::string dir = argv[1];
    uint64_t done = 0;
    if (gen::inline_board >= 0) { // gen::BoardMem needs an rt::M2Board: no replay bus
        std::fprintf(stderr, "m2native: the i960 code was generated with m2recomp --board (recompile.py --inline-bus): "
                             "the lockstep needs the default output\n");
        return 2;
    }
    try {
        rt::M2ReplayBus bus(load(dir + "/program.bin"), load(dir + "/main_data.bin"), argv[2]);
        rt::Cpu core(&bus);
        bus.attach(&core);
        // M2: the geometry ports, the copro (TGP or SHARC) and buffer RAM are
        // native, not replayed.
        const bool m1 = std::getenv("M2NATIVE_M1") != nullptr;
        rt::TgpBoard board(load(dir + "/copro_tables.bin"), load(dir + "/copro_data.bin"));
        rt::TgpBoardModel geo(board);
        if (!m1) bus.set_model(&geo);
        rt::Lockstep ls(core, argv[3]);
        core.reset();
        gen::Env env{core, ls};
        // M2: the geometrizer, checked at each vblank against MAME's GEO.log.
        std::unique_ptr<rt::Geo> geom;
        std::unique_ptr<GeoCheck> geocheck;
        if (argc == 5 && !m1) {
            geom = std::make_unique<rt::Geo>(load(dir + "/polygons.bin"), load(dir + "/textures.bin"), board.buffer_data());
            geocheck = std::make_unique<GeoCheck>(argv[4], *geom, board, bus, ls);
        }

        const auto t0 = std::chrono::steady_clock::now();
        while (!ls.finished() && !bus.trace_done()) {
            if (!gen::has_code(core.m_IP)) {
                char b[128];
                std::snprintf(b, sizeof b, "no recompiled code at %08x (instruction %" PRIu64 "): add it to the seeds",
                              core.m_IP, ls.count);
                throw rt::Fatal(b);
            }
            gen::run(env);
            done = ls.count;
        }
        done = ls.count;
        if (!bus.all_events_consumed())
            throw rt::Divergence("MAME's run ended but the trace still has events we did not make; " + bus.where());
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2native: MATCH. %" PRIu64 " instructions, all native (%" PRIu64 " recompiled), %" PRIu64
                    " epochs, %" PRIu64 " device events, %d interrupts, identical to MAME; %.2f s (%.1f M instr/s)\n",
                    ls.count, gen::native_instructions(), bus.epochs_matched(), bus.events_matched(), ls.interrupts(), s,
                    double(ls.count) / s / 1e6);
        if (m1) {
            std::printf("  copro, buffer RAM and geometrizer replayed from the trace (M1)\n");
            return 0;
        }
        if (board.sharc_instructions())
            std::printf("  SHARC (recompiled, run on demand): %" PRIu64 " instructions\n", board.sharc_instructions());
        std::printf("  TGP (recompiled, run on demand): %" PRIu64 " instructions, %" PRIu64
                    " output words identical to MAME\n"
                    "  measured (depends on relative timing): buffer RAM reads %" PRIu64 " differ of %" PRIu64
                    "; FIFO-status polls %" PRIu64 " differ of %" PRIu64 "; buffer RAM hash differs at %" PRIu64 " of %" PRIu64 " samples\n",
                    board.tgp_instructions(), geo.fifo_words, geo.buffer_mismatch, geo.buffer_reads, geo.status_mismatch,
                    geo.status_reads, geo.region_mismatch, geo.region_samples);
        if (geocheck)
            std::printf("  geometrizer (native): %" PRIu64 " frames, %" PRIu64 " rasterizer words and %" PRIu64
                        " polygons identical to MAME\n",
                        geocheck->frames_checked, geocheck->words, geocheck->polys);
        if (geocheck) {
            std::printf("  3D layer (CPU reference rasterizer): %" PRIu64 " of %" PRIu64 " frames identical to MAME%s%s\n",
                        geocheck->fb_checked - geocheck->fb_mismatch, geocheck->fb_checked,
                        geocheck->fb_mismatch ? "; first difference " : "", geocheck->first_fb_mismatch.c_str());
            std::printf("  screen (tilemaps + 3D, composed): %" PRIu64 " of %" PRIu64 " frames identical to MAME%s%s\n",
                        geocheck->scr_checked - geocheck->scr_mismatch, geocheck->scr_checked,
                        geocheck->scr_mismatch ? "; first difference " : "", geocheck->first_scr_mismatch.c_str());
        }
        if (geo.buffer_mismatch) std::printf("  first buffer RAM difference: %s\n", geo.first_buffer_mismatch.c_str());
        if (geo.status_mismatch) std::printf("  first FIFO-status difference: %s\n", geo.first_status_mismatch.c_str());
        return 0;
    } catch (const rt::Divergence &d) {
        std::printf("m2native: DIVERGED after %" PRIu64 " instructions: %s\n", done, d.what());
        return 1;
    } catch (const rt::GeoFatal &f) {
        std::printf("m2native: GEOMETRIZER FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const rt::TgpFatal &f) {
        std::printf("m2native: TGP FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const rt::Fatal &f) {
        std::printf("m2native: FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2native: %s\n", e.what());
        return 2;
    }
}
