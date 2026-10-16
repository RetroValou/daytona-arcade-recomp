// floor(count * N0 / D0) for counts that mostly grow, without the 64-bit
// division (a library call on the Vita's Cortex-A9, ~150-250 cycles) in the
// common case. The fraction is reduced at compile time (the same floor); the
// last count, quotient and remainder are kept (count * N == q * D + r,
// 0 <= r < D), so a step d forward costs a multiply and a 32-bit division by
// the constant D (a multiply by its inverse). A count that goes back, or a
// step too large for 32 bits, takes the original 64-bit expression: every
// result is count * N0 / D0's (tests/test_rate_conv.cpp).
#pragma once

#include <cstdint>
#include <numeric>

namespace rt {

template <uint64_t N0, uint64_t D0>
class RateConv {
    static constexpr uint64_t G = std::gcd(N0, D0), N = N0 / G, D = D0 / G;
    static_assert(D < (uint64_t(1) << 31), "D must leave room in 32 bits");
    static constexpr uint64_t kMaxStep = (uint64_t(0xffffffffu) - D) / (N % D ? N % D : 1); // r + d * (N % D) < 2^32
    uint64_t c_ = 0, q_ = 0;
    uint32_t r_ = 0;

public:
    uint64_t at(uint64_t count) {
        if (count >= c_ && count - c_ <= kMaxStep) {
            const uint64_t d = count - c_;
            const uint32_t t = r_ + uint32_t(d * (N % D));
            q_ += d * (N / D) + t / uint32_t(D);
            r_ = t % uint32_t(D);
        } else {
            q_ = count * N / D;
            r_ = uint32_t(count * N % D);
        }
        c_ = count;
        return q_;
    }
};

} // namespace rt
