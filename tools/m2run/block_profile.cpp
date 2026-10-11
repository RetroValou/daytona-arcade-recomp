// Block profile counters (block_profile.h). Built into m2run only, with
// M2_PROFILE: the generated code calls hit() at each basic block while on.
#include "block_profile.h"

#include "runtime/gen_support.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gen::prof {

bool on = false;

namespace {
std::vector<uint64_t> counts;                 // by block index
std::unordered_map<uint64_t, uint64_t> edges; // (from << 32 | to) -> count
uint32_t last = UINT32_MAX;
} // namespace

void hit(uint32_t block) {
    ++counts[block];
    if (last != UINT32_MAX) ++edges[uint64_t(last) << 32 | block];
    last = block;
}

void start() {
    counts.assign(block_count(), 0);
    edges.reserve(1 << 16);
    last = UINT32_MAX;
    on = true;
}

void write(const std::string &path, uint64_t frames, uint64_t instructions) {
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fprintf(f, "# m2run block profile: %" PRIu64 " frames, %" PRIu64 " i960 instructions\n", frames, instructions);
    std::fprintf(f, "# block ADDRESS INSTRUCTIONS COUNT / edge FROM TO COUNT (tools/m2run/block_profile.h)\n");
    for (size_t b = 0; b < counts.size(); ++b)
        if (counts[b]) std::fprintf(f, "block %08x %u %" PRIu64 "\n", block_addr(b), block_length(b), counts[b]);
    std::vector<std::pair<uint64_t, uint64_t>> sorted(edges.begin(), edges.end());
    std::sort(sorted.begin(), sorted.end());
    for (const auto &[e, n] : sorted)
        std::fprintf(f, "edge %08x %08x %" PRIu64 "\n", block_addr(size_t(e >> 32)), block_addr(size_t(e & 0xffffffffu)), n);
    std::fclose(f);
}

} // namespace gen::prof
