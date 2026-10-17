// i960 profiling (M2_I960_PROFILE): counters and report. See i960_profile.h.
#include "runtime/cpu/i960_profile.h"

#ifdef M2_I960_PROFILE

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace rt {

namespace {

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

const char *const kOpName[kI960Ops] = {"ld", "st", "alu", "mul", "div", "cmp", "cond", "b", "call", "ret", "indirect", "fp", "fpmath", "sys"};
const char *const kRegionName[kBusRegions] = {"work RAM", "other RAM", "ROM", "texture", "TGP", "devices", "unmapped"};

} // namespace

void I960ProfileReport::start(uint64_t i960_instructions, uint64_t interrupts) {
    at_ = i960_profile;
    instructions_ = i960_instructions;
    interrupts_ = interrupts;
    dispatches_ = i960gen::dispatches;
    chunk_entries_ = i960gen::chunk_entries;
    events_ = i960gen::events;
    hits_.assign(i960gen::block_hits, i960gen::block_hits + i960gen::block_count);
}

size_t I960ProfileReport::format(char *out, size_t size, uint64_t frames, uint64_t i960_instructions, uint64_t interrupts,
                                 int cpu_mhz, double tgp_ms, double whole_ms, const char *whole) const {
    Writer w{out, size};
    const I960Profile &now = i960_profile;
    if (hits_.size() != i960gen::block_count || !frames) {
        w.add("i960: no window\n");
        return w.used;
    }
    const double f = double(frames);
    const double tps = double(now.ticks_per_second ? now.ticks_per_second : 1);
    const uint64_t runs = now.runs - at_.runs, instr = i960_instructions - instructions_;
    const double run_ms = double(now.ticks - at_.ticks) * 1000.0 / tps / f;
    const double board_ms = double(now.board_ticks - at_.board_ticks) * 1000.0 / tps / f;
    const double i960_ms = std::max(0.0, run_ms - board_ms - std::max(0.0, tgp_ms));
    const double ns_instr = instr ? i960_ms * f * 1e6 / double(instr) : 0.0;

    w.add("---- i960 (M2_I960_PROFILE) ----\n");
    w.add("i960 gen::run: %.3f ms/frame, %.1f calls/frame", run_ms, double(runs) / f);
    if (whole) w.add(" = %.1f%% of %s (%.3f ms/frame)", whole_ms > 0 ? 100.0 * run_ms / whole_ms : 0.0, whole, whole_ms);
    w.add("\n");
    w.add("i960 board callbacks inside it: %.3f ms/frame, %.1f calls/frame\n", board_ms,
          double(now.board_calls - at_.board_calls) / f);
    if (tgp_ms >= 0)
        w.add("i960 alone: %.3f ms/frame (gen::run minus the board callbacks and the TGP's %.3f), ", i960_ms, tgp_ms);
    else
        w.add("i960 + TGP: %.3f ms/frame (gen::run minus the board callbacks; no TGP profile to remove), ", i960_ms);
    w.add("%.0f instr/frame, %.2f ns/instr", double(instr) / f, ns_instr);
    if (cpu_mhz > 0) w.add(" = %.1f cycles/instr at %d MHz", ns_instr * double(cpu_mhz) / 1000.0, cpu_mhz);
    w.add(" | interrupts %.2f/frame\n", double(interrupts - interrupts_) / f);

    // Work: executions x contents of every block.
    double op[kI960Ops] = {};
    uint64_t counted = 0, executed = 0;
    for (uint32_t i = 0; i < i960gen::block_count; i++) {
        const uint64_t h = i960gen::block_hits[i] - hits_[i];
        if (!h) continue;
        executed += h;
        counted += h * i960gen::blocks[i].length;
        for (int k = 0; k < kI960Ops; k++) op[k] += double(h) * i960gen::blocks[i].ops[k];
    }
    auto pf = [&](double v) { return v / f; };
    auto cf = [&](uint64_t a, uint64_t b) { return double(a - b) / f; };
    w.add("i960 blocks/frame: %.0f executed, %.1f instr/block | dispatches %.0f, chunk entries %.0f, lockstep slow paths %.0f\n",
          double(executed) / f, executed ? double(counted) / double(executed) : 0.0, cf(i960gen::dispatches, dispatches_),
          cf(i960gen::chunk_entries, chunk_entries_), cf(i960gen::events, events_));
    w.add("i960 ops/frame:");
    for (int k = 0; k < kI960Ops; k++) w.add(" %s %.0f", kOpName[k], pf(op[k]));
    w.add("\n");
    uint64_t bus_r = 0, bus_w = 0;
    for (int k = 0; k < kBusRegions; k++) bus_r += now.bus_r[k] - at_.bus_r[k], bus_w += now.bus_w[k] - at_.bus_w[k];
    w.add("i960 bus calls/frame: reads %.0f (", double(bus_r) / f);
    for (int k = 0; k < kBusRegions; k++) w.add("%s%s %.0f", k ? ", " : "", kRegionName[k], cf(now.bus_r[k], at_.bus_r[k]));
    w.add("), writes %.0f (", double(bus_w) / f);
    for (int k = 0; k < kBusRegions; k++) w.add("%s%s %.0f", k ? ", " : "", kRegionName[k], cf(now.bus_w[k], at_.bus_w[k]));
    w.add(")\n");
    // A byte or word access unaligned for its size is several bus calls; a
    // direct work-RAM access (the Vita's --fast-inaccuracy pass) is none.
    w.add("i960 loads+stores %.0f words/frame, bus calls %.0f: %.0f made direct (or %.0f extra calls from unaligned accesses)\n",
          pf(op[I960OpLoad] + op[I960OpStore]), double(bus_r + bus_w) / f,
          std::max(0.0, pf(op[I960OpLoad] + op[I960OpStore]) - double(bus_r + bus_w) / f),
          std::max(0.0, double(bus_r + bus_w) / f - pf(op[I960OpLoad] + op[I960OpStore])));
    if (counted != instr)
        w.add("i960 blocks: %.0f instr/frame counted by blocks (entered mid-block counts the whole block; a taken "
              "interrupt leaves one uncounted)\n",
              double(counted) / f);

    // The hottest blocks by instructions.
    order_.resize(i960gen::block_count);
    for (uint32_t i = 0; i < i960gen::block_count; i++) order_[i] = i;
    auto weight = [&](uint32_t i) { return (i960gen::block_hits[i] - hits_[i]) * i960gen::blocks[i].length; };
    const size_t top = std::min<size_t>(15, order_.size());
    std::partial_sort(order_.begin(), order_.begin() + std::ptrdiff_t(top), order_.end(),
                      [&](uint32_t a, uint32_t b) { return weight(a) > weight(b); });
    w.add("i960 hottest blocks (instructions):\n");
    for (size_t r = 0; r < top; r++) {
        const uint32_t i = order_[r];
        const uint64_t wt = weight(i);
        if (!wt) break;
        const I960BlockInfo &bl = i960gen::blocks[i];
        w.add("  %08x len %3u: %5.1f%% of instr, %8.1f runs/frame |", unsigned(bl.start), unsigned(bl.length),
              counted ? 100.0 * double(wt) / double(counted) : 0.0, double(i960gen::block_hits[i] - hits_[i]) / f);
        for (int k = 0; k < kI960Ops; k++)
            if (bl.ops[k]) w.add(" %s %u", kOpName[k], unsigned(bl.ops[k]));
        w.add("\n");
    }
    return w.used;
}

} // namespace rt

#endif // M2_I960_PROFILE
