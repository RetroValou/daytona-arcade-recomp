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
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
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
            o << "template <uint32_t OPCODE>\nvoid adsp21062_device::COMPUTE_t()\n" << replace_word(body, "opcode", "OPCODE", false) << "\n\n";
        } else {
            if (!(*i)[3].str().empty()) throw std::runtime_error(name + " takes arguments upstream");
            body = templated_compute_calls(replace_word(body, "m_core->opcode", "OPCODE", true));
            for (const char *f : {"GET_UREG", "SET_UREG", "SHIFT_OPERATION_IMM"})
                body = replace_word(body, f, std::string(f) + "_i", false);
            o << "template <uint64_t OPCODE>\n" << (*i)[1] << "void adsp21062_device::" << name << "_t()\n" << body << "\n\n";
        }
        ++count;
    }
    if (count < 40) throw std::runtime_error(path + ": only " + std::to_string(count) + " handlers found");
    return o.str();
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
        out += t.substr(start, match_brace(t, open) - start + 1) + "\n\n";
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
        out += "inline " + m[1].str() + " adsp21062_device::" + name + "_i(" + m[2].str() + ")\n" + body + "\n\n";
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
    return std::regex_replace(t, table, "");
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
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2sharcrecomp SHARC_PROGRAM.bin OUT.cpp [--ops SHARCOPS_TABLE.inc]\n");
        return 2;
    }
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) ops_path = argv[++i];
        else if (!std::strcmp(argv[i], "--sharcops") && i + 1 < argc) sharcops_path = argv[++i];
        else if (!std::strcmp(argv[i], "--compute") && i + 1 < argc) compute_path = argv[++i];
        else if (!std::strcmp(argv[i], "--full-steps")) full_steps = true;
        else {
            std::fprintf(stderr, "m2sharcrecomp: unknown option %s\n", argv[i]);
            return 2;
        }
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

        // Where loops can end (a do-until's target) and the wait loops.
        std::set<uint32_t> loop_ends, waits;
        for (uint32_t k = 0; k < n; ++k) {
            const std::string h = handler(ops, prog[k]);
            if (h.rfind("sharcop_do_until", 0) != 0) continue;
            const uint32_t end = uint32_t(base + k + sext24(prog[k]));
            loop_ends.insert(end);
            if (end == base + k + 1 && k + 1 < n && handler(ops, prog[k + 1]) == "sharcop_nop") waits.insert(end);
        }

        size_t handlers = 0;
        const std::string templates = templated_handlers(sharcops_path, handlers);
        // The program in chunks of 256 instructions, one function each and one
        // file each, compiled in parallel (one function: 9 min to optimise on
        // the PC; one file of chunks: 1 min 45 s on the PC, over 20 min with the
        // Vita's compiler). Control falls through within a chunk and goes back
        // to run_generated's dispatch between chunks. OUT.cpp holds the core
        // (sharc.cpp, compiled once) and the dispatch; sharc_handlers.h, next
        // to it, what every chunk needs inline: sharcops.hxx's macros and inline
        // helpers, compute.hxx without its two lookup tables (the only
        // definitions it cannot repeat) and the templated handlers.
        constexpr uint32_t chunk = 256;
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
        // the full step, and so do the two after one (delay slots, MODE1's
        // write latency) and the three before a loop end (the ASTAT history
        // step_loop_end reads). The step_pre checks a plain run skips (a
        // delayed IOP write, DMA cycles, interrupts, idling) only change when a
        // full step runs, or with a DMA running: run_generated refuses that.
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
        std::vector<bool> full(n, false);
        for (uint32_t k = 0; k < n; ++k) {
            const std::string h = handler(ops, prog[k]);
            if (!plain_names.count(h) || writes_sysreg(h, prog[k])) {
                full[k] = true;
                if (k + 1 < n) full[k + 1] = true;
                if (k + 2 < n) full[k + 2] = true;
            }
        }
        for (uint32_t k = 0; k < n; ++k)
            if (loop_ends.count(base + k))
                for (uint32_t j = k >= 3 ? k - 3 : 0; j <= k; ++j) full[j] = true;
        if (full_steps) full.assign(n, true);
        uint32_t plain_count = 0;
        for (uint32_t k = 0; k < n; ++k) plain_count += !full[k];

        for (const auto &e : std::filesystem::directory_iterator(dir))
            if (e.path().filename().string().rfind("sharc_chunk_", 0) == 0) std::filesystem::remove(e.path());
        write_file(dir + "/sharc_handlers.h",
                   "// Generated by m2sharcrecomp: what the recompiled SHARC program's chunks need\n"
                   "// inline. Derived from MAME's sources (BSD-3) and the game: never commit.\n"
                   "#pragma once\n\n// sharcops.hxx's macros\n" + defines(sharcops_path) +
                   "\n// sharcops.hxx's inline helpers\n" + inline_helpers(sharcops_path) +
                   "\n// GET_UREG, SET_UREG, SHIFT_OPERATION_IMM, inline\n" + inline_accessors(sharcops_path) +
                   "\n// compute.hxx without its lookup tables\n" + compute_inline(compute_path) +
                   "\n// sharcops.hxx's handlers, templates on the opcode\n" + templates);
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
                 "#include \"sharc_handlers.h\"\n\n"
                 "// 0 when control leaves the chunk (step_pre done), 1 when the SHARC waits,\n"
                 "// 2 when the budget ran out.\n"
                 "template <>\nint adsp21062_device::run_chunk<" << c << ">(uint64_t &done, uint64_t budget)\n{\n"
                 "\tuint32_t plain = 0; // plain instructions run since the last count\n"
                 "\tfor (;;) {\n"
                 "\t\tswitch (m_core->daddr) {\n";
            for (uint32_t k = first; k < last; ++k) o << "\t\tcase " << hx(base + k) << ": goto L_" << std::hex << base + k << std::dec << ";\n";
            o << "\t\tdefault: return 0;\n"
                 "\t\t}\n";
            for (uint32_t k = first; k < last; ++k) {
                const uint32_t a = base + k;
                o << "\tL_" << std::hex << a << std::dec << ":\n";
                if (!full[k]) {
                    o << "\t\t" << handler(ops, prog[k]) << "_t<" << hx(prog[k]) << "ull>();\n"
                      << "\t\t++plain;\n";
                    if (k + 1 < last && !full[k + 1]) continue; // the run goes on
                    // The run ends: the pipeline as this instruction leaves it, its count.
                    o << "\t\tm_core->pc = " << hx(a) << "; m_core->daddr = " << hx(a + 1) << "; m_core->faddr = " << hx(a + 2)
                      << "; m_core->nfaddr = " << hx(a + 3) << ";\n"
                      << "\t\tm_instructions += plain; done += plain; plain = 0;\n";
                } else {
                    o << "\t\tstep_fetch(" << hx(prog[k]) << "ull);\n";
                    if (loop_ends.count(a)) o << "\t\tstep_loop_end();\n";
                    o << "\t\t" << handler(ops, prog[k]) << "_t<" << hx(prog[k]) << "ull>();\n"
                      << "\t\tstep_after();\n";
                    if (waits.count(a)) o << "\t\tif (m_core->daddr == " << hx(a) << ") return 1; // polling its flags\n";
                    o << "\t\t++done;\n";
                }
                o << "\t\tif (done >= budget) return 2;\n"
                  << "\t\tif (!step_pre()) return 1;\n";
                if (k + 1 < last) o << "\t\tif (m_core->daddr == " << hx(a + 1) << ") goto L_" << std::hex << a + 1 << std::dec << ";\n";
                o << "\t\tcontinue;\n";
            }
            o << "\t}\n}\n\n} // namespace rt::sharc\n";
            char name[32];
            std::snprintf(name, sizeof name, "/sharc_chunk_%03u.cpp", c);
            write_file(dir + name, o.str());
        }
        std::ostringstream o;
        o << "// Generated by m2sharcrecomp from the SHARC program the i960 uploads (Model 2B):\n"
             "// the core and the dispatch over the chunks (sharc_chunk_*.cpp).\n"
             "// Derived from the game: never commit.\n"
             "#include \"runtime/sharc/sharc.cpp\"\n\n"
             "namespace rt::sharc {\n\n"
             "const uint32_t sharc_program_crc32 = "
          << hx(crc32(raw.data(), raw.size())) << "u;\nconst uint32_t sharc_program_instructions = " << n
          << ";\n\nnamespace {\nconstexpr uint64_t kProgram[" << n << "] = {\n";
        for (uint32_t k = 0; k < n; ++k) o << "    " << hx(prog[k]) << "ull,\n";
        o << "};\n} // namespace\n\n";
        for (uint32_t c = 0; c < chunks; ++c)
            o << "template <> int adsp21062_device::run_chunk<" << c << ">(uint64_t &done, uint64_t budget);\n";
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
             "\t// Plain runs skip step_pre and the DMA cycle: not while a DMA or a delayed IOP write is pending.\n"
             "\tif ((m_core->dma_status & 0xc0) || m_iop_delay > 0)\n"
             "\t\tthrow SharcFatal(\"SHARC: a DMA or delayed IOP write while the recompiled program runs\");\n"
             "\tif (!step_pre()) return true;\n"
             "\tfor (;;) {\n"
             "\t\tint r;\n"
             "\t\tswitch ((m_core->daddr - " << hx(base) << ") >> 8) {\n";
        for (uint32_t c = 0; c < chunks; ++c) o << "\t\tcase " << c << ": r = run_chunk<" << c << ">(done, budget); break;\n";
        o << "\t\tdefault: throw SharcFatal(emu_fatalerror::fmt(\"SHARC: no recompiled code at %05x\", m_core->daddr));\n"
             "\t\t}\n"
             "\t\tif (r == 1) return true;\n"
             "\t\tif (r == 2) return false;\n"
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
