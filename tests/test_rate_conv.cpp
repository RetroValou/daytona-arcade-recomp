// rt::RateConv against the 64-bit expression it replaces, count * N0 / D0:
// counts that grow by random steps (0 to 100,000, and past the 32-bit fast
// path), go back, and jump, for the fractions the boards use.
#include "runtime/rate_conv.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>

namespace {

unsigned failures = 0;
uint64_t checked = 0;

template <uint64_t N0, uint64_t D0>
void check(const char *name, uint64_t seed) {
    rt::RateConv<N0, D0> conv;
    std::mt19937_64 rng(seed);
    uint64_t count = 0;
    for (int i = 0; i < 5'000'000; ++i) {
        const uint64_t pick = rng() % 1000;
        if (pick == 0) count = rng() % (uint64_t(1) << 38);                // a jump
        else if (pick == 1) count -= std::min<uint64_t>(count, rng() % 100000); // back
        else if (pick == 2) count += rng() % (uint64_t(1) << 34);           // past the fast path
        else count += rng() % 100001;
        count &= (uint64_t(1) << 38) - 1; // keeps count * N0 in 64 bits (wrapping: a step back)
        ++checked;
        const uint64_t want = count * N0 / D0, got = conv.at(count);
        if (want != got && failures++ < 10)
            std::printf("%s: count %llu: want %llu, got %llu\n", name, (unsigned long long)count,
                        (unsigned long long)want, (unsigned long long)got);
    }
}

} // namespace

int main() {
    check<44100, 825876>("SCSP samples (Scsp::kRate / ScspBoard::kIps)", 1);
    check<8000000, 144 * 1000000>("FM-like", 2);
    check<434783, 110000>("ticks-like (N > D)", 3);
    if (failures) {
        std::printf("rate_conv: %u of %llu counts differ\n", failures, (unsigned long long)checked);
        return 1;
    }
    std::printf("rate_conv: %llu counts, all as count * N / D\n", (unsigned long long)checked);
    return 0;
}
