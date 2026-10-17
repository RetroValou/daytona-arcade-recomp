// m2dsprecomp: the SCSP DSP program recompiler. The DSP runs its program (MPRO,
// up to 128 steps of 4 words) once per sample, 44,100 times a second; MAME's
// interpreter (src/runtime/sound/scsp.cpp ScspDsp::Step) decodes every field of every
// step each time. This writes the program as C++ instead: one block per step,
// its fields resolved here, the same operations in the same order as the
// interpreter, on the same state (ACC, TEMP, MEMS, COEF, MADRS, the sound RAM).
//
//   m2dsprecomp DSP_PROGRAM.bin OUT.cpp
//   m2dsprecomp --random SEED STEPS OUT.cpp         (a synthetic program, for the tests:
//   m2dsprecomp --random-return SEED STEPS OUT.cpp   every input index valid; or one past
//                                                     0x31 half way, the interpreter's early return)
//
// DSP_PROGRAM.bin: the program as the game's sound ROM holds it (m2import:
// dsp_program.bin, Game::dsp), big-endian words, 4 per step; empty for a game
// without one (OUT.cpp then registers nothing). The generated file registers
// the program at start-up (snd::scsp_dsp_native); ScspDsp::Step runs it while
// MPRO and LastStep are that program's, else the interpreter.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", v);
    return b;
}

// One step, as ScspDsp::Step does it (scsp.cpp; the field names are MAME's).
// Returns false when the step ends the program (an input index past 0x31: the
// interpreter returns from Step there, before DEC and MIXS are updated).
bool emit_step(std::ostringstream &o, int step, const uint16_t *w) {
    const uint32_t TRA = (w[0] >> 8) & 0x7f, TWT = (w[0] >> 7) & 1, TWA = w[0] & 0x7f;
    const uint32_t XSEL = (w[1] >> 15) & 1, YSEL = (w[1] >> 13) & 3, IRA = (w[1] >> 6) & 0x3f, IWT = (w[1] >> 5) & 1,
                   IWA = w[1] & 0x1f;
    const uint32_t TABLE = (w[2] >> 15) & 1, MWT = (w[2] >> 14) & 1, MRD = (w[2] >> 13) & 1, EWT = (w[2] >> 12) & 1,
                   EWA = (w[2] >> 8) & 0xf, ADRL = (w[2] >> 7) & 1, FRCL = (w[2] >> 6) & 1, SHIFT = (w[2] >> 4) & 3,
                   YRL = (w[2] >> 3) & 1, NEGB = (w[2] >> 2) & 1, ZERO = (w[2] >> 1) & 1, BSEL = w[2] & 1;
    const uint32_t NOFL = (w[3] >> 15) & 1, COEF = (w[3] >> 9) & 0x3f, MASA = (w[3] >> 2) & 0x1f, ADREB = (w[3] >> 1) & 1,
                   NXADR = w[3] & 1;
    char head[96];
    std::snprintf(head, sizeof head, "    { // step %d: %04x %04x %04x %04x\n", step, w[0], w[1], w[2], w[3]);
    o << head;
    std::string in;
    if (IRA <= 0x1f) in = "d.MEMS[" + std::to_string(IRA) + "]";
    else if (IRA <= 0x2f) in = "(d.MIXS[" + std::to_string(IRA - 0x20) + "] << 4)";
    else if (IRA <= 0x31) in = "(d.EXTS[" + std::to_string(IRA - 0x30) + "] << 8)";
    else {
        o << "        return; // input index " << hex(IRA) << ": the interpreter returns here\n    }\n";
        return false;
    }
    const std::string temp_r = "sext(d.TEMP[(" + std::to_string(TRA) + "u + DEC) & 0x7f], 24)";
    o << "        [[maybe_unused]] int32_t INPUTS = sext(" << in << ", 24);\n";
    if (IWT) {
        o << "        d.MEMS[" << IWA << "] = MEMVAL;\n";
        if (IRA == IWA) o << "        INPUTS = MEMVAL;\n";
    }
    std::string B = "0";
    if (!ZERO) B = NEGB ? "0 - " + std::string(BSEL ? "ACC" : temp_r) : (BSEL ? "ACC" : temp_r);
    o << "        const int32_t B = " << B << ";\n";
    o << "        const int32_t X = " << (XSEL ? "INPUTS" : temp_r) << ";\n";
    static const char *const ys[4] = {"FRC_REG", nullptr, "(Y_REG >> 11) & 0x1fff", "(Y_REG >> 4) & 0x0fff"};
    o << "        int32_t Y = " << (YSEL == 1 ? "d.COEF[" + std::to_string(COEF) + "] >> 3" : std::string(ys[YSEL])) << ";\n";
    if (YRL) o << "        Y_REG = INPUTS;\n";
    static const char *const shifts[4] = {"std::clamp<int32_t>(ACC, -0x00800000, 0x007fffff)",
                                          "std::clamp<int32_t>(ACC * 2, -0x00800000, 0x007fffff)", "sext(ACC * 2, 24)",
                                          "sext(ACC, 24)"};
    o << "        [[maybe_unused]] const int32_t SHIFTED = " << shifts[SHIFT] << ";\n";
    o << "        Y = sext(Y, 13);\n";
    o << "        ACC = int(((int64_t(X) * int64_t(Y)) >> 12) + B);\n";
    if (TWT) o << "        d.TEMP[(" << TWA << "u + DEC) & 0x7f] = SHIFTED;\n";
    if (FRCL) o << (SHIFT == 3 ? "        FRC_REG = SHIFTED & 0x0fff;\n" : "        FRC_REG = (SHIFTED >> 11) & 0x1fff;\n");
    // Memory: only on odd steps (the interpreter computes the address on every
    // step with MRD or MWT, but reads or writes only on odd ones).
    if ((MRD || MWT) && (step & 1)) {
        o << "        {\n            uint32_t ADDR = d.MADRS[" << MASA << "];\n";
        if (!TABLE) o << "            ADDR += DEC;\n";
        if (ADREB) o << "            ADDR += ADRS_REG & 0x0FFF;\n";
        if (NXADR) o << "            ADDR++;\n";
        o << (!TABLE ? "            ADDR &= d.RBL - 1;\n" : "            ADDR &= 0xffff;\n");
        o << "            ADDR += d.RBP << 12;\n            ADDR <<= 1;\n";
        if (MRD) o << (NOFL ? "            MEMVAL = d.read_word(ADDR) << 8;\n" : "            MEMVAL = UNPACK(d.read_word(ADDR));\n");
        if (MWT)
            o << (NOFL ? "            d.write_word(ADDR, uint16_t(SHIFTED >> 8));\n" : "            d.write_word(ADDR, PACK(SHIFTED));\n");
        o << "        }\n";
    }
    if (ADRL)
        o << (SHIFT == 3 ? "        ADRS_REG = uint32_t(SHIFTED >> 12) & 0xfff;\n" : "        ADRS_REG = uint32_t(INPUTS >> 16);\n");
    if (EWT) o << "        d.EFREG[" << EWA << "] = int16_t(d.EFREG[" << EWA << "] + (SHIFTED >> 8));\n";
    o << "    }\n";
    return true;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<uint16_t> words;
    std::string out, source;
    if (argc == 5 && (!std::strcmp(argv[1], "--random") || !std::strcmp(argv[1], "--random-return"))) {
        // A synthetic program: every field random, the input index in the valid
        // range (0-0x31) but, with --random-return, past it at the middle step.
        std::mt19937 rng(uint32_t(std::strtoul(argv[2], nullptr, 0)));
        const int steps = std::atoi(argv[3]);
        const int bad = !std::strcmp(argv[1], "--random-return") ? steps / 2 : -1;
        for (int s = 0; s < steps; ++s)
            for (int k = 0; k < 4; ++k) {
                uint16_t v = uint16_t(rng());
                if (k == 1) v = uint16_t((v & ~0x0fc0) | ((s == bad ? 0x32 + rng() % 14 : rng() % 0x32) << 6));
                words.push_back(v);
            }
        if (!words.empty() && !words.back()) words.back() = 1; // a last step that is not all zero: LastStep = steps
        out = argv[4];
        source = std::string(argv[1]) + " " + argv[2] + " " + argv[3];
    } else if (argc == 3) {
        std::ifstream f(argv[1], std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "m2dsprecomp: cannot read %s\n", argv[1]);
            return 1;
        }
        std::vector<uint8_t> b{std::istreambuf_iterator<char>(f), {}};
        if (b.size() % 8) {
            std::fprintf(stderr, "m2dsprecomp: %s: %zu bytes, not whole steps of 8\n", argv[1], b.size());
            return 1;
        }
        for (size_t i = 0; i < b.size(); i += 2) words.push_back(uint16_t(b[i] << 8 | b[i + 1]));
        out = argv[2];
        source = argv[1];
    } else {
        std::fprintf(stderr, "usage: m2dsprecomp DSP_PROGRAM.bin OUT.cpp | m2dsprecomp --random SEED STEPS OUT.cpp\n");
        return 2;
    }
    // ScspDsp::Start: LastStep is one past the last step that is not all zero.
    int steps = int(words.size() / 4);
    while (steps > 0 && !(words[size_t(steps) * 4 - 4] | words[size_t(steps) * 4 - 3] | words[size_t(steps) * 4 - 2] |
                          words[size_t(steps) * 4 - 1]))
        --steps;
    if (steps > 128) {
        std::fprintf(stderr, "m2dsprecomp: %d steps, the DSP has 128\n", steps);
        return 1;
    }

    std::ostringstream o;
    o << "// Generated by m2dsprecomp from " << (source.rfind("--random", 0) == 0 ? source : std::string("the game's DSP program"))
      << ". Derived from the game: never commit.\n"
      << "#include \"runtime/sound/scsp.h\"\n\n#include <algorithm>\n#include <iterator>\n\nnamespace snd::dspgen {\n\n";
    o << "extern const int program_steps;\nconst int program_steps = " << steps << ";\n\n";
    if (steps) {
        o << "namespace {\n\nconst uint16_t kProgram[" << steps * 4 << "] = {";
        for (int i = 0; i < steps * 4; ++i) o << (i % 8 ? " " : "\n    ") << hex(words[size_t(i)]) << ",";
        o << "\n};\n\n";
        o << "void step(ScspDsp &d) {\n    using namespace snd::dsp;\n"
          << "    std::fill(std::begin(d.EFREG), std::end(d.EFREG), int16_t(0));\n"
          << "    [[maybe_unused]] int32_t ACC = 0, MEMVAL = 0, FRC_REG = 0, Y_REG = 0;\n"
          << "    [[maybe_unused]] uint32_t ADRS_REG = 0;\n"
          << "    [[maybe_unused]] const uint32_t DEC = d.DEC;\n";
        bool whole = true;
        for (int s = 0; s < steps && whole; ++s) whole = emit_step(o, s, &words[size_t(s) * 4]);
        if (whole) o << "    --d.DEC;\n    std::fill(std::begin(d.MIXS), std::end(d.MIXS), 0);\n";
        o << "}\n\nconst ScspDspNative kNative = {kProgram, " << steps << ", step};\n"
          << "struct Register {\n    Register() { scsp_dsp_native = &kNative; }\n} register_program;\n\n} // namespace\n\n";
    }
    o << "} // namespace snd::dspgen\n";
    std::ofstream f(out, std::ios::binary);
    if (!(f << o.str())) {
        std::fprintf(stderr, "m2dsprecomp: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("m2dsprecomp: %d steps -> %s%s\n", steps, out.c_str(), steps ? "" : " (no program: the interpreter only)");
    return 0;
}
