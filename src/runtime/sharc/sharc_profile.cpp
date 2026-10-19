// SHARC profiling report (sharc_profile.h): one window's counters, as text.
#include "runtime/sharc/sharc_profile.h"

#ifdef M2_SHARC_PROFILE

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace rt::sharc {

namespace {

struct Out {
    char *p;
    size_t size, used = 0;
    void add(const char *fmt, ...) {
        if (used >= size) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(p + used, size - used, fmt, ap);
        va_end(ap);
        if (n > 0) used = std::min(size - 1, used + size_t(n));
    }
};

double ratio(double a, double b) { return b > 0 ? a / b : 0.0; }

} // namespace

void SharcProfileReport::start(uint64_t sharc_instructions, const uint32_t *counts) {
    if (clock_ns_ < 0 && sharc_profile.clock) { // the clock's own cost, once
        constexpr int kReads = 2000;
        const uint64_t t0 = sharc_profile.clock();
        uint64_t sink = 0;
        for (int i = 0; i < kReads; ++i) sink += sharc_profile.clock();
        const uint64_t t1 = sharc_profile.clock();
        clock_ns_ = sink ? double(t1 - t0) * 1e9 / double(sharc_profile.ticks_per_second) / kReads : 0.0;
    }
    at_ = sharc_profile;
    instructions_ = sharc_instructions;
    if (counts && sharc_gen_count) counts_.assign(counts, counts + sharc_gen_count);
}

size_t SharcProfileReport::format(char *out, size_t size, uint64_t frames, uint64_t sharc_instructions, const uint32_t *counts,
                                  int cpu_mhz) {
    const SharcProfile &now = sharc_profile;
    const auto d = [](uint64_t a, uint64_t b) { return double(a - b); };
    const double f = double(frames ? frames : 1);
    const double instr = double(sharc_instructions - instructions_);
    const double tps = double(now.ticks_per_second ? now.ticks_per_second : 1);
    Out o{out, size};

    o.add("SHARC profile (--sharc-profile; a profiling build: each instruction also counts itself), %.0f frames, "
          "%.0f instructions a frame\n", f, instr / f);

    // By caller.
    static const char *const kCaller[kSharcCallers] = {"run to the wait (vblank, sync)", "need an output word (i960 FIFO read)",
                                                       "256-instruction slice (buffer RAM read)"};
    double all_ms = 0, all_calls = 0;
    for (int c = 0; c < kSharcCallers; ++c) {
        const double ms = d(now.ticks[c], at_.ticks[c]) * 1000.0 / tps / f;
        all_ms += ms;
        all_calls += d(now.calls[c], at_.calls[c]);
    }
    o.add("  by caller (a frame)                       calls   gen calls       instr      ms  instr/call  ns/instr\n");
    for (int c = 0; c < kSharcCallers; ++c) {
        const double calls = d(now.calls[c], at_.calls[c]) / f, runs = d(now.runs[c], at_.runs[c]) / f;
        const double in = d(now.instructions[c], at_.instructions[c]) / f;
        const double ms = d(now.ticks[c], at_.ticks[c]) * 1000.0 / tps / f;
        o.add("    %-40s %8.1f %10.1f %11.0f %7.2f %11.1f %9.1f\n", kCaller[c], calls, runs, in, ms, ratio(in, calls),
              ratio(ms * 1e6, in));
    }
    if (clock_ns_ >= 0)
        o.add("    clock: %.2f us a read; the callers' reads: %.3f ms a frame (in the times above)\n", clock_ns_ / 1000.0,
              all_calls / f * 2 * clock_ns_ / 1e6);
    if (cpu_mhz > 0 && instr > 0)
        o.add("    all: %.2f ms a frame, %.1f ns an instruction = %.0f cycles at %d MHz\n", all_ms, all_ms * 1e6 / (instr / f),
              all_ms * 1e6 / (instr / f) * cpu_mhz / 1000.0, cpu_mhz);

    // run_generated.
    const double gen = d(now.gen_calls, at_.gen_calls);
    o.add("  run_generated: %.1f calls a frame, %.1f instructions a call; chunk calls %.1f, dispatches %.1f, "
          "plain run ends %.1f, a frame\n",
          gen / f, ratio(instr, gen), d(now.chunk_calls, at_.chunk_calls) / f, d(now.dispatches, at_.dispatches) / f,
          d(now.run_ends, at_.run_ends) / f);
    o.add("    per 1000 instructions: dispatches %.1f, run ends %.1f, branches taken %.1f, after delay slots %.1f, "
          "loop backs %.1f, full steps %.1f, interpreter steps %.2f (%.2f entries)\n",
          ratio(d(now.dispatches, at_.dispatches) * 1000, instr), ratio(d(now.run_ends, at_.run_ends) * 1000, instr),
          ratio(d(now.taken, at_.taken) * 1000, instr), ratio(d(now.slot_taken, at_.slot_taken) * 1000, instr),
          ratio(d(now.loop_backs, at_.loop_backs) * 1000, instr), ratio(d(now.full_steps, at_.full_steps) * 1000, instr),
          ratio(d(now.fallback_steps, at_.fallback_steps) * 1000, instr),
          ratio(d(now.fallback_entries, at_.fallback_entries) * 1000, instr));
    static const char *const kSize[kSharcSizeBuckets] = {"0", "1-16", "17-64", "65-256", "257-4096", ">4096"};
    o.add("    calls by instructions run:");
    for (int b = 0; b < kSharcSizeBuckets; ++b) o.add(" %s %.1f%%", kSize[b], ratio(d(now.sizes[b], at_.sizes[b]) * 100, gen));
    o.add("; exits: waiting %.1f, budget %.1f a frame\n", d(now.exit_wait, at_.exit_wait) / f, d(now.exit_budget, at_.exit_budget) / f);

    // Memory.
    o.add("  memory, per 1000 instructions: data space: internal r %.0f w %.0f, 16-bit view r %.1f w %.1f, IOP r %.1f w %.1f, "
          "board r %.1f w %.1f; program memory data r %.1f w %.1f (into the code's range %.1f)\n",
          ratio(d(now.dm_r, at_.dm_r) * 1000, instr), ratio(d(now.dm_w, at_.dm_w) * 1000, instr),
          ratio(d(now.short_r, at_.short_r) * 1000, instr), ratio(d(now.short_w, at_.short_w) * 1000, instr),
          ratio(d(now.iop_r, at_.iop_r) * 1000, instr), ratio(d(now.iop_w, at_.iop_w) * 1000, instr),
          ratio(d(now.ext_r, at_.ext_r) * 1000, instr), ratio(d(now.ext_w, at_.ext_w) * 1000, instr),
          ratio(d(now.pm_r, at_.pm_r) * 1000, instr), ratio(d(now.pm_w, at_.pm_w) * 1000, instr),
          ratio(d(now.pm_w_code, at_.pm_w_code) * 1000, instr));
    o.add("  board, a frame: input FIFO words %.0f (empty reads %.0f), output FIFO words %.0f, buffer RAM r %.0f w %.0f, "
          "data ROM r %.0f, other %.0f\n",
          d(now.fifo_in, at_.fifo_in) / f, d(now.fifo_in_empty, at_.fifo_in_empty) / f, d(now.fifo_out, at_.fifo_out) / f,
          d(now.buffer_r, at_.buffer_r) / f, d(now.buffer_w, at_.buffer_w) / f, d(now.rom_r, at_.rom_r) / f, d(now.other, at_.other) / f);

    // The program: kinds, handlers, hottest instructions.
    if (counts && sharc_gen_count && counts_.size() == sharc_gen_count) {
        std::vector<std::pair<uint32_t, uint32_t>> hot; // executions, index
        double kinds[3] = {}, total = 0;
        std::map<std::string, double> by_name;
        for (uint32_t k = 0; k < sharc_gen_count; ++k) {
            const uint32_t n = counts[k] - counts_[k];
            if (!n) continue;
            total += n;
            kinds[sharc_gen_kinds[k] < 3 ? sharc_gen_kinds[k] : 2] += n;
            by_name[sharc_gen_names[k]] += n;
            hot.push_back({n, k});
        }
        o.add("  executed: %.0f distinct instructions; plain %.1f%%, pinned branches %.1f%%, full steps %.1f%% "
              "(counted by the code; the interpreter's steps not)\n",
              double(hot.size()), ratio(kinds[SharcPlain] * 100, total), ratio(kinds[SharcPinned] * 100, total),
              ratio(kinds[SharcFull] * 100, total));
        std::vector<std::pair<double, std::string>> names;
        for (const auto &[name, n] : by_name) names.push_back({n, name});
        std::sort(names.rbegin(), names.rend());
        o.add("  handlers:");
        for (size_t i = 0; i < names.size() && i < 12; ++i) o.add(" %s %.1f%%", names[i].second.c_str(), ratio(names[i].first * 100, total));
        o.add("\n");
        std::sort(hot.rbegin(), hot.rend());
        double share = 0;
        size_t half = 0, most = 0;
        for (size_t i = 0; i < hot.size(); ++i) {
            share += hot[i].first;
            if (!half && share >= total * 0.5) half = i + 1;
            if (!most && share >= total * 0.9) most = i + 1;
        }
        o.add("  hot set: 50%% of the instructions in %u addresses, 90%% in %u; the hottest:\n", unsigned(half), unsigned(most)); // no %zu on the Vita
        static const char kKind[3] = {'p', 'b', 'F'};
        for (size_t i = 0; i < hot.size() && i < 16; ++i) {
            const uint32_t k = hot[i].second;
            o.add("    %05x %c %-34s %6.2f%% %9.0f a frame\n", 0x20000 + k, kKind[sharc_gen_kinds[k] < 3 ? sharc_gen_kinds[k] : 2],
                  sharc_gen_names[k], ratio(hot[i].first * 100.0, total), hot[i].first / f);
        }
        o.add("    (p plain, b pinned branch, F full step)\n");
    }
    start(sharc_instructions, counts);
    return o.used;
}

} // namespace rt::sharc

#endif // M2_SHARC_PROFILE
