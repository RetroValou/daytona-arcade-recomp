// dsp::PACK (one CLZ) against MAME scspdsp.cpp's PACK (the bit loop), on every
// 24-bit val and a few int32_t values outside that range.
#include <cstdint>
#include <cstdio>
#include "runtime/sound/scsp.h"

static uint16_t pack_loop(int32_t val) {
    int const sign = (val >> 23) & 1;
    uint32_t temp = uint32_t(val ^ (val << 1)) & 0xFFFFFF;
    int exponent = 0;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000) break;
        temp <<= 1;
        exponent += 1;
    }
    if (exponent < 12) val = (val << exponent) & 0x3FFFFF;
    else val <<= 11;
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return uint16_t(val);
}

int main() {
    unsigned failures = 0, checked = 0;
    auto check = [&](int32_t v) {
        ++checked;
        const uint16_t want = pack_loop(v), got = snd::dsp::PACK(v);
        if (want != got && failures++ < 10) std::printf("val 0x%08x: loop 0x%04x, clz 0x%04x\n", uint32_t(v), want, got);
    };
    for (int32_t v = -0x800000; v <= 0x7fffff; ++v) check(v);
    for (int32_t v : {0x800000, -0x800001, 0x1000000, -0x1000000, 0x7fffffff, int32_t(0x80000000), 0x12345678, -0x12345678})
        check(v);
    if (failures) {
        std::printf("scsp_pack: %u of %u values differ\n", failures, checked);
        return 1;
    }
    std::printf("scsp_pack: %u values, all as MAME's loop\n", checked);
    return 0;
}
