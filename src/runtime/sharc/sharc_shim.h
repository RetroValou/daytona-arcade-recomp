// The few MAME helpers the SHARC code lifted from MAME uses (src/runtime/sharc;
// THIRD_PARTY.md), with MAME's meanings: BIT and util::sext, util::make_bitmask
// (lib/util/coretmpl.h), emu_fatalerror and fatalerror (as an exception), and
// stand-ins for the timer types its DRC-only branches name.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace rt::sharc {

using offs_t = uint32_t;
using u32 = uint32_t;

// An instruction the lifted core does not implement, or a state MAME treats as
// fatal (its emu_fatalerror): the run stops and says which.
struct SharcFatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct emu_fatalerror : SharcFatal {
    template <typename... A>
    static std::string fmt(const char *format, A... args) {
        if constexpr (sizeof...(A) == 0) {
            return format;
        } else {
            char b[256];
            std::snprintf(b, sizeof b, format, args...);
            return b;
        }
    }
    template <typename... A>
    emu_fatalerror(const char *format, A... args) : SharcFatal(fmt(format, args...)) {}
};

template <typename... A>
[[noreturn]] inline void fatalerror(const char *format, A... args) {
    throw emu_fatalerror(format, args...);
}

template <typename T, typename U>
constexpr T BIT(T x, U n) noexcept {
    return (x >> n) & T(1);
}
template <typename T, typename U, typename V>
constexpr T BIT(T x, U n, V w) {
    return (x >> n) & T(((T(1) << w) - 1));
}

namespace util {
template <typename T, typename U>
constexpr std::make_signed_t<T> sext(T value, U width) noexcept {
    using S = std::make_signed_t<T>;
    return S(value << (8 * sizeof(T) - width)) >> (8 * sizeof(T) - width);
}
template <typename T, typename U>
constexpr T make_bitmask(U n) {
    return T((n < U(8 * sizeof(T)) ? (std::make_unsigned_t<T>(1) << n) : std::make_unsigned_t<T>(0)) - 1);
}
// A little-endian host (every target here): the plain pointer cast.
template <typename T, typename U>
T *little_endian_cast(U *p) noexcept {
    return reinterpret_cast<T *>(p);
}
} // namespace util

// MAME's eminline.h multiplies (their portable versions).
constexpr int64_t mul_32x32(int32_t a, int32_t b) { return int64_t(a) * int64_t(b); }
constexpr uint64_t mulu_32x32(uint32_t a, uint32_t b) { return uint64_t(a) * uint64_t(b); }
constexpr int32_t mul_32x32_shift(int32_t a, int32_t b, uint8_t shift) { return int32_t((int64_t(a) * int64_t(b)) >> shift); }

// The DMA timers only the DRC path uses (m_enable_drc is false here).
struct attotime {
    static constexpr int never = 0;
};
struct emu_timer {
    void adjust(int, int = 0) {}
};
inline int cycles_to_attotime(int) { return 0; }

} // namespace rt::sharc

#define ATTR_COLD
#define TIMER_CALLBACK_MEMBER(name) void name(int param)
#define ACCESSING_BITS_0_15 ((mem_mask & 0x0000ffffU) != 0)
