// m2sharccheck: lockstep check of the SHARC (Model 2B's copro) against MAME.
//
//   m2sharccheck IMAGES_DIR SHARC.log [SHARC.pc]
//
// SHARC.log is MAME's SHARC-side event log (M2TRACE_SHARCLOG, patch 0005): the
// i960's IOP writes, the program upload, every word the SHARC popped from its
// input FIFO and pushed to its output FIFO and its buffer RAM reads and writes,
// in order. The SHARC (src/runtime/sharc, MAME's interpreter lifted) boots
// from sharc_program.bin through the same IOP writes and upload, then runs
// with one cursor on the log: each input FIFO read gets MAME's next "in" word,
// each buffer RAM read MAME's value (buffer RAM is shared with the i960 and
// the geometrizer), and every write must be MAME's next event, bit for bit.
// FLAG0 (input FIFO empty) is clear exactly when MAME's next event is an input
// read: the SHARC runs on demand, as the board will run it. SHARC.pc
// (M2TRACE_SHARCPC) adds a per-instruction check of PC, ASTAT (but its copy
// of the flag inputs: mask_flags), MODE1 and R0-R15; a run at one address is one line on both sides (how many times the
// SHARC polls depends on timing).

#include "runtime/sharc/sharc.h"

// M2SHARC_GEN (m2sharccheck_gen): the recompiled program (m2sharcrecomp)
// instead of the interpreter.

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

struct Divergence : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Ev {
    enum Kind { Up, In, InEmpty, Out, Br, Bw, Iop } kind;
    uint32_t a = 0, b = 0;
};

const char *kind_name(Ev::Kind k) {
    static const char *const names[] = {"up", "in", "ie", "out", "br", "bw", "iop"};
    return names[k];
}

std::vector<Ev> read_log(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<Ev> v;
    std::string line, k;
    while (std::getline(f, line)) {
        std::istringstream s(line);
        Ev e{};
        s >> k >> std::hex >> e.a >> e.b;
        if (k == "up") e.kind = Ev::Up;
        else if (k == "in") e.kind = Ev::In;
        else if (k == "ie") e.kind = Ev::InEmpty;
        else if (k == "out") e.kind = Ev::Out;
        else if (k == "br") e.kind = Ev::Br;
        else if (k == "bw") e.kind = Ev::Bw;
        else if (k == "iop") e.kind = Ev::Iop;
        else continue;
        if (e.kind == Ev::In || e.kind == Ev::Out) e.b = e.a, e.a = 0;
        v.push_back(e);
    }
    return v;
}

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%" PRIx64, v);
    return b;
}

// The board's side of the SHARC's data space (MAME model2b copro_sharc_map):
// 0x400000-0xbfffff the input FIFO, 0xc00000-0x13fffff the output FIFO,
// 0x1400000-0x1bfffff buffer RAM (word offset & 0x7fff), 0x1c00000-0x1dfffff
// the copro data ROM.
struct LogBus : rt::sharc::Bus {
    const std::vector<Ev> &log;
    const std::vector<uint32_t> &rom;
    size_t pos = 0;
    uint64_t fifo_in = 0, fifo_out = 0, buffer_reads = 0, buffer_writes = 0;
    rt::sharc::adsp21062_device *sharc = nullptr;
    // FLAG0 (input FIFO empty): clear exactly when MAME's next event is an input.
    void update_flag() {
        if (sharc) sharc->set_flag_input(0, next_is_input() ? 0 : 1);
    }
    LogBus(const std::vector<Ev> &l, const std::vector<uint32_t> &r) : log(l), rom(r) {}

    const Ev &expect(Ev::Kind k, uint32_t a, uint32_t b, bool check_b) {
        if (pos >= log.size())
            throw Divergence(std::string("the SHARC made ") + kind_name(k) + " " + hex(a) + " " + hex(b) + " after MAME's last event");
        const Ev &e = log[pos];
        if (e.kind != k || e.a != a || (check_b && e.b != b))
            throw Divergence(std::string("event ") + std::to_string(pos) + ": MAME " + kind_name(e.kind) + " " + hex(e.a) + " " +
                             hex(e.b) + ", ours " + kind_name(k) + " " + hex(a) + (check_b ? " " + hex(b) : std::string()));
        ++pos;
        update_flag();
        return e;
    }
    bool next_is_input() const { return pos < log.size() && log[pos].kind == Ev::In; }

    uint32_t read(uint32_t address) override {
        if (address >= 0x400000 && address < 0xc00000) {
            if (pos < log.size() && log[pos].kind == Ev::InEmpty) { ++pos; update_flag(); return 0; }
            ++fifo_in;
            return expect(Ev::In, 0, 0, false).b;
        }
        if (address >= 0x1400000 && address < 0x1c00000) {
            ++buffer_reads;
            return expect(Ev::Br, address & 0x7fff, 0, false).b;
        }
        if (address >= 0x1c00000 && address < 0x1e00000) return rom[(address - 0x1c00000) & (rom.size() - 1)];
        return 0;
    }
    void write(uint32_t address, uint32_t data) override {
        if (address >= 0xc00000 && address < 0x1400000) { ++fifo_out; expect(Ev::Out, 0, data, true); return; }
        if (address >= 0x1400000 && address < 0x1c00000) { ++buffer_writes; expect(Ev::Bw, address & 0x7fff, data, true); return; }
    }
};

// MAME's per-instruction log, folded: one line per run at one address.
struct PcLog {
    std::ifstream f;
    std::string line;
    explicit PcLog(const std::string &path) : f(path) {
        if (!f) throw std::runtime_error("cannot open " + path);
    }
    bool next(std::string &out) {
        while (std::getline(f, line))
            if (line.compare(0, 4, "rep ") != 0) { out = line; return true; }
        return false;
    }
};

// MAME's line with its ASTAT's FLG0-3 bits replaced by ours. Those bits are a
// copy of the flag inputs (FLAG0: input FIFO empty) taken when the program
// rewrites ASTAT, so they depend on when the i960 filled the FIFO; while the
// flags are inputs, reads of ASTAT see the live flags instead (GET_UREG).
std::string mask_flags(const std::string &theirs, uint32_t our_astat) {
    std::istringstream in(theirs);
    std::string pc, op, astat;
    in >> pc >> op >> astat;
    const uint32_t a = (uint32_t(std::stoul(astat, nullptr, 16)) & ~0x780000u) | (our_astat & 0x780000u);
    std::string rest;
    std::getline(in, rest);
    char b[32];
    std::snprintf(b, sizeof b, "%x", a);
    return pc + " " + op + " " + b + rest;
}

std::string our_line(const rt::sharc::adsp21062_device &s) {
    char b[256];
    int n = std::snprintf(b, sizeof b, "%x %012" PRIx64 " %x %x", s.pc(), s.opcode(), s.astat(), s.mode1());
    for (int i = 0; i < 16; ++i) n += std::snprintf(b + n, sizeof b - size_t(n), " %x", s.r(i));
    return b;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: m2sharccheck IMAGES_DIR SHARC.log [SHARC.pc]\n");
        return 2;
    }
    uint64_t steps = 0;
    try {
        const std::string dir = argv[1];
        const std::vector<uint8_t> program = load(dir + "/sharc_program.bin");
        const std::vector<uint8_t> data = load(dir + "/copro_data.bin");
        std::vector<uint32_t> rom(data.size() / 4);
        std::memcpy(rom.data(), data.data(), rom.size() * 4);
        const std::vector<Ev> log = read_log(argv[2]);
        LogBus bus(log, rom);
        rt::sharc::adsp21062_device sharc(bus);
        std::unique_ptr<PcLog> pcs = argc == 4 ? std::make_unique<PcLog>(argv[3]) : nullptr;

        // The boot: the i960's IOP writes, then the upload (MAME copro_fifo_w in
        // boot mode: external_dma_write(count, word)), then the SHARC runs.
        while (bus.pos < log.size() && log[bus.pos].kind == Ev::Iop) {
            sharc.external_iop_write(log[bus.pos].a, log[bus.pos].b);
            ++bus.pos;
        }
        if (bus.pos >= log.size() || log[bus.pos].kind != Ev::Up) throw Divergence("no program upload after the IOP writes");
        const Ev &up = log[bus.pos++];
        if (up.a * 2 != program.size())
            throw Divergence("MAME uploaded " + std::to_string(up.a) + " words, sharc_program.bin has " + std::to_string(program.size() / 2));
        for (uint32_t i = 0; i < up.a; ++i) sharc.external_dma_write(i, program[i * 2] | program[i * 2 + 1] << 8);
        sharc.set_halted(false);
        bus.sharc = &sharc;
        bus.update_flag();

        const auto t0 = std::chrono::steady_clock::now();
        uint64_t idle_steps = 0;
        double sharc_seconds = 0; // M2SHARC_GEN: in the recompiled program's calls
        struct Tracer {
            PcLog *pcs;
            uint32_t last_pc = ~0u;
            uint64_t lines = 0;
            std::string theirs;
        } tr{pcs.get(), ~0u, 0, {}};
        if (pcs)
            sharc.set_trace([](void *ctx, const rt::sharc::adsp21062_device &s) {
                Tracer &t = *static_cast<Tracer *>(ctx);
                if (s.pc() == t.last_pc) return;
                t.last_pc = s.pc();
                if (!t.pcs->next(t.theirs)) throw Divergence("MAME's instruction log ends before ours");
                ++t.lines;
                const std::string ours = our_line(s);
                if (ours != t.theirs && ours != mask_flags(t.theirs, s.astat()))
                    throw Divergence("instruction line " + std::to_string(t.lines) + ":\n  MAME " + t.theirs + "\n  ours " + ours);
            }, &tr);
        while (bus.pos < log.size()) {
            // IOP writes after the boot reach the SHARC between its instructions.
            while (bus.pos < log.size() && log[bus.pos].kind == Ev::Iop) {
                sharc.external_iop_write(log[bus.pos].a, log[bus.pos].b);
                ++bus.pos;
            }
            const size_t before = bus.pos;
            const uint64_t steps_before = steps;
#ifdef M2SHARC_GEN
            // The recompiled program: until it waits or a million instructions.
            const uint64_t at = sharc.instructions();
            const auto r0 = std::chrono::steady_clock::now();
            const bool waiting = sharc.run_generated(1'000'000);
            sharc_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count();
            steps += sharc.instructions() - at;
            if (waiting && bus.pos == before && !bus.next_is_input() && bus.pos < log.size() && log[bus.pos].kind != Ev::Iop)
                throw Divergence(std::string("the SHARC waits for input; MAME's next event is ") + kind_name(log[bus.pos].kind));
#else
            sharc.step();
            ++steps;
#endif
            idle_steps = bus.pos == before ? idle_steps + (steps - steps_before) + 1 : 0;
            if (idle_steps > 50'000'000) throw Divergence("the SHARC made no event in 50 M instructions (next MAME event " +
                                                          std::string(kind_name(log[bus.pos].kind)) + ")");
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2sharccheck: MATCH. %zu events, %" PRIu64 " FIFO words in, %" PRIu64 " out, %" PRIu64 " buffer RAM reads, %" PRIu64
                    " writes, identical to MAME; %" PRIu64 " SHARC instructions%s%s; %.2f s\n",
                    log.size(), bus.fifo_in, bus.fifo_out, bus.buffer_reads, bus.buffer_writes, steps,
                    pcs ? ", instruction log identical over " : "", pcs ? (std::to_string(tr.lines) + " lines").c_str() : "", s);
        if (sharc_seconds > 0)
            std::printf("  in the recompiled program: %.2f s, %.1f M SHARC instructions/s (with the log's bus)\n", sharc_seconds,
                        double(steps) / sharc_seconds / 1e6);
        return 0;
    } catch (const Divergence &d) {
        std::printf("m2sharccheck: DIVERGED after %" PRIu64 " SHARC instructions: %s\n", steps, d.what());
        return 1;
    } catch (const rt::sharc::SharcFatal &f) {
        std::printf("m2sharccheck: SHARC FATAL after %" PRIu64 " instructions: %s\n", steps, f.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2sharccheck: %s\n", e.what());
        return 2;
    }
}
