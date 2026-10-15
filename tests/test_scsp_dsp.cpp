// The SCSP DSP recompiler (tools/m2dsprecomp) against MAME's interpreter
// (src/runtime/scsp.cpp ScspDsp::Step): a synthetic program (m2dsprecomp
// --random, built into test_dsp_gen.cpp, which registers it), the same random
// state in two DSPs, one held to the interpreter, the other running the
// recompiled program; every sample, with new inputs, the whole state and the
// sound RAM must stay identical. No ROM.
#include "runtime/scsp.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace snd::dspgen {
extern const int program_steps;
}

int main() {
    const snd::ScspDspNative *native = snd::scsp_dsp_native;
    if (!native || native->steps != snd::dspgen::program_steps || native->steps < 1) {
        std::fprintf(stderr, "test_scsp_dsp: no recompiled program registered\n");
        return 1;
    }
    std::mt19937 rng(1234);
    std::vector<uint8_t> ram_a(0x80000), ram_b;
    for (uint8_t &b : ram_a) b = uint8_t(rng());
    ram_b = ram_a;
    snd::ScspDsp a{}, b{};
    a.ram = ram_a.data();
    a.Init();
    a.RBL = (8 * 1024) << (rng() & 3);
    a.RBP = rng() & 0x7f;
    for (int16_t &c : a.COEF) c = int16_t(rng());
    for (uint16_t &m : a.MADRS) m = uint16_t(rng());
    for (int32_t &t : a.TEMP) t = int32_t(rng()) >> 8;
    for (int32_t &m : a.MEMS) m = int32_t(rng()) >> 8;
    a.DEC = rng();
    std::memcpy(a.MPRO, native->mpro, size_t(native->steps) * 4 * sizeof(uint16_t));
    a.Start();
    b = a;
    b.ram = ram_b.data();

    // a: the interpreter (no program registered when it first checks).
    snd::scsp_dsp_native = nullptr;
    a.Step();
    snd::scsp_dsp_native = native;
    b.Step();
    if (a.native || !b.native) {
        std::fprintf(stderr, "test_scsp_dsp: expected the interpreter in a and the recompiled program in b\n");
        return 1;
    }
    int samples = 1;
    for (; samples < 20000; ++samples) {
        auto same = [&] {
            return !std::memcmp(a.EFREG, b.EFREG, sizeof a.EFREG) && !std::memcmp(a.TEMP, b.TEMP, sizeof a.TEMP) &&
                   !std::memcmp(a.MEMS, b.MEMS, sizeof a.MEMS) && !std::memcmp(a.MIXS, b.MIXS, sizeof a.MIXS) &&
                   a.DEC == b.DEC && ram_a == ram_b;
        };
        if (!same()) {
            std::fprintf(stderr, "test_scsp_dsp: state differs after sample %d\n", samples);
            return 1;
        }
        for (int i = 0; i < 16; ++i) {
            const int32_t v = int32_t(rng()) >> 12; // 20-bit mixer inputs
            a.SetSample(v, i, 0);
            b.SetSample(v, i, 0);
        }
        for (int i = 0; i < 2; ++i) a.EXTS[i] = b.EXTS[i] = int16_t(rng());
        if (rng() % 64 == 0) { // the 68000 changing a coefficient or an address now and then
            const int c = int(rng() % 64);
            a.COEF[c] = b.COEF[c] = int16_t(rng());
            const int m = int(rng() % 32);
            a.MADRS[m] = b.MADRS[m] = uint16_t(rng());
        }
        a.Step();
        b.Step();
    }
    std::printf("test_scsp_dsp: %d-step program, %d samples: the recompiled program and the interpreter agree "
                "(EFREG, TEMP, MEMS, MIXS, DEC, sound RAM)\n", native->steps, samples);
    return 0;
}
