// m2sharcrecomp: recompile the SHARC program a Model 2B game uploads (Fighting
// Vipers) to C++.
//
//   m2sharcrecomp SHARC_PROGRAM.bin OUT.cpp [--ops SHARCOPS_TABLE.inc] [--sharcops SHARCOPS.hxx]
//                 [--compute COMPUTE.hxx] [--full-steps]
//
// SHARC_PROGRAM.bin is m2import's cut of the upload: 16-bit little-endian words,
// three per 48-bit instruction (DMA channel 6's 16/48 packing, MSWF clear: the
// first word is the low 16 bits), loaded from 0x20000 one instruction an
// address (the IOP set-up every Model 2B game's boot writes; the generated code
// compares program memory with it on its first run). Every instruction of the
// upload is recompiled, executed or not (some are data): no reachability
// analysis to get wrong. Each one runs MAME's interpreter step as
// src/runtime/sharc's step() does (step_pre, step_fetch, step_loop_end,
// the instruction's handler, step_after). The handlers are upstream's, made
// templates on the opcode: the output carries a copy of every sharcop_* of
// sharcops.hxx (and COMPUTE, on its compute field) with m_core->opcode replaced
// by the template parameter, so each instruction's decoding folds at compile
// time; the copy is made at each run, from the file as it is (the code is
// MAME's word for word: only that substitution and COMPUTE(x) calls becoming
// COMPUTE_t<x>()). A handler is named from MAME's opcode table
// (sharcops_table.inc, as build_opcode_table does). The loop-end
// check only where a do-until can end a loop (laddr is only ever a do-until's
// target), and control falls through to the next instruction when the
// pipeline's next address is it, else through a dispatch on it. A one-nop
// do-until loop that loops again (the SHARC polling its flags) returns
// "waiting" to the caller. The output includes the lifted core (it is the
// core's other build): link it instead of the `sharc` library.

#include <algorithm>
#include "runtime/sharc/sharc.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <bit>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

struct Op {
    uint16_t mask, bits;
    std::string name;
};

// MAME's table, in its order; the first match wins (build_opcode_table).
std::vector<Op> read_ops(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string text{std::istreambuf_iterator<char>(f), {}};
    const std::regex entry(R"(\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*&adsp21062_device::(\w+)\s*\})");
    std::vector<Op> ops;
    for (std::sregex_iterator i(text.begin(), text.end(), entry), end; i != end; ++i)
        ops.push_back({uint16_t(std::stoul((*i)[1], nullptr, 16)), uint16_t(std::stoul((*i)[2], nullptr, 16)), (*i)[3]});
    if (ops.empty()) throw std::runtime_error(path + ": no opcode table entries");
    return ops;
}

// The text from `open` (a '{') to its matching '}', skipping comments and
// string and character literals.
size_t match_brace(const std::string &t, size_t open) {
    int depth = 0;
    for (size_t i = open; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') i = t.find('\n', i);
        else if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') i = t.find("*/", i + 2) + 1;
        else if (c == '"' || c == '\'') {
            for (++i; i < t.size() && t[i] != c; ++i)
                if (t[i] == '\\') ++i;
        } else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return i;
        if (i == std::string::npos) break;
    }
    throw std::runtime_error("unbalanced braces in sharcops.hxx");
}

// Every identifier `word` not preceded by '.', "->" or an identifier character.
std::string replace_word(const std::string &t, const std::string &word, const std::string &with, bool member_too) {
    std::string out;
    size_t i = 0;
    for (size_t at; (at = t.find(word, i)) != std::string::npos; i = at + word.size()) {
        const auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        const bool before_ok = at == 0 || !ident(t[at - 1]);
        const bool after_ok = at + word.size() >= t.size() || !ident(t[at + word.size()]);
        const bool member = at >= 1 && (t[at - 1] == '.' || (at >= 2 && t[at - 2] == '-' && t[at - 1] == '>'));
        out += t.substr(i, at - i);
        out += before_ok && after_ok && (member_too || !member) ? with : word;
    }
    return out + t.substr(i);
}

// COMPUTE(x) -> COMPUTE_t<uint32_t(x)>().
std::string templated_compute_calls(const std::string &t) {
    std::string out;
    size_t i = 0;
    for (size_t at; (at = t.find("COMPUTE(", i)) != std::string::npos;) {
        const bool word = at == 0 || !(std::isalnum(static_cast<unsigned char>(t[at - 1])) || t[at - 1] == '_');
        if (!word) { out += t.substr(i, at + 8 - i); i = at + 8; continue; }
        int depth = 0;
        size_t close = at + 7;
        for (; close < t.size(); ++close) {
            if (t[close] == '(') ++depth;
            else if (t[close] == ')' && --depth == 0) break;
        }
        out += t.substr(i, at - i) + "COMPUTE_t<uint32_t(" + t.substr(at + 8, close - at - 8) + ")>()";
        i = close + 1;
    }
    return out + t.substr(i);
}

// sharcops.hxx's sharcop_* and COMPUTE, as templates on the opcode.
std::string templated_handlers(const std::string &path, size_t &count) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string t{std::istreambuf_iterator<char>(f), {}};
    const std::regex def(R"(\n(\[\[noreturn\]\] )?void adsp21062_device::(sharcop_\w+|COMPUTE)\(([^)]*)\)\s*\n\{)");
    std::ostringstream o;
    count = 0;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const std::string name = (*i)[2];
        const size_t open = size_t(i->position(0) + i->length(0) - 1);
        const size_t close = match_brace(t, open);
        std::string body = t.substr(open, close - open + 1);
        if (name == "COMPUTE") {
            if ((*i)[3] != "uint32_t opcode") throw std::runtime_error("COMPUTE's signature changed upstream");
            o << "template <uint32_t OPCODE>\nSHARC_INLINE void adsp21062_device::COMPUTE_t()\n" << replace_word(body, "opcode", "OPCODE", false) << "\n\n";
        } else {
            if (!(*i)[3].str().empty()) throw std::runtime_error(name + " takes arguments upstream");
            body = templated_compute_calls(replace_word(body, "m_core->opcode", "OPCODE", true));
            for (const char *f : {"GET_UREG", "SET_UREG", "SHIFT_OPERATION_IMM", "dm_read32", "dm_write32", "pm_read32", "pm_write32"})
                body = replace_word(body, f, std::string(f) + "_i", false);
            // Forced inline (SHARC_INLINE): the Vita's GCC otherwise calls most of
            // them (a call an instruction, the registers saved and m_core reloaded).
            o << "template <uint64_t OPCODE>\n" << ((*i)[1].matched ? (*i)[1].str() : std::string("SHARC_INLINE ")) << "void adsp21062_device::"
              << name << "_t()\n" << body << "\n\n";
        }
        ++count;
    }
    if (count < 40) throw std::runtime_error(path + ": only " + std::to_string(count) + " handlers found");
    return o.str();
}

// sharcops.hxx's handler bodies, by name.
std::map<std::string, std::string> handler_bodies(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string t{std::istreambuf_iterator<char>(f), {}};
    const std::regex def(R"(\nvoid adsp21062_device::(sharcop_\w+)\(\)\s*\n\{)");
    std::map<std::string, std::string> bodies;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const size_t open = size_t(i->position(0) + i->length(0) - 1);
        bodies[i->str(1)] = t.substr(open, match_brace(t, open) - open + 1);
    }
    return bodies;
}

// The handlers of sharcops.hxx that can delay a branch (CHANGE_PC_DELAYED):
// each does it only when the opcode's J bit (26) is set.
std::set<std::string> delaying_handlers(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::string t{std::istreambuf_iterator<char>(f), {}};
    const std::regex def(R"(\nvoid adsp21062_device::(sharcop_\w+)\(\)\s*\n\{)");
    std::set<std::string> names;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const size_t open = size_t(i->position(0) + i->length(0) - 1);
        const std::string body = t.substr(open, match_brace(t, open) - open + 1);
        if (body.find("CHANGE_PC_DELAYED") == std::string::npos) continue;
        if (body.find("op_get_jump_j(m_core->opcode)") == std::string::npos)
            throw std::runtime_error(std::string(i->str(1)) + " delays a branch without the J bit upstream");
        names.insert(i->str(1));
    }
    if (names.size() < 8) throw std::runtime_error(path + ": only " + std::to_string(names.size()) + " delayed branch handlers found");
    return names;
}

std::string read_text(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

// Every #define of a file, with its continuation lines.
std::string defines(const std::string &path) {
    std::istringstream in(read_text(path));
    std::string line, out;
    bool cont = false;
    while (std::getline(in, line)) {
        if (cont || line.rfind("#define", 0) == 0) {
            out += line + "\n";
            cont = !line.empty() && line.back() == '\\';
        }
    }
    return out;
}

// sharcops.hxx's inline helpers (the PC, loop and status stacks, the
// conditions), word for word.
std::string inline_helpers(const std::string &path) {
    const std::string t = read_text(path);
    const std::regex def(R"(\ninline [^\n]*adsp21062_device::\w+\([^)]*\)\s*\n\{)");
    std::string out;
    size_t count = 0;
    for (std::sregex_iterator i(t.begin(), t.end(), def), end; i != end; ++i) {
        const size_t start = size_t(i->position(0)) + 1, open = size_t(i->position(0) + i->length(0) - 1);
        out += "SHARC_INLINE " + t.substr(start + 7, match_brace(t, open) - start - 6) + "\n\n"; // "inline " forced
        ++count;
    }
    if (count < 9) throw std::runtime_error(path + ": fewer inline helpers than expected");
    return out;
}

// GET_UREG, SET_UREG and SHIFT_OPERATION_IMM, word for word, as inline
// GET_UREG_i, SET_UREG_i, SHIFT_OPERATION_IMM_i: called by the templated
// handlers with constant arguments, their switches fold.
std::string inline_accessors(const std::string &path) {
    const std::string t = read_text(path);
    std::string out;
    for (const char *name : {"GET_UREG", "SET_UREG", "SHIFT_OPERATION_IMM"}) {
        const std::regex def(std::string(R"(\n(uint32_t|void) adsp21062_device::)") + name + R"(\(([^)]*)\)\s*\n\{)");
        std::smatch m;
        if (!std::regex_search(t, m, def)) throw std::runtime_error(path + ": no " + name);
        const size_t open = size_t(m.position(0) + m.length(0) - 1);
        const std::string body = t.substr(open, match_brace(t, open) - open + 1);
        out += "SHARC_INLINE " + m[1].str() + " adsp21062_device::" + name + "_i(" + m[2].str() + ")\n" + body + "\n\n";
    }
    return out;
}

// compute.hxx without its #include lines and its two lookup tables.
std::string compute_inline(const std::string &path) {
    std::string t = read_text(path);
    t = std::regex_replace(t, std::regex(R"(\n#include [^\n]*)"), "\n");
    const std::regex table(R"(const uint32_t adsp21062_device::\w+\[128\] =\s*\{[^}]*\};)");
    if (std::distance(std::sregex_iterator(t.begin(), t.end(), table), std::sregex_iterator()) != 2)
        throw std::runtime_error(path + ": not the two lookup tables expected");
    t = std::regex_replace(t, table, "");
    // Its member functions forced inline (sharc.h declares them inline).
    return std::regex_replace(t, std::regex(R"(\n([A-Za-z][^\n]*?adsp21062_device::\w+\([^\n]*\))[ \t]*\n\{)"), "\nSHARC_INLINE $1\n{");
}

// Flag analysis: which of the ALU, multiplier and shifter flags of ASTAT (the
// only ones it may leave uncomputed: AF, BTF and the compare accumulation are
// always computed) a plain instruction reads and always writes, measured by
// running MAME's handler (the lifted core) on many states: a flag is read if
// flipping it on entry changes anything else, written if its value on exit
// never depends on its value on entry. Registers and floats from a pool of
// edge values (0, +-1, the extremes, +-0.0, infinities, NaNs, denormals)
// and random ones; MODE1's saturation, truncation and 32-bit rounding at
// random; the DAGs inside internal RAM. A handler that throws on a probe
// (an address it may not touch) reads all and writes none.
constexpr uint32_t kTracked = 0x3bff; // AZ AV AN AC AS AI MN MV MU MI SV SZ SS
struct Flags {
    uint32_t read = kTracked, written = 0;
};
struct NullBus : rt::sharc::Bus {
    uint32_t read(uint32_t) override { return 0; }
    void write(uint32_t, uint32_t) override {}
};
Flags probe_flags(rt::sharc::adsp21062_device &core, uint64_t opcode, uint32_t &seed) {
    auto rnd = [&seed] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    static const uint32_t pool[] = {0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0x3f800000, 0xbf800000, 0x80000000,
                                    0x7f800000, 0xff800000, 0x7fc00000, 0x7fffffff, 0x00000001, 0x007fffff,
                                    0x7f7fffff, 0x00800000, 0x4b000000, 0xcf000000, 0x00010000, 0x0000ffff};
    Flags f{0, kTracked};
    try {
        for (int trial = 0; trial < 384; ++trial) {
            rt::sharc::adsp21062_device::ProbeState in{};
            for (int k = 0; k < 16; ++k) {
                const uint32_t pick = rnd();
                in.r[k] = (pick & 3) == 0 ? rnd() : pool[(pick >> 2) % (sizeof pool / 4)];
                in.i[k] = 0x20000 + (rnd() & 0x1ff); // inside the words probe() clears and hashes
                in.m[k] = uint32_t(int32_t(rnd() % 9) - 4);
            }
            in.astat = rnd() & 0x00ffffff & ~0x780000u;
            in.mode1 = rnd() & (0x2000 | 0x8000 | 0x10000);
            in.stky = 0;
            in.mrf = uint64_t(rnd()) << 32 | rnd();
            in.mrb = uint64_t(rnd()) << 32 | rnd();
            const auto out0 = core.probe(opcode, in);
            for (int bit = 0; bit < 14; ++bit) {
                const uint32_t b = 1u << bit;
                if (!(kTracked & b)) continue;
                auto flipped = in;
                flipped.astat ^= b;
                const auto out1 = core.probe(opcode, flipped);
                if ((out1.astat ^ out0.astat) & b) f.written &= ~b;
                bool other = ((out1.astat ^ out0.astat) & ~b) || out1.stky != out0.stky || out1.mrf != out0.mrf ||
                             out1.mrb != out0.mrb || out1.ram_hash != out0.ram_hash;
                for (int k = 0; k < 16 && !other; ++k) other = out1.r[k] != out0.r[k] || out1.i[k] != out0.i[k];
                if (other) f.read |= b;
            }
        }
    } catch (const std::exception &) {
        return Flags{};
    }
    return f;
}

std::string handler(const std::vector<Op> &ops, uint64_t opcode) {
    const uint16_t op = uint16_t(((opcode >> 39) & 0x1ff) << 7);
    for (const Op &o : ops)
        if ((o.mask & op) == o.bits) return o.name;
    return "sharcop_unimplemented";
}

int32_t sext24(uint64_t v) { return int32_t(uint32_t(v & 0xffffff) << 8) >> 8; }

std::string hx(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%" PRIx64, v);
    return b;
}

} // namespace

int main(int argc, char **argv) {
    std::string ops_path = "src/runtime/sharc/sharcops_table.inc", sharcops_path = "src/runtime/sharc/sharcops.hxx",
                compute_path = "src/runtime/sharc/compute.hxx";
    bool full_steps = false; // every instruction the interpreter's full step (no plain runs)
    // Instructions a chunk (a function and a file). fvipers' fight, PC profile, chunk
    // calls a frame / dispatches per 1000 instructions: 256 4,116 / 109, 512 4,035 /
    // 107, 1024 2,300 / 76, 2048 1,279 / 58 (its hot code calls far away); yet the
    // fight check on the PC 314-318 M instructions/s with 256, 272-277 with 1024,
    // 269-273 with 2048 (bigger functions, worse code), and the Vita's compiler
    // (handlers forced inline) takes 133 s for a 1024 chunk, 183 s and 800 MB for 2048.
    uint32_t chunk = 256;
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2sharcrecomp SHARC_PROGRAM.bin OUT.cpp [--ops SHARCOPS_TABLE.inc]\n");
        return 2;
    }
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) ops_path = argv[++i];
        else if (!std::strcmp(argv[i], "--sharcops") && i + 1 < argc) sharcops_path = argv[++i];
        else if (!std::strcmp(argv[i], "--compute") && i + 1 < argc) compute_path = argv[++i];
        else if (!std::strcmp(argv[i], "--full-steps")) full_steps = true;
        else if (!std::strcmp(argv[i], "--chunk") && i + 1 < argc) chunk = uint32_t(std::strtoul(argv[++i], nullptr, 0));
        else {
            std::fprintf(stderr, "m2sharcrecomp: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!chunk || (chunk & (chunk - 1))) { // the dispatch divides by it: a shift (no divider on the Vita)
        std::fprintf(stderr, "m2sharcrecomp: --chunk must be a power of two\n");
        return 2;
    }
    try {
        std::ifstream f(argv[1], std::ios::binary);
        const std::vector<uint8_t> raw{std::istreambuf_iterator<char>(f), {}};
        if (raw.empty() || raw.size() % 6) throw std::runtime_error(std::string(argv[1]) + ": not a SHARC program (16-bit words, 3 an instruction)");
        const std::vector<Op> ops = read_ops(ops_path);
        constexpr uint32_t base = 0x20000;
        const uint32_t n = uint32_t(raw.size() / 6);
        std::vector<uint64_t> prog(n);
        for (uint32_t k = 0; k < n; ++k)
            for (int w = 0; w < 3; ++w)
                prog[k] |= uint64_t(raw[k * 6 + w * 2] | raw[k * 6 + w * 2 + 1] << 8) << (16 * w);

        // Where loops can end (a do-until's target; laddr has no other source),
        // those of loops on a condition (the ASTAT history), the wait loops.
        std::set<uint32_t> loop_ends, cond_loop_ends, waits;
        std::map<uint32_t, std::set<uint32_t>> loop_tops; // by loop end: where its do-untils loop back to
        for (uint32_t k = 0; k < n; ++k) {
            const std::string h = handler(ops, prog[k]);
            if (h.rfind("sharcop_do_until", 0) != 0) continue;
            const uint32_t end = uint32_t(base + k + sext24(prog[k]));
            loop_ends.insert(end);
            loop_tops[end].insert(base + k + 1);
            if (h == "sharcop_do_until") cond_loop_ends.insert(end); // loop_type 0; the counter ones 1-3
            if (end == base + k + 1 && k + 1 < n && handler(ops, prog[k + 1]) == "sharcop_nop") waits.insert(end);
        }

        size_t handlers = 0;
        const std::string templates = templated_handlers(sharcops_path, handlers);
        // The program in chunks (--chunk, 256 instructions), one function each
        // and one file each, compiled in parallel (the whole program in one
        // function, before the forced inlining: 9 min to optimise on the PC; in
        // one file: over 20 min with the Vita's compiler). Control falls through within a chunk and goes back
        // to run_generated's dispatch between chunks. OUT.cpp holds the core
        // (sharc.cpp, compiled once) and the dispatch; sharc_handlers.h, next
        // to it, what every chunk needs inline: sharcops.hxx's macros and inline
        // helpers, compute.hxx without its two lookup tables (the only
        // definitions it cannot repeat) and the templated handlers.
        const uint32_t chunks = (n + chunk - 1) / chunk;
        const std::string out_path = argv[2];
        const std::string dir = out_path.find('/') == std::string::npos ? "." : out_path.substr(0, out_path.rfind('/'));
        const auto write_file = [](const std::string &path, const std::string &text) {
            std::ofstream f(path);
            if (!(f << text)) throw std::runtime_error("cannot write " + path);
        };
        // Plain instructions: their handlers touch neither the pipeline nor the
        // stacks, loops, interrupts or system registers (sharcops.hxx: outside
        // control flow, the pipeline registers are only read in error messages).
        // They run without the interpreter's per-instruction bookkeeping: the
        // pipeline is written once when a run of them ends, as the last one
        // leaves it (each arrival is linear: daddr, faddr, nfaddr consecutive),
        // and their count is kept in a register. Every other instruction runs
        // the full step, and so do what the opcodes say needs it: the two delay
        // slots of a delayed branch (J bit) and the ends of loops on a condition.
        // The three before those stay plain but shift the ASTAT history
        // (step_loop_end reads it). The ends of counter loops stay plain with
        // step_loop_end's counter part inline: a run ends where one loops back. What no opcode tells
        // (a MODE1 write's latency, from a register write or a status stack pop;
        // a pipeline not linear) is checked where a plain instruction is entered
        // after a full step (plain_ready): the interpreter's steps run until it
        // is gone. The step_pre checks a plain run skips (a delayed IOP write,
        // DMA cycles, interrupts, idling) only change when a full step runs, or
        // with a DMA running: run_generated refuses that.
        const std::set<std::string> plain_names = {
            "sharcop_compute", "sharcop_compute_ureg_dmpm_premod", "sharcop_compute_ureg_dmpm_postmod",
            "sharcop_compute_dm_to_dreg_immmod", "sharcop_compute_dreg_to_dm_immmod", "sharcop_compute_pm_to_dreg_immmod",
            "sharcop_compute_dreg_to_pm_immmod", "sharcop_compute_ureg_to_ureg", "sharcop_compute_dreg_dm_dreg_pm",
            "sharcop_imm_shift", "sharcop_imm_shift_dreg_dmpm", "sharcop_compute_modify", "sharcop_dm_to_ureg_direct",
            "sharcop_dm_to_ureg_indirect", "sharcop_pm_to_ureg_direct", "sharcop_pm_to_ureg_indirect",
            "sharcop_ureg_to_dm_direct", "sharcop_ureg_to_dm_indirect", "sharcop_ureg_to_pm_direct",
            "sharcop_ureg_to_pm_indirect", "sharcop_imm_to_dmpm", "sharcop_imm_to_ureg", "sharcop_modify", "sharcop_nop"};
        const auto writes_sysreg = [&](const std::string &h, uint64_t op) {
            unsigned ureg = 0;
            if (h == "sharcop_compute_ureg_dmpm_premod" || h == "sharcop_compute_ureg_dmpm_postmod" ||
                h == "sharcop_compute_ureg_to_ureg") ureg = unsigned(op >> 23) & 0xff;
            else if (h == "sharcop_imm_to_ureg" || h == "sharcop_dm_to_ureg_direct" || h == "sharcop_dm_to_ureg_indirect" ||
                     h == "sharcop_pm_to_ureg_direct" || h == "sharcop_pm_to_ureg_indirect") ureg = unsigned(op >> 32) & 0xff;
            else return false;
            return (ureg >> 4) == 6 || (ureg >> 4) == 7;
        };
        // Branches, calls, returns and do-untils run inside plain runs too
        // ("pinned", as m2tgprecomp's branches): their handler, MAME's, on the
        // pipeline as the fetch of a linear arrival leaves it (constants), then
        // a direct goto to a constant target where the branch is taken (the
        // count, budget and step_pre first, as at a run's end), the dispatch
        // for a computed one. A delayed branch's two delay slots run in the
        // run (plain, same chunk, no loop end) and the jump follows the second
        // (`pending`: the target CHANGE_PC_DELAYED left in nfaddr). Not pinned:
        // a jump clearing an interrupt (CI bit 24: the status stack, MODE1's
        // latency, interrupt_active), rti, anything else: full steps.
        const std::set<std::string> delaying = delaying_handlers(sharcops_path);
        const std::set<std::string> pinned_names = {
            "sharcop_direct_jump", "sharcop_relative_jump", "sharcop_indirect_jump", "sharcop_relative_jump_compute",
            "sharcop_direct_call", "sharcop_relative_call", "sharcop_indirect_call", "sharcop_relative_call_compute",
            "sharcop_rts", "sharcop_do_until", "sharcop_do_until_counter_imm", "sharcop_do_until_counter_ureg"};
        {
            const auto bodies = handler_bodies(sharcops_path);
            for (const std::string &name : pinned_names) {
                const auto it = bodies.find(name);
                if (it == bodies.end()) throw std::runtime_error(name + " not found upstream");
                const std::string &b = it->second;
                for (const char *w : {"SET_UREG", "idle", "imask", "irq_pending", "systemreg_latency", "dma_", "iop"})
                    if (b.find(w) != std::string::npos) throw std::runtime_error(name + " touches " + w + " upstream: not pinned");
                if (b.find("interrupt_active") != std::string::npos && b.find("if (ci)") == std::string::npos)
                    throw std::runtime_error(name + " changes interrupt_active without the CI bit upstream");
            }
        }
        const auto pinned_op = [&](const std::string &h, uint64_t op) {
            if (!pinned_names.count(h)) return false;
            return h.find("_jump") == std::string::npos || !((op >> 24) & 1); // CI
        };
        const auto has_j = [&](const std::string &h, uint64_t op) { return delaying.count(h) && ((op >> 26) & 1); };
        // A branch's constant target (direct, relative), or 0.
        const auto target_of = [&](const std::string &h, uint32_t a, uint64_t op) -> uint32_t {
            if (h == "sharcop_direct_jump" || h == "sharcop_direct_call") return uint32_t(op & 0xffffff);
            if (h == "sharcop_relative_jump" || h == "sharcop_relative_call") return uint32_t(a + sext24(op));
            return 0;
        };
        std::vector<bool> full(n, false), pinned(n, false), astat_shift(n, false);
        std::vector<uint8_t> why(n, 0); // 1 itself, 2 a delay slot, 4 a loop end (the profile's statistics)
        for (uint32_t k = 0; k < n; ++k) {
            const std::string h = handler(ops, prog[k]);
            if (pinned_op(h, prog[k]) && !loop_ends.count(base + k)) pinned[k] = true;
            else if (!plain_names.count(h) || writes_sysreg(h, prog[k])) full[k] = true, why[k] |= 1;
            if (cond_loop_ends.count(base + k)) full[k] = true, why[k] |= 4;
            if (cond_loop_ends.count(base + k))
                for (uint32_t j = k >= 3 ? k - 3 : 0; j < k; ++j) astat_shift[j] = true;
        }
        // Delay slots, to a fixed point (a branch that cannot keep its slots in
        // the run becomes a full step, and its slots too).
        for (bool changed = true; changed;) {
            changed = false;
            for (uint32_t k = 0; k < n; ++k) {
                const std::string h = handler(ops, prog[k]);
                if (!has_j(h, prog[k])) continue;
                const bool slots_ok = pinned[k] && !full[k] && k + 2 < n && (k + 2) / chunk == k / chunk &&
                                      !full[k + 1] && !full[k + 2] && !pinned[k + 1] && !pinned[k + 2] &&
                                      !loop_ends.count(base + k + 1) && !loop_ends.count(base + k + 2);
                if (slots_ok) continue;
                for (uint32_t j = k; j <= k + 2 && j < n; ++j) {
                    if (j > k) why[j] |= 2;
                    if (!full[j]) full[j] = true, changed = true;
                    if (j == k && !(why[k] & 1)) why[k] |= 1;
                }
            }
        }
        for (uint32_t k = 0; k < n; ++k) if (full[k]) pinned[k] = false;
        if (full_steps) full.assign(n, true), pinned.assign(n, false);

        // The flag analysis (plain instructions), then the liveness within each
        // run of them: every tracked flag is live where a run ends (a full step
        // may read anything: conditions, loop ends, status pushes, ASTAT reads).
        std::vector<Flags> flags(n);
        std::vector<uint32_t> keep(n, kTracked); // the flags an instruction's writes must still compute
        {
            NullBus bus;
            rt::sharc::adsp21062_device core(bus);
            uint32_t seed = 0x2545f491;
            std::map<uint64_t, Flags> seen;
            for (uint32_t k = 0; k < n; ++k) {
                if (full[k] || pinned[k]) continue;
                auto it = seen.find(prog[k]);
                if (it == seen.end()) it = seen.emplace(prog[k], probe_flags(core, prog[k], seed)).first;
                flags[k] = it->second;
            }
            for (uint32_t k = n; k-- > 0;) {
                if (full[k] || pinned[k]) continue;
                const bool run_ends = k + 1 >= n || full[k + 1] || pinned[k + 1] || (k + 1) % chunk == 0;
                const uint32_t live_after = run_ends ? kTracked : (flags[k + 1].read | (keep[k + 1] & ~flags[k + 1].written));
                keep[k] = live_after;
            }
        }
        uint64_t written_bits = 0, dead_bits = 0;
        for (uint32_t k = 0; k < n; ++k)
            if (!full[k] && !pinned[k]) {
                written_bits += std::popcount(flags[k].written);
                dead_bits += std::popcount(flags[k].written & ~keep[k]);
            }
        std::printf("m2sharcrecomp: flags: %llu flag writes in plain instructions, %llu dead (%.0f %%)\n",
                    (unsigned long long)written_bits, (unsigned long long)dead_bits,
                    written_bits ? 100.0 * double(dead_bits) / double(written_bits) : 0.0);
        if (const char *prof = std::getenv("M2SHARC_PROFILE_FILE")) { // the dynamic share, from m2sharcprof's counts
            std::ifstream pf(prof);
            uint32_t addr = 0;
            uint64_t count = 0, w = 0, d = 0, all = 0, by[8] = {}, shifts = 0, pins = 0;
            while (pf >> std::hex >> addr >> std::dec >> count) {
                const uint32_t k = addr - base;
                all += count;
                if (k < n) by[full[k] ? why[k] : 0] += count, shifts += !full[k] && astat_shift[k] ? count : 0, pins += pinned[k] ? count : 0;
                if (k < n && !full[k] && !pinned[k]) w += count * std::popcount(flags[k].written), d += count * std::popcount(flags[k].written & ~keep[k]);
            }
            std::printf("m2sharcrecomp: flags, weighted by the profile: %.2f flag writes an executed instruction, %.2f dead\n",
                        all ? double(w) / double(all) : 0.0, all ? double(d) / double(all) : 0.0);
            const auto pc = [&](uint64_t v) { return all ? 100.0 * double(v) / double(all) : 0.0; };
            std::printf("m2sharcrecomp: steps, weighted by the profile: %.1f %% in plain runs (%.1f %% pinned branches, %.1f %% shifting the ASTAT "
                        "history); full: %.1f %% as control flow/sysreg writes, %.1f %% only as delay slots, %.1f %% only as loop ends, "
                        "%.1f %% for several reasons\n",
                        pc(by[0]), pc(pins), pc(shifts), pc(by[1]), pc(by[2]), pc(by[4]), pc(by[3] + by[5] + by[6] + by[7]));
        }
        uint32_t plain_count = 0;
        for (uint32_t k = 0; k < n; ++k) plain_count += !full[k];

        for (const auto &e : std::filesystem::directory_iterator(dir))
            // (sharc_compact_* and sharc_mixed_*: an older generator's other versions)
            if (e.path().filename().string().rfind("sharc_chunk_", 0) == 0 || e.path().filename().string().rfind("sharc_compact_", 0) == 0 ||
                e.path().filename().string().rfind("sharc_mixed_", 0) == 0)
                std::filesystem::remove(e.path());
        write_file(dir + "/sharc_inline.h",
                   "// Generated by m2sharcrecomp: sharcops.hxx's macros and inline helpers, for\n"
                   "// sharc_handlers.h. From MAME's sources (BSD-3).\n"
                   "#pragma once\n\n// sharcops.hxx's macros\n" + defines(sharcops_path) +
                   "\n// sharcops.hxx's inline helpers\n" + inline_helpers(sharcops_path));
        write_file(dir + "/sharc_handlers.h",
                   "// Generated by m2sharcrecomp: what the chunks of the recompiled SHARC\n"
                   "// program need inline. Derived from MAME's sources (BSD-3) and the game: never commit.\n"
                   "#pragma once\n\n#include \"sharc_inline.h\"\n"
                   "\n// GET_UREG, SET_UREG, SHIFT_OPERATION_IMM, inline\n" + inline_accessors(sharcops_path) +
                   "\n// compute.hxx without its lookup tables\n" + compute_inline(compute_path) +
                   "\n// sharcops.hxx's handlers, templates on the opcode\n" + templates);
        // The chunks: MAME's handlers as templates on each opcode, the step
        // pieces inline (a shared-handler version, a few bytes an instruction,
        // ran 26 % slower on the Vita: its code cache was not the limit).
        for (uint32_t c = 0; c < chunks; ++c) {
            const uint32_t first = c * chunk, last = std::min(n, first + chunk);
            std::ostringstream o;
            o << "// Generated by m2sharcrecomp: the recompiled SHARC program, chunk " << c << ".\n"
                 "// Derived from the game: never commit.\n"
                 "#include \"runtime/sharc/sharc.h\"\n\n"
                 "#include <algorithm>\n#include <bit>\n#include <cmath>\n#include <cstring>\n#include <iterator>\n#include <limits>\n\n"
                 "namespace rt::sharc {\n\n"
                 "#include \"runtime/sharc/sharcinternal.ipp\"\n"
                 "#include \"runtime/sharc/sharc_step.h\"\n"
                 "#include \"sharc_handlers.h\"\n"
                 "\n// 0 when control leaves the chunk (step_pre done), 1 when the SHARC waits,\n"
                 "// 2 when the budget ran out, 3 when the next instruction needs the\n"
                 "// interpreter's step (plain_ready).\n"
                 "template <>\nint adsp21062_device::run_chunk<" << c << ">(uint64_t &done)\n{\n"
                 "\tuint32_t plain = 0; // plain instructions run since the last count\n"
                 "\tuint32_t pending = 0; // a pinned delayed branch's target, taken after its second delay slot\n"
                 "\tfor (;;) {\n"
                 "\t\tif (!plain_ready()) return 3;\n"
                 "\t\tM2SHARC_EV(dispatches);\n"
                 "\t\tpending = 0;\n"
                 "\t\tswitch (m_core->daddr) {\n";
            for (uint32_t k = first; k < last; ++k) o << "\t\tcase " << hx(base + k) << ": goto L_" << std::hex << base + k << std::dec << ";\n";
            o << "\t\tdefault: return 0;\n"
                 "\t\t}\n";
            for (uint32_t k = first; k < last; ++k) {
                const uint32_t a = base + k;
                const std::string call = handler(ops, prog[k]) + "_t<" + hx(prog[k]) + "ull>();";
                o << "\tL_" << std::hex << a << std::dec << ":\n"
                  << "\t\tM2SHARC_COUNT(" << k << ");\n";
                if (!full[k]) {
                    const std::string h = handler(ops, prog[k]);
                    if (pinned[k]) // the pipeline as the fetch leaves it (a linear arrival: plain_ready), the full step's ASTAT history
                        o << "\t\tm_core->pc = " << hx(a) << "; m_core->daddr = " << hx(a + 1) << "; m_core->faddr = " << hx(a + 2)
                          << "; m_core->nfaddr = " << hx(a + 3) << ";\n";
                    if (astat_shift[k] || pinned[k])
                        o << "\t\tm_core->astat_old_old_old = m_core->astat_old_old; m_core->astat_old_old = m_core->astat_old; "
                             "m_core->astat_old = m_core->astat;\n";
                    const bool loop_end = loop_ends.count(a) != 0;
                    if (loop_end) // step_loop_end, counter loops (cond_loop_ends are full); the pipeline as CHANGE_PC leaves it
                        o << "\t\t{\n"
                             "\t\tbool back = false;\n"
                             "\t\tif (!(m_core->stky & LSEM) && m_core->laddr.addr == " << hx(a) << ") {\n"
                             "\t\t\tif (--m_core->curlcntr == 0) { POP_LOOP(); POP_PC(); }\n"
                             "\t\t\telse back = true;\n"
                             "\t\t}\n";
                    o << "\t\t" << call << "\n"
                      << "\t\t++plain;\n";
                    if (loop_end) {
                        o << "\t\tif (back) {\n"
                             "\t\t\tconst uint32_t top = TOP_PC();\n"
                             "\t\t\tM2SHARC_EV(loop_backs);\n"
                             "\t\t\tm_core->pc = top; m_core->daddr = top; m_core->faddr = top + 1; m_core->nfaddr = top + 2;\n"
                             "\t\t\tm_instructions += plain; done += plain; plain = 0;\n"
                             "\t\t\tif (done >= m_budget) return 2;\n"
                             "\t\t\tif (!step_pre()) return 1;\n";
                        const auto &tops = loop_tops.at(a);
                        if (tops.size() == 1 && *tops.begin() - base >= first && *tops.begin() - base < last)
                            o << "\t\t\tif (m_core->daddr == " << hx(*tops.begin()) << ") goto L_" << std::hex << *tops.begin() << std::dec << ";\n";
                        o << "\t\t\tcontinue;\n"
                             "\t\t}\n"
                             "\t\t}\n";
                    }
                    // Where a pinned branch is taken: the count, budget and step_pre as at a
                    // run's end (CHANGE_PC wrote the pipeline), then the target.
                    const auto taken = [&](uint32_t target, const char *event) {
                        o << "\t\t\tM2SHARC_EV(" << event << ");\n"
                             "\t\t\tm_instructions += plain; done += plain; plain = 0;\n"
                             "\t\t\tif (done >= m_budget) return 2;\n"
                             "\t\t\tif (!step_pre()) return 1;\n";
                        if (target >= base && target - base >= first && target - base < last)
                            o << "\t\t\tif (m_core->daddr == " << hx(target) << ") goto L_" << std::hex << target << std::dec << ";\n";
                        o << "\t\t\tcontinue;\n"
                             "\t\t}\n";
                    };
                    if (pinned[k] && has_j(h, prog[k]))
                        o << "\t\tif (m_core->nfaddr != " << hx(a + 3) << ") pending = m_core->nfaddr;\n";
                    else if (pinned[k] && h.rfind("sharcop_do_until", 0) != 0) {
                        o << "\t\tif (m_core->daddr != " << hx(a + 1) << ") {\n";
                        taken(target_of(h, a, prog[k]), "taken");
                    }
                    if (k >= 2 && pinned[k - 2] && has_j(handler(ops, prog[k - 2]), prog[k - 2])) {
                        // The second delay slot: the pipeline as the fetch of the target finds it.
                        o << "\t\tif (pending) {\n"
                             "\t\t\tconst uint32_t t = pending;\n"
                             "\t\t\tpending = 0;\n"
                             "\t\t\tm_core->pc = " << hx(a) << "; m_core->daddr = t; m_core->faddr = t + 1; m_core->nfaddr = t + 2;\n";
                        taken(target_of(handler(ops, prog[k - 2]), a - 2, prog[k - 2]), "slot_taken");
                    }
                    if (k + 1 < last && !full[k + 1]) continue; // the run goes on
                    // The run ends: the pipeline as this instruction leaves it, its count.
                    o << "\t\tM2SHARC_EV(run_ends);\n"
                      << "\t\tm_core->pc = " << hx(a) << "; m_core->daddr = " << hx(a + 1) << "; m_core->faddr = " << hx(a + 2)
                      << "; m_core->nfaddr = " << hx(a + 3) << ";\n"
                      << "\t\tm_instructions += plain; done += plain; plain = 0;\n"
                      << "\t\tif (done >= m_budget) return 2;\n"
                      << "\t\tif (!step_pre()) return 1;\n";
                } else {
                    o << "\t\tM2SHARC_EV(full_steps);\n"
                      << "\t\tstep_fetch_gen(" << hx(prog[k]) << "ull);\n";
                    if (loop_ends.count(a)) o << "\t\tstep_loop_end();\n";
                    o << "\t\t" << call << "\n"
                      << "\t\tstep_after();\n";
                    if (waits.count(a)) o << "\t\tif (m_core->daddr == " << hx(a) << ") return 1; // polling its flags\n";
                    o << "\t\t++done;\n\t\tif (done >= m_budget) return 2;\n\t\tif (!step_pre()) return 1;\n";
                }
                if (k + 1 < last) {
                    // From a full step into a plain instruction: only in a state it can run in.
                    const bool guard = full[k] && !full[k + 1];
                    o << "\t\tif (m_core->daddr == " << hx(a + 1) << (guard ? " && plain_ready()" : "") << ") goto L_" << std::hex << a + 1
                      << std::dec << ";\n";
                }
                o << "\t\tcontinue;\n";
            }
            o << "\t}\n}\n\n} // namespace rt::sharc\n";
            char name[40];
            std::snprintf(name, sizeof name, "/sharc_chunk_%03u.cpp", c);
            write_file(dir + name, o.str());
        }
        std::ostringstream o;
        o << "// Generated by m2sharcrecomp from the SHARC program the i960 uploads (Model 2B):\n"
             "// the core and the dispatch over the chunks (sharc_chunk_*).\n"
             "// Derived from the game: never commit.\n"
             "#include \"runtime/sharc/sharc.cpp\"\n\n"
             "namespace rt::sharc {\n\n"
             "const uint32_t sharc_program_crc32 = "
          << hx(crc32(raw.data(), raw.size())) << "u;\nconst uint32_t sharc_program_instructions = " << n
          << ";\n\nnamespace {\nconstexpr uint64_t kProgram[" << n << "] = {\n";
        for (uint32_t k = 0; k < n; ++k) o << "    " << hx(prog[k]) << "ull,\n";
        o << "};\n} // namespace\n\n";
        // The profile's view of the program (sharc_profile.h): each instruction's handler and kind.
        o << "#ifdef M2_SHARC_PROFILE\nconst uint32_t sharc_gen_count = " << n << ";\nconst char *const sharc_gen_names[" << n << "] = {\n";
        for (uint32_t k = 0; k < n; ++k) o << "    \"" << handler(ops, prog[k]).substr(8) << "\",\n"; // without "sharcop_"
        o << "};\nconst uint8_t sharc_gen_kinds[" << n << "] = {";
        for (uint32_t k = 0; k < n; ++k) o << (k % 32 ? "" : "\n    ") << (full[k] ? "SharcFull" : pinned[k] ? "SharcPinned" : "SharcPlain") << ",";
        o << "\n};\n"
             "// A run_generated exit: its size, why (waiting or the budget).\n"
             "#define M2SHARC_EXIT(w) do { M2SHARC_SIZE(done); ++((w) ? sharc_profile.exit_wait : sharc_profile.exit_budget); return (w); } while (0)\n"
             "#else\n#define M2SHARC_EXIT(w) return (w)\n#endif\n\n";
        for (uint32_t c = 0; c < chunks; ++c)
            o << "template <> int adsp21062_device::run_chunk<" << c << ">(uint64_t &done);\n";
        o << "\nbool adsp21062_device::run_generated(uint64_t budget)\n{\n"
             "\tif (m_halted) return true;\n"
             "\tif (!m_gen_checked) {\n"
             "\t\tfor (uint32_t i = 0; i < " << n << "; ++i)\n"
             "\t\t\tif (m_program.read_qword(" << hx(base) << " + i) != kProgram[i])\n"
             "\t\t\t\tthrow SharcFatal(emu_fatalerror::fmt(\"SHARC program memory at %05x is not the recompiled program\", " << hx(base) << " + i));\n"
             "\t\tm_gen_checked = true;\n"
             "\t\tm_gen_end = " << hx(base + n) << ";\n"
             "\t}\n"
             "\tuint64_t done = 0;\n"
             "\tm_budget = budget; // end_run() lowers it\n"
             "\tM2SHARC_EV(gen_calls);\n"
             "\t// Plain runs skip step_pre and the DMA cycle: not while a DMA or a delayed IOP write is pending.\n"
             "\tif ((m_core->dma_status & 0xc0) || m_iop_delay > 0)\n"
             "\t\tthrow SharcFatal(\"SHARC: a DMA or delayed IOP write while the recompiled program runs\");\n"
             "\tif (!step_pre()) M2SHARC_EXIT(true);\n"
             "\tfor (;;) {\n"
             "\t\tint r;\n"
             "\t\tM2SHARC_EV(chunk_calls);\n"
             "\t\tswitch ((m_core->daddr - " << hx(base) << ") / " << chunk << "u) {\n";
        for (uint32_t c = 0; c < chunks; ++c) o << "\t\tcase " << c << ": r = run_chunk<" << c << ">(done); break;\n";
        o << "\t\tdefault: throw SharcFatal(emu_fatalerror::fmt(\"SHARC: no recompiled code at %05x\", m_core->daddr));\n"
             "\t\t}\n"
             "\t\tif (r == 1) M2SHARC_EXIT(true);\n"
             "\t\tif (r == 2) M2SHARC_EXIT(false);\n"
             "\t\tif (r == 3) { // a delay slot entered from elsewhere, a MODE1 write's latency: the interpreter's steps\n"
             "\t\t\tM2SHARC_EV(fallback_entries);\n"
             "\t\t\tdo {\n"
             "\t\t\t\tM2SHARC_EV(fallback_steps);\n"
             "\t\t\t\tstep_body();\n"
             "\t\t\t\tif (++done >= m_budget) M2SHARC_EXIT(false);\n"
             "\t\t\t\tif (!step_pre()) M2SHARC_EXIT(true);\n"
             "\t\t\t} while (!plain_ready());\n"
             "\t\t}\n"
             "\t\tif (m_core->daddr - " << hx(base) << " >= " << n << "u)\n"
             "\t\t\tthrow SharcFatal(emu_fatalerror::fmt(\"SHARC: no recompiled code at %05x\", m_core->daddr));\n"
             "\t}\n}\n\n} // namespace rt::sharc\n";
        write_file(out_path, o.str());
        std::printf("m2sharcrecomp: %u instructions (%u plain), %zu loop ends, %zu wait loops, %zu handlers templated -> %s and %u chunks\n", n,
                    plain_count, loop_ends.size(), waits.size(), handlers, argv[2], chunks);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2sharcrecomp: %s\n", e.what());
        return 2;
    }
}
