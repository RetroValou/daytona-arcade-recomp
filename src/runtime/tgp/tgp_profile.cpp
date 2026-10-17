// TGP profiling (M2_TGP_PROFILE): counters, host micro-benchmark and report.
// See tgp_profile.h.
#include "runtime/tgp/tgp_profile.h"

#ifdef M2_TGP_PROFILE

#include "runtime/tgp/tgp_board.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>

namespace rt {

TgpProfile tgp_profile;

namespace {

volatile uint32_t g_sink; // keeps the benchmark loops

struct Writer {
    char *out;
    size_t size, used = 0;
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void add(const char *format, ...) {
        if (!out || used + 1 >= size) return;
        va_list args;
        va_start(args, format);
        const int n = std::vsnprintf(out + used, size - used, format, args);
        va_end(args);
        if (n > 0) used = std::min(size - 1, used + size_t(n));
    }
};

const char *const kOpName[kTgpOps] = {"fifo-in", "fifo-out", "bank-w", "io-r",  "io-w",    "data-r",   "data-w", "prog-r", "fp-add",
                                      "fp-mul",  "fp-div",   "fp>int", "int>fp", "int-alu", "branch", "computed", "rep"};

} // namespace

TgpBench tgp_microbench(TgpBoard &board) {
    TgpBench b;
    uint64_t (*const clock)() = tgp_profile.clock;
    if (!clock) return b;
    const double tps = double(tgp_profile.ticks_per_second);
    auto ns = [&](uint64_t ticks, uint64_t n) { return double(ticks) * 1e9 / tps / double(n); };
    constexpr uint32_t N = 1u << 20;
    uint32_t in[1024];
    for (uint32_t i = 0; i < 1024; i++) in[i] = Tgp::f2u(float(int(i * 37u % 2001u) - 1000) * 0.37f); // -370..370
    uint32_t acc = g_sink;

    { // one clock read
        constexpr uint32_t n = 4096;
        uint64_t x = 0;
        const uint64_t t0 = clock();
        for (uint32_t i = 0; i < n; i++) x += clock();
        b.clock_ns = ns(clock() - t0, n);
        acc ^= uint32_t(x);
    }
    auto loop = [&](auto &&body) {
        const uint64_t t0 = clock();
        for (uint32_t i = 0; i < N; i++) body(i);
        return ns(clock() - t0, N);
    };
    b.loop_ns = loop([&](uint32_t i) { acc ^= in[i & 1023]; });
    auto net = [&](double v) { return std::max(0.0, v - b.loop_ns); };
    { // ALU 0x09 as the TGP chains it: d = d + p, p = a * b with a = d (a bounded rotation)
        uint32_t d = Tgp::f2u(1.0f) ^ (acc & 1), p = Tgp::f2u(0.5f);
        const uint32_t m = Tgp::f2u(-1.0f);
        b.fp_muladd_ns = net(loop([&](uint32_t i) {
            const uint32_t r1 = Tgp::f2u(Tgp::u2f(d) + Tgp::u2f(p));
            const uint32_t r2 = Tgp::f2u(Tgp::u2f(d) * Tgp::u2f(m));
            d = r1 ^ (in[i & 1023] & 0); // (the same loop shape as the others)
            p = r2;
        }));
        acc ^= d ^ p;
    }
    { // ALU 0x10
        uint32_t d = Tgp::f2u(1.25f) ^ (acc & 1);
        const float k = 1.5f;
        b.fp_div_ns = net(loop([&](uint32_t i) { d = Tgp::f2u(k / Tgp::u2f(d)) ^ (in[i & 1023] & 0); }));
        acc ^= d;
    }
    // ALU 0x0f, mode 0
    b.fp_round_ns = net(loop([&](uint32_t i) { acc ^= Tgp::to_s32(::roundf(Tgp::u2f(in[i & 1023]))); }));
    { // the sin/cos table port (out of line, tgp.cpp)
        Tgp t;
        t.tables = board.table_data();
        b.table_r_ns = net(loop([&](uint32_t i) {
            t.sincos_base = in[i & 1023];
            acc ^= t.io_r(uint16_t(0x20 + (i & 3)));
        }));
    }
    TgpBus &bus = board; // a virtual call, as from the TGP
    b.bank_rom_ns = net(loop([&](uint32_t i) { acc ^= bus.mem_r(0x800000u | (i & 0x1fffff)); }));
    b.bank_buffer_ns = net(loop([&](uint32_t i) { acc ^= bus.mem_r(0x400000u | (i & 0x7fff)); }));
    {
#ifdef M2_FAST_TGP
        WordFifo f;
#else
        std::deque<uint32_t> f;
#endif
        b.fifo_ns = net(loop([&](uint32_t i) {
            f.push_back(in[i & 1023]);
            acc ^= f.front();
            f.pop_front();
        }));
    }
    g_sink = acc;
    b.done = true;
    return b;
}

void TgpProfileReport::start(uint64_t tgp_instructions) {
    at_ = tgp_profile;
    instructions_ = tgp_instructions;
    dispatches_ = tgpgen::dispatches;
    hits_.assign(tgpgen::block_hits, tgpgen::block_hits + tgpgen::block_count);
}

double TgpProfileReport::ms_per_frame(uint64_t frames, const TgpBench &bench) const {
    const TgpProfile &now = tgp_profile;
    if (!frames) return 0.0;
    const double tps = double(now.ticks_per_second ? now.ticks_per_second : 1);
    // Each timed run includes about one clock read: removed.
    const double raw_ms = double(now.ticks - at_.ticks) * 1000.0 / tps;
    const double bias_ms = double(now.runs - at_.runs) * bench.clock_ns * 1e-6;
    return std::max(0.0, raw_ms - bias_ms) / double(frames);
}

size_t TgpProfileReport::format(char *out, size_t size, uint64_t frames, uint64_t tgp_instructions, int cpu_mhz,
                                double whole_ms, const char *whole, const TgpBench &bench) const {
    Writer w{out, size};
    const TgpProfile &now = tgp_profile;
    if (hits_.size() != tgpgen::block_count || !frames) {
        w.add("TGP: no window\n");
        return w.used;
    }
    const double f = double(frames);
    const uint64_t runs = now.runs - at_.runs, empty = now.empty_runs - at_.empty_runs;
    const uint64_t instr = tgp_instructions - instructions_;
    const double bias_ms = double(runs) * bench.clock_ns * 1e-6;
    const double tgp_ms = ms_per_frame(frames, bench);
    const double ns_instr = instr ? tgp_ms * f * 1e6 / double(instr) : 0.0;

    w.add("---- TGP (M2_TGP_PROFILE) ----\n");
    if (whole)
        w.add("TGP time: %.3f ms/frame = %.1f%% of %s (%.3f ms/frame)\n", tgp_ms,
              whole_ms > 0 ? 100.0 * tgp_ms / whole_ms : 0.0, whole, whole_ms);
    else
        w.add("TGP time: %.3f ms/frame\n", tgp_ms);
    w.add("TGP runs: %.1f/frame (%.1f found no input), %.0f instr/frame, %.1f instr/run, %.2f ns/instr",
          double(runs) / f, double(empty) / f, double(instr) / f, runs > empty ? double(instr) / double(runs - empty) : 0.0,
          ns_instr);
    if (cpu_mhz > 0) w.add(" = %.1f cycles/instr at %d MHz", ns_instr * double(cpu_mhz) / 1000.0, cpu_mhz);
    w.add("\nTGP timing: clock read %.3f us, removed %.3f ms/frame; the 2 reads per run add ~%.3f ms/frame to the time around the TGP\n",
          bench.clock_ns / 1000.0, bias_ms / f, double(runs) * 2.0 * bench.clock_ns * 1e-6 / f);

    // Work: executions x contents of every block.
    double op[kTgpOps] = {};
    uint64_t counted = 0;
    for (uint32_t i = 0; i < tgpgen::block_count; i++) {
        const uint64_t h = tgpgen::block_hits[i] - hits_[i];
        if (!h) continue;
        counted += h * tgpgen::blocks[i].length;
        for (int k = 0; k < kTgpOps; k++) op[k] += double(h) * tgpgen::blocks[i].ops[k];
    }
    auto pf = [&](double v) { return v / f; };
    auto cf = [&](uint64_t a, uint64_t b) { return double(a - b) / f; };
    w.add("TGP work/frame: fifo in %.0f out %.0f | tables read: sin/cos %.0f atan %.0f 1/x %.0f 1/sqrt %.0f, written %.0f, unmapped %.0f\n",
          pf(op[TgpOpFifoIn]), pf(op[TgpOpFifoOut]), cf(now.table_r[0], at_.table_r[0]), cf(now.table_r[1], at_.table_r[1]),
          cf(now.table_r[2], at_.table_r[2]), cf(now.table_r[3], at_.table_r[3]), cf(now.table_w, at_.table_w),
          cf(now.io_unmapped, at_.io_unmapped));
    w.add("TGP work/frame: banked reads ROM %.0f buffer RAM %.0f other %.0f, writes %.0f, bank switches %.0f | data RAM r %.0f w %.0f | prog r %.0f\n",
          cf(now.bank_r_rom, at_.bank_r_rom), cf(now.bank_r_buffer, at_.bank_r_buffer), cf(now.bank_r_none, at_.bank_r_none),
          cf(now.bank_w, at_.bank_w), pf(op[TgpOpBankW]), pf(op[TgpOpDataR]), pf(op[TgpOpDataW]), pf(op[TgpOpProgR]));
    w.add("TGP ALU/frame: fp add %.0f mul %.0f div %.0f fp>int %.0f int>fp %.0f int %.0f | branches %.0f computed %.0f, dispatches %.0f, rep %.0f\n",
          pf(op[TgpOpFpAdd]), pf(op[TgpOpFpMul]), pf(op[TgpOpFpDiv]), pf(op[TgpOpFpToInt]), pf(op[TgpOpIntToFp]),
          pf(op[TgpOpIntAlu]), pf(op[TgpOpBranch]), pf(op[TgpOpComputed]), cf(tgpgen::dispatches, dispatches_), pf(op[TgpOpRep]));
    if (counted != instr)
        w.add("TGP blocks: %.0f instr/frame counted by blocks (a stall ends a block early: the instruction runs again)\n",
              double(counted) / f);

    if (bench.done) {
        w.add("TGP unit cost ns (host micro-benchmark, warm caches): clock %.1f loop %.1f | fp add+mul %.1f div %.1f round %.1f | table %.1f | bank ROM %.1f buffer %.1f | fifo %.1f\n",
              bench.clock_ns, bench.loop_ns, bench.fp_muladd_ns, bench.fp_div_ns, bench.fp_round_ns, bench.table_r_ns,
              bench.bank_rom_ns, bench.bank_buffer_ns, bench.fifo_ns);
        const double tables = cf(now.table_r[0] + now.table_r[1] + now.table_r[2] + now.table_r[3],
                                 at_.table_r[0] + at_.table_r[1] + at_.table_r[2] + at_.table_r[3]);
        const double e_fp = (pf(op[TgpOpFpAdd]) + pf(op[TgpOpFpMul])) * bench.fp_muladd_ns / 2 * 1e-6;
        const double e_div = pf(op[TgpOpFpDiv]) * bench.fp_div_ns * 1e-6;
        const double e_round = pf(op[TgpOpFpToInt]) * bench.fp_round_ns * 1e-6;
        const double e_table = tables * bench.table_r_ns * 1e-6;
        const double e_bank = (cf(now.bank_r_rom, at_.bank_r_rom) * bench.bank_rom_ns +
                               (cf(now.bank_r_buffer, at_.bank_r_buffer) + cf(now.bank_w, at_.bank_w)) * bench.bank_buffer_ns) * 1e-6;
        const double e_fifo = (pf(op[TgpOpFifoIn]) + pf(op[TgpOpFifoOut])) * bench.fifo_ns / 2 * 1e-6;
        const double sum = e_fp + e_div + e_round + e_table + e_bank + e_fifo;
        w.add("TGP estimate ms/frame: fp add/mul %.3f div %.3f fp>int %.3f tables %.3f banked %.3f fifo %.3f = %.3f; "
              "rest %.3f (%.0f%%: dispatch, branches, data RAM, loads/stores of the TGP's registers)\n",
              e_fp, e_div, e_round, e_table, e_bank, e_fifo, sum, std::max(0.0, tgp_ms - sum),
              tgp_ms > 0 ? 100.0 * std::max(0.0, tgp_ms - sum) / tgp_ms : 0.0);
    }

    // The hottest blocks by instructions.
    order_.resize(tgpgen::block_count);
    for (uint32_t i = 0; i < tgpgen::block_count; i++) order_[i] = i;
    auto weight = [&](uint32_t i) { return (tgpgen::block_hits[i] - hits_[i]) * tgpgen::blocks[i].length; };
    const size_t top = std::min<size_t>(10, order_.size());
    std::partial_sort(order_.begin(), order_.begin() + std::ptrdiff_t(top), order_.end(),
                      [&](uint32_t a, uint32_t b) { return weight(a) > weight(b); });
    w.add("TGP hottest blocks (instructions):\n");
    for (size_t r = 0; r < top; r++) {
        const uint32_t i = order_[r];
        const uint64_t wt = weight(i);
        if (!wt) break;
        const TgpBlockInfo &bl = tgpgen::blocks[i];
        w.add("  %03x-%03x len %2u: %5.1f%% of instr, %8.1f runs/frame |", bl.start, bl.start + bl.length - 1, bl.length,
              counted ? 100.0 * double(wt) / double(counted) : 0.0, double(tgpgen::block_hits[i] - hits_[i]) / f);
        for (int k = 0; k < kTgpOps; k++)
            if (bl.ops[k]) w.add(" %s %u", kOpName[k], bl.ops[k]);
        w.add("\n");
    }
    return w.used;
}

} // namespace rt

#endif // M2_TGP_PROFILE
