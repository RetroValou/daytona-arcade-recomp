// m2recomp: the i960 static recompiler (design doc, i960 static recompiler).
//
//   m2recomp PROGRAM.bin OUTDIR [--seeds FILE]... [--hooks FILE] [--chunk N] [--fast_inaccuracy]
//            [--profile FILE [--hot-share PERCENT]]
//
// --hooks: "ADDRESS name" lines; the generated code calls rt::hook_<name>(c)
// just before the instruction at ADDRESS (enhancements: see
// src/runtime/enhance.h). A hook that is off does nothing.
//
// Decodes every instruction reachable from the boot record plus the seed
// addresses (src/i960/reach) and emits native C++: each instruction's
// semantics inline, with registers, literals and effective-address forms
// resolved here, at recompile time. Known branch targets are direct gotos; an
// indirect transfer (bx, callx, ret, an interrupt) re-dispatches on the
// address. The semantics are MAME's (src/refcore/i960_ref.cpp, src/runtime/cpu.cpp), expression
// for expression, so the lockstep harness can hold the output to MAME.
//
// --fast_inaccuracy (off by default) groups the speed options for slow targets
// (PS Vita). Without it the output is the exact one the lockstep and trace
// comparisons against MAME need. With it:
//
// 1. Block bookkeeping: the lockstep bookkeeping (IP store, interrupt and
// event check, instruction count) is emitted once per basic block instead of
// before every instruction. Each instruction's semantics and the instruction
// count are unchanged, but an interrupt or a scheduled event is taken at the
// next block start, a few instructions later than MAME takes it, so the output
// no longer matches MAME's interrupt log: the lockstep and trace comparisons
// need the default (exact) output. Faster, for slow targets (PS Vita).
// A block starts at every direct branch target, after every transfer of
// control, at every seed (boot entries, interrupt handlers, harvested indirect
// targets) and at every hook address. An indirect jump into the middle of a
// block (an address no seed names) still lands on the instruction's label;
// its switch case corrects the count for the instructions skipped. The
// runtime applies an event only at its exact count, which a block can step
// over: the generated block_boundary() replays such an event at its own count
// at the next block start, then restores the true count.
//
// 2. Direct chaining: gen::run moves from chunk to chunk itself (binary search
// on the chunk ranges) instead of returning to GameLoop, which re-checked
// has_code and re-scanned every chunk on each transfer between chunks (~1,360
// per frame in a race, 0.68 us each on the Vita: platform/vita perf.log, CPU-2).
// It returns as before when the frame ends (the lockstep is finished) or when
// control reaches an address no chunk has code for, so GameLoop still reports
// that address. Same instructions in the same order: this part alone keeps
// the output exact.
//
// Block profile: every basic block starts with M2_PROF(index) (after its
// lockstep check). It compiles to nothing except in m2run, built with
// M2_PROFILE, where m2run --profile counts each block's executions and the
// transfers between blocks (scripts/profile.py runs every scripts/inputs
// scenario and writes profiles/<set>.txt). The block tables (gen::block_addr)
// are emitted for M2_PROFILE builds only.
//
// --profile FILE (profiles/<set>.txt; scripts/recompile.py --profile and
// scripts/build_vita.py --profile pass it, off by default) uses such a profile,
// without changing any instruction or the order the lockstep sees them in (the
// output stays exact):
//
// 1. Hot chunks: the hottest chains (runs of instructions that fall through
// one into the next, kept whole), by instructions executed per instruction of
// code, until they cover --hot-share percent (default 90) of the executed
// instructions, are taken out of address order and clustered, wherever they
// are in the program: chains executed one after the other (caller, callee and
// return site, a loop and what it calls) are merged greedily, heaviest
// transfer first, up to `--chunk` instructions. The clusters fill the first
// chunks, hottest first. A transfer inside a chunk is a goto (or the chunk's
// own switch, for ret and bx) instead of a return to gen::run and a chunk
// lookup. (A hot chunk's switch spans scattered addresses: the compiler makes
// it a compare tree over several jump tables instead of one table.)
// 2. The rest stays in address order, cut by dynamic programming where the
// fewest of the profile's transfers cross, at most `--chunk` instructions a
// chunk. These chunks have holes where hot chains were taken out.
// gen::run finds the chunk of an address by binary search on the instruction
// index (kChunkOf). fast_gen.py (Dreamcast, Vita) keeps that table as it is.
//
// There is no fallback: an instruction without a native template fails the
// recompile, listed by address and mnemonic. At run time, control reaching an
// address that was not recompiled is a hard error naming it (add it to the
// seeds). Nothing is ever interpreted. The output is portable C++20 (no
// inline assembly, no compiler builtins) for x86-64 and ARM64 alike.
//
// The output is derived from the game: write it under build/ (git-ignored),
// never commit it (rules.md rule 6).
//
// Seeds files: one hex address per line, or the MAME harvest format
// ("kind from target count"; the target column is used).

#include "i960/decode.h"
#include "i960/reach.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using i960::Insn;

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08xu", v);
    return b;
}
std::string lbl(uint32_t a) {
    char b[16];
    std::snprintf(b, sizeof b, "L_%08x", a);
    return b;
}
std::string R(unsigned n) { return "R[" + std::to_string(n) + "]"; }
std::string lit(unsigned n) { return std::to_string(n) + "u"; }

// Operand forms, exactly as MAME's get_1_ri / get_2_ri / get_1_ci / get_2_ci.
std::string s1(const Insn &in) { return in.m1 ? lit(in.src1) : R(in.src1); }
std::string s2(const Insn &in) { return in.m2 ? lit(in.src2) : R(in.src2); }
std::string c1(const Insn &in) { return in.m1 ? lit(in.src1) : R(in.src1); }
std::string c2(const Insn &in) { return R(in.src2); }

// MAME get_ea, with the displacement word and IP-relative base known now.
std::string ea(const Insn &in) {
    const std::string ab = R(in.src2), ix = R(in.index);
    const std::string scaled = "(" + ix + " << " + std::to_string(in.scale) + ")";
    switch (in.ea) {
    case i960::EaMode::Offset: return hex(in.offset);
    case i960::EaMode::AbaseOffset: return "(" + ab + " + " + hex(in.offset) + ")";
    case i960::EaMode::Abase: return ab;
    case i960::EaMode::IpDisp: return hex(in.addr + 8 + in.disp); // disp + address after the displacement word
    case i960::EaMode::AbaseIndex: return "(" + ab + " + " + scaled + ")";
    case i960::EaMode::Disp: return hex(in.disp);
    case i960::EaMode::AbaseDisp: return "(" + hex(in.disp) + " + " + ab + ")";
    case i960::EaMode::IndexDisp: return "(" + hex(in.disp) + " + " + scaled + ")";
    case i960::EaMode::AbaseIndexDisp: return "(" + hex(in.disp) + " + " + ab + " + " + scaled + ")";
    default: return "";
    }
}

// --fast_inaccuracy: the check at a block start. The lockstep runtime applies an event
// (a frame-clock probe, a UART shift) when the count equals the event's; a
// block adds its whole length at its end and can step over it. Such an event
// is applied here at its own count, then the true count is restored: the
// instruction total stays exact, the event runs a few instructions late.
const char *const kBlockBoundary = R"(namespace {
bool block_events(rt::Lockstep &ls) {
    const uint64_t now = ls.count;
    while (ls.next_count < now && ls.next_count < ls.end_count) { // stepped over
        const uint64_t at = ls.next_count;
        ls.count = at;
        const bool taken = ls.boundary(); // applies the events at their count
        ls.count = now;
        if (taken) return true;           // an interrupt was taken: re-dispatch
        if (ls.next_count == at) break;   // nothing consumed (never expected)
    }
    return ls.boundary();
}
inline bool block_boundary(rt::Lockstep &ls) {
    if (ls.count < ls.next_count && ls.count < ls.end_count) [[likely]] return false;
    return block_events(ls);
}
} // namespace

)";

// Enhancement hooks: address -> name (rt::hook_<name>), from --hooks.
std::map<uint32_t, std::string> g_hooks;

// --profile: executions per basic block and transfers between blocks, by block
// start address (m2run --profile, scripts/profile.py). Addresses the current
// seeds do not start a block at are ignored.
struct Profile {
    std::map<uint32_t, uint64_t> blocks;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> edges;
};

Profile load_profile(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "m2recomp: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    Profile p;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string kind, a, b, n;
        ls >> kind >> a >> b >> n;
        if (kind == "block") // block ADDRESS INSTRUCTIONS COUNT
            p.blocks[uint32_t(std::strtoul(a.c_str(), nullptr, 16))] += std::strtoull(n.c_str(), nullptr, 10);
        else if (kind == "edge") // edge FROM TO COUNT
            p.edges[{uint32_t(std::strtoul(a.c_str(), nullptr, 16)), uint32_t(std::strtoul(b.c_str(), nullptr, 16))}] +=
                std::strtoull(n.c_str(), nullptr, 10);
    }
    return p;
}

struct Emitter {
    const std::set<uint32_t> *chunk_addrs; // instructions emitted in this chunk
    std::ostringstream o;
    uint64_t native = 0;
    std::vector<std::string> *missing; // instructions without a native template

    // --fast_inaccuracy block state, set by the caller before each emit().
    bool blocks = false;                   // block bookkeeping
    bool leader = true;                    // first instruction of a block (always, without --fast_inaccuracy)
    unsigned block_end = 0;                // a block's last instruction: the block's length
    std::set<uint32_t> *targets = nullptr; // analysis pass: records every direct transfer target
    bool exits = false;                    // after emit(): the instruction has an explicit exit

    long prof_block = -1; // >= 0: the instruction starts this basic block (M2_PROF), set before each emit()

    // Transfer to a known address: a goto when it is in this chunk.
    std::string jump(uint32_t t) const {
        if (targets) targets->insert(t);
        if (chunk_addrs->count(t)) return "goto " + lbl(t) + ";";
        return "{ c.m_IP = " + hex(t) + "; goto dispatch; }";
    }

    void line(const std::string &s) { o << "    " << s << "\n"; }

    // Emits one instruction. Returns false when control never falls through
    // to the next address.
    bool emit(const Insn &in) {
        const uint32_t pc = in.addr, next = pc + in.length;
        const std::string mn = in.op->mnem;
        const uint16_t code = in.fmt == i960::Format::Reg ? in.op->code : uint16_t(in.word >> 24);
        char comment[96];
        std::snprintf(comment, sizeof comment, "// %08x: %s", pc, i960::format_mame(in).c_str());
        o << lbl(pc) << ": " << comment << "\n";
        // Lockstep: MAME's interrupt events happen before this instruction
        // (--fast_inaccuracy: before the block's first instruction only).
        if (leader) {
            line("c.m_IP = " + hex(pc) + ";");
            line(blocks ? "if (block_boundary(ls)) goto dispatch;" : "if (ls.boundary()) goto dispatch;");
            if (const auto h = g_hooks.find(pc); h != g_hooks.end()) line("rt::hook_" + h->second + "(c);");
        }
        if (prof_block >= 0) line("M2_PROF(" + std::to_string(prof_block) + ");");

        std::string body, exit;   // body runs before ++count; exit transfers control
        bool falls = true;
        const std::string d = R(in.dst);
        auto set = [&](const std::string &v) { body += d + " = " + v + ";"; };
        auto two = [&](const std::string &v) { // read both sources first, as MAME does
            body += "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; " + d + " = " + v + "; }";
        };
        const uint32_t tgt = in.target, tgt_masked = in.target & ~3u; // bxx/bxx_s mask the target, b/bal/call/bbc/bbs do not

        switch (in.fmt) {
        case i960::Format::Ctrl:
            switch (code) {
            case 0x08: exit = jump(tgt); falls = false; break;                                   // b
            case 0x09: body = "c.m_IP = " + hex(next) + "; c.do_call(" + hex(tgt) + ", 0, R[1]);"; // call
                       exit = jump(tgt); falls = false; break;
            case 0x0a: body = "c.do_ret();"; exit = "goto dispatch;"; falls = false; break;       // ret
            case 0x0b: body = "R[30] = " + hex(next) + ";"; exit = jump(tgt); falls = false; break; // bal
            case 0x10: exit = "if (!(AC & 7)) " + jump(tgt); break;                                // bno (unmasked, as MAME)
            case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
                exit = "if (AC & " + std::to_string(code & 7) + ") " + jump(tgt_masked); break;
            default: return unsupported(in);                                                     // faults
            }
            break;

        case i960::Format::Cobr:
            if (code >= 0x20 && code <= 0x27) { // test*
                const std::string cond = code == 0x20 ? "!(AC & 7)" : "(AC & " + std::to_string(code & 7) + ")";
                body = R(in.src1) + " = " + cond + " ? 1u : 0u;";
            } else if (code == 0x30 || code == 0x37) { // bbc / bbs
                body = "{ const uint32_t t1 = " + c1(in) + " & 0x1f, t2 = " + c2(in) + "; cond = " +
                       (code == 0x37 ? "(t2 & (1u << t1)) != 0" : "!(t2 & (1u << t1))") +
                       "; AC = cond ? ((AC & ~7u) | 2u) : (AC & ~7u); }";
                exit = "if (cond) " + jump(tgt);
            } else if (code >= 0x31 && code <= 0x36) { // cmpob*
                body = "AC = (AC & ~7u) | gen::cc_u(" + c1(in) + ", " + c2(in) + ");";
                exit = "if (AC & " + std::to_string(code & 7) + ") " + jump(tgt_masked);
            } else if (code >= 0x39 && code <= 0x3e) { // cmpib*
                body = "AC = (AC & ~7u) | gen::cc_s(int32_t(" + c1(in) + "), int32_t(" + c2(in) + "));";
                exit = "if (AC & " + std::to_string(code & 7) + ") " + jump(tgt_masked);
            } else return unsupported(in);
            break;

        case i960::Format::Mem: {
            const std::string e = ea(in), r = R(in.src1);
            switch (code) {
            case 0x80: body = r + " = c.bus->read_byte(" + e + ");"; break;                          // ldob
            case 0x82: body = "c.bus->write_byte(" + e + ", uint8_t(" + r + "));"; break;            // stob
            case 0x84: body = "c.m_IP = " + e + ";"; exit = "goto dispatch;"; falls = false; break; // bx
            case 0x85: body = "{ const uint32_t t = " + e + "; " + r + " = " + hex(next) + "; c.m_IP = t; }"; // balx
                       exit = "goto dispatch;"; falls = false; break;
            case 0x86: body = "{ const uint32_t t = " + e + "; c.m_IP = " + hex(next) + "; c.do_call(t, 0, R[1]); }"; // callx
                       exit = "goto dispatch;"; falls = false; break;
            case 0x88: body = r + " = c.i960_read_word_unaligned(" + e + ");"; break;                 // ldos
            case 0x8a: body = "c.i960_write_word_unaligned(" + e + ", uint16_t(" + r + "));"; break;  // stos
            case 0x8c: body = r + " = " + e + ";"; break;                                             // lda
            case 0x90: body = r + " = c.i960_read_dword_unaligned(" + e + ");"; break;                // ld
            case 0x92: body = "c.i960_write_dword_unaligned(" + e + ", " + r + ");"; break;           // st
            case 0x98: case 0xa0: case 0xb0: case 0x9a: case 0xa2: case 0xb2: {                      // ldl/ldt/ldq, stl/stt/stq
                const int n = (code & 0xf0) == 0x90 ? 2 : (code & 0xf0) == 0xa0 ? 3 : 4;
                const unsigned base = in.src1 & (n == 2 ? 0x1eu : 0x1cu);
                const bool store = code & 2;
                // MAME advances the address only on BURST-flagged regions.
                body = "{ uint32_t a = " + e + "; ";
                for (int i = 0; i < n; ++i) {
                    body += store ? "c.i960_write_dword_unaligned(a, " + R(base + i) + "); "
                                  : R(base + i) + " = c.i960_read_dword_unaligned(a); ";
                    if (i + 1 < n) body += "if (c.bus->flags(a) & rt::Cpu::BURST) a += 4; ";
                }
                body += "}";
                break;
            }
            case 0xc0: body = r + " = uint32_t(int32_t(int8_t(c.bus->read_byte(" + e + "))));"; break;     // ldib
            case 0xc2: body = "c.bus->write_byte(" + e + ", uint8_t(" + r + "));"; break;                   // stib
            case 0xc8: body = r + " = uint32_t(int32_t(int16_t(c.i960_read_word_unaligned(" + e + "))));"; break; // ldis
            case 0xca: body = "c.i960_write_word_unaligned(" + e + ", uint16_t(" + r + "));"; break;         // stis
            default: return unsupported(in);
            }
            break;
        }

        case i960::Format::Reg: {
            // An FP operand with its mode bit set names fp0-fp3 or an FP literal,
            // which needs extended-precision state; only the operands each op
            // actually uses count (unused fields may carry mode bits).
            switch (code) {
            case 0x580: two("t2 ^ (1u << (t1 & 31))"); break;             // notbit
            case 0x581: two("t2 & t1"); break;                             // and
            case 0x582: two("t2 & ~t1"); break;                            // andnot
            case 0x583: two("t2 | (1u << (t1 & 31))"); break;             // setbit
            case 0x584: two("(~t2) & t1"); break;                          // notand
            case 0x586: two("t2 ^ t1"); break;                             // xor
            case 0x587: two("t2 | t1"); break;                             // or
            case 0x588: two("(~t2) & (~t1)"); break;                       // nor
            case 0x589: two("~(t2 ^ t1)"); break;                          // xnor
            case 0x58a: set("~" + s1(in)); break;                          // not
            case 0x58b: two("t2 | ~t1"); break;                            // ornot
            case 0x58c: two("t2 & ~(1u << (t1 & 31))"); break;            // clrbit
            case 0x58d: two("(~t2) | t1"); break;                          // notor
            case 0x58e: two("~t2 | ~t1"); break;                           // nand
            case 0x58f: two("(AC & 2) ? (t2 | (1u << (t1 & 31))) : (t2 & ~(1u << (t1 & 31)))"); break; // alterbit
            case 0x590: case 0x591: two("t2 + t1"); break;                 // addo, addi (MAME: no overflow)
            case 0x592: case 0x593: two("t2 - t1"); break;                 // subo, subi
            case 0x598: two("t1 >= 32 ? 0u : t2 >> t1"); break;            // shro
            case 0x59a: two("t1 >= 32 ? 0u : (int32_t(t2) < 0 ? ((t2 & ((1u << t1) - 1)) ? uint32_t((int32_t(t2) >> t1) + 1) "
                            ": uint32_t(int32_t(t2) >> t1)) : t2 >> t1)"); break; // shrdi
            case 0x59b: two("t1 >= 32 ? (int32_t(t2) < 0 ? 0xffffffffu : 0u) : uint32_t(int32_t(t2) >> t1)"); break; // shri
            case 0x59c: case 0x59e: two("t1 >= 32 ? 0u : t2 << t1"); break; // shlo, shli
            case 0x59d: two("std::rotl(t2, int(t1 & 0x1f))"); break;       // rotate
            case 0x5a0: body = "AC = (AC & ~7u) | gen::cc_u(" + s1(in) + ", " + s2(in) + ");"; break; // cmpo
            case 0x5a1: body = "AC = (AC & ~7u) | gen::cc_s(int32_t(" + s1(in) + "), int32_t(" + s2(in) + "));"; break; // cmpi
            case 0x5a2: body = "if (!(AC & 4)) AC = (AC & ~7u) | (" + s1(in) + " <= " + s2(in) + " ? 2u : 1u);"; break; // concmpo
            case 0x5a3: body = "if (!(AC & 4)) AC = (AC & ~7u) | (int32_t(" + s1(in) + ") <= int32_t(" + s2(in) + ") ? 2u : 1u);"; break; // concmpi
            case 0x5a4: case 0x5a5: case 0x5a6: case 0x5a7: {              // cmpinco/i, cmpdeco/i
                const bool sgn = code & 1, inc = code < 0x5a6;
                body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; AC = (AC & ~7u) | " +
                       (sgn ? "gen::cc_s(int32_t(t1), int32_t(t2))" : "gen::cc_u(t1, t2)") + "; " + d + " = t2 " +
                       (inc ? "+" : "-") + " 1; }";
                break;
            }
            case 0x5ac: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; AC &= ~7u; "
                               "if ((t1 & 0xff000000) == (t2 & 0xff000000) || (t1 & 0x00ff0000) == (t2 & 0x00ff0000) || "
                               "(t1 & 0x0000ff00) == (t2 & 0x0000ff00) || (t1 & 0x000000ff) == (t2 & 0x000000ff)) AC |= 2; }"; break; // scanbyte
            case 0x5ae: body = "{ const uint32_t t1 = " + s1(in) + " & 0x1f, t2 = " + s2(in) + "; AC = (t2 & (1u << t1)) ? ((AC & ~7u) | 2u) : (AC & ~7u); }"; break; // chkbit
            case 0x5cc: set(s1(in)); break;                                // mov
            case 0x5dc: case 0x5ec: case 0x5fc: {                          // movl/movt/movq
                const int n = code == 0x5dc ? 2 : code == 0x5ec ? 3 : 4;
                const unsigned dst = in.dst & (n == 2 ? 0x1eu : 0x1cu);
                if (in.m1) {
                    for (int i = 0; i < n; ++i) body += R(dst + i) + " = " + lit(in.src1) + "; ";
                } else { // copy through temporaries (MAME memcpy; glibc loads before storing for these sizes)
                    body += "{ uint32_t t[4]; ";
                    for (int i = 0; i < n; ++i) body += "t[" + std::to_string(i) + "] = " + R(in.src1 + i) + "; ";
                    for (int i = 0; i < n; ++i) body += R(dst + i) + " = t[" + std::to_string(i) + "]; ";
                    body += "}";
                }
                break;
            }
            case 0x600: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; if (t1 == 0xff000004u) c.m_ICR = c.bus->read_dword(t2); "
                               "else c.bus->write_dword(t1, c.bus->read_dword(t2)); AC = (AC & ~7u) | 2u; }"; break; // synmov
            case 0x602: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; if (t1 == 0xff000010u) { c.m_IP = " + hex(next) +
                               "; c.send_iac(t2); } else { for (uint32_t k = 0; k < 16; k += 4) c.bus->write_dword(t1 + k, c.bus->read_dword(t2 + k)); } "
                               "AC = (AC & ~7u) | 2u; }";
                        exit = "if (c.m_IP != " + hex(next) + " && c.m_IP != " + hex(pc) + ") goto dispatch;"; break; // synmovq (an IAC can reinit)
            case 0x645: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; " + d + " = AC; AC = (AC & ~t1) | (t2 & t1); }"; break; // modac
            case 0x655: body = "{ const uint32_t t1 = c.m_PC, t2 = " + s2(in) + "; c.m_PC = (c.m_PC & ~t2) | (" + d + " & t2); " + d +
                               " = t1; c.m_IP = " + hex(next) + "; if ((t1 >> 16 & 0x1f) > (c.m_PC >> 16 & 0x1f)) c.check_pending_irqs(); }";
                        exit = "if (c.m_IP != " + hex(next) + ") goto dispatch;"; break; // modpc
            case 0x670: body = "{ const uint64_t p = uint64_t(" + s1(in) + ") * uint64_t(" + s2(in) + "); " + d + " = uint32_t(p); " +
                               R(in.dst + 1) + " = uint32_t(p >> 32); }"; break; // emul
            case 0x671: {                                                  // ediv
                const std::string src2 = in.m2 ? "uint64_t(" + lit(in.src2) + ")"
                                               : "(uint64_t(" + R(in.src2) + ") | (uint64_t(" + R(in.src2 + 1) + ") << 32))";
                body = "{ const uint64_t a = " + s1(in) + ", b = " + src2 + "; const uint32_t rem = uint32_t(b % a), quo = uint32_t(b / a); " +
                       d + " = rem; " + R(in.dst + 1) + " = quo; }";
                break;
            }
            case 0x674: if (in.m3) return unsupported(in);                  // cvtir: integer src1, real dst
                        set("gen::f2u(float(double(int32_t(" + s1(in) + "))))"); break;
            case 0x677: if (in.m2 || in.m3) return unsupported(in);         // scaler: integer src1, real src2/dst
                        set("gen::f2u(float(double(gen::u2f(" + R(in.src2) + ")) * std::pow(2.0, double(int32_t(" + s1(in) + ")))))"); break;
            case 0x685: if (in.m1 || in.m2) return unsupported(in);         // cmpr
                        body = "AC = (AC & ~7u) | gen::cc_d(double(gen::u2f(" + R(in.src1) + ")), double(gen::u2f(" + R(in.src2) + ")));"; break; // cmpr
            case 0x6c0: if (in.m1) return unsupported(in);                  // cvtri: real src1, integer dst
                        set("uint32_t(int32_t(gen::round_to_int(double(gen::u2f(" + R(in.src1) + ")), AC)))"); break; // cvtri
            case 0x6c2: if (in.m1) return unsupported(in);                  // cvtzri
                        set("uint32_t(int32_t(double(gen::u2f(" + R(in.src1) + "))))"); break; // cvtzri
            case 0x701: two("t2 * t1"); break;                             // mulo
            case 0x708: two("t2 % t1"); break;                             // remo (MAME: undefined on 0)
            case 0x70b: two("t1 == 0 ? 0u : t2 / t1"); break;              // divo (MAME: 0 on divide by zero)
            case 0x741: two("uint32_t(int32_t(t2) * int32_t(t1))"); break; // muli
            case 0x748: two("uint32_t(int32_t(t2) % int32_t(t1))"); break; // remi
            case 0x749: two("uint32_t([](int32_t a, int32_t b) { int32_t r = a - (a / b) * b; "
                            "if ((b ^ a) < 0 && r != 0) r += b; return r; }(int32_t(t2), int32_t(t1)))"); break; // modi (sign of src1)
            case 0x74b: two("uint32_t(int32_t(t2) / int32_t(t1))"); break; // divi
            case 0x78d: if (in.m1 || in.m2 || in.m3) return unsupported(in); // subr: reals in registers (MAME: double, then float)
                        set("gen::f2u(float(double(gen::u2f(" + R(in.src2) + ")) - double(gen::u2f(" + R(in.src1) + "))))"); break;
            case 0x78f: if (in.m1 || in.m2 || in.m3) return unsupported(in); // addr
                        set("gen::f2u(float(double(gen::u2f(" + R(in.src2) + ")) + double(gen::u2f(" + R(in.src1) + "))))"); break;
            default: return unsupported(in);
            }
            break;
        }
        default: return unsupported(in);
        }

        ++native;
        exits = !exit.empty();
        // --fast_inaccuracy: inside a block the IP is not stored; an instruction that
        // compares the IP with its own address (synmovq's IAC check) gets it.
        if (!leader && exit.find("m_IP != " + hex(pc)) != std::string::npos) line("c.m_IP = " + hex(pc) + ";");
        if (body.find("cond") != std::string::npos) line("{ bool cond;");
        if (!body.empty()) line(body);
        if (!blocks) line("++ls.count;");
        else if (block_end) line(block_end == 1 ? "++ls.count;" : "ls.count += " + std::to_string(block_end) + "u;");
        if (!exit.empty()) line(exit);
        if (body.find("cond") != std::string::npos) line("}");
        return falls;
    }

    // No native template: recorded, and the recompile fails.
    bool unsupported(const Insn &in) {
        char b[128];
        std::snprintf(b, sizeof b, "%08x: %s", in.addr, i960::format_mame(in).c_str());
        missing->push_back(b);
        return false;
    }
};

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "m2recomp: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(f), {}};
}

void load_hooks(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "m2recomp: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string addr, name;
        if (ls >> addr >> name) g_hooks[uint32_t(std::strtoul(addr.c_str(), nullptr, 16))] = name;
    }
}

std::vector<uint32_t> load_seeds(const std::string &path) {
    std::ifstream f(path);
    std::vector<uint32_t> v;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        std::string a, b, c;
        s >> a;
        if (s >> b >> c) { // harvest format: kind from target count
            if (a == "icr" || a == "irqat") continue;
            v.push_back(uint32_t(std::strtoul(c.c_str(), nullptr, 16)));
        } else {
            v.push_back(uint32_t(std::strtoul(a.c_str(), nullptr, 16)));
        }
    }
    return v;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2recomp PROGRAM.bin OUTDIR [--seeds FILE]... [--hooks FILE] [--chunk N] [--fast_inaccuracy] "
                             "[--profile FILE [--hot-share PERCENT]]\n");
        return 2;
    }
    const std::vector<uint8_t> img = load(argv[1]);
    const std::string out = argv[2];
    std::vector<uint32_t> seeds;
    size_t chunk = 1500;
    bool blocks = false, chain = false;
    std::optional<Profile> profile;
    uint64_t hot_share = 90; // --profile: percent of the executed instructions the hot chunks cover
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) {
            auto s = load_seeds(argv[++i]);
            seeds.insert(seeds.end(), s.begin(), s.end());
        } else if (!std::strcmp(argv[i], "--hooks") && i + 1 < argc) {
            load_hooks(argv[++i]);
        } else if (!std::strcmp(argv[i], "--chunk") && i + 1 < argc) {
            chunk = std::strtoul(argv[++i], nullptr, 0);
        } else if (!std::strcmp(argv[i], "--profile") && i + 1 < argc) {
            profile = load_profile(argv[++i]);
        } else if (!std::strcmp(argv[i], "--hot-share") && i + 1 < argc) {
            hot_share = std::min<uint64_t>(100, std::strtoull(argv[++i], nullptr, 0));
        } else if (!std::strcmp(argv[i], "--fast_inaccuracy")) {
            blocks = true; // 1. block bookkeeping
            chain = true;  // 2. direct chaining
        } else {
            std::fprintf(stderr, "m2recomp: unknown option %s (--blocks is now --fast_inaccuracy)\n", argv[i]);
            return 2;
        }
    }

    // model2o: program ROM at 0, its upper 128 KiB mirrored at 0x00220000.
    auto read = [&](uint32_t a) -> std::optional<uint32_t> {
        uint64_t off;
        if (a < img.size()) off = a;
        else if (a >= 0x220000 && a < 0x240000) off = a - 0x200000;
        else return std::nullopt;
        if (off + 4 > img.size()) return std::nullopt;
        uint32_t v;
        std::memcpy(&v, &img[off], 4);
        return v;
    };
    std::vector<uint32_t> all = i960::boot_seeds(read).all();
    all.insert(all.end(), seeds.begin(), seeds.end());
    const i960::ReachResult r = i960::reach(all, read);

    std::vector<const Insn *> insns;
    for (const auto &[a, in] : r.insns) insns.push_back(&in);

    // Block starts. An analysis pass over every instruction records the direct
    // transfer targets, the instructions that end a block and those that can
    // fall through to the next address. The basic blocks (by address) are the
    // block profile's (M2_PROF); --fast_inaccuracy's blocks also start
    // wherever the emission order breaks the address order (chunk starts).
    std::set<uint32_t> leaders;
    std::vector<char> falls(insns.size());
    {
        std::set<uint32_t> targets, none;
        std::vector<std::string> ignored; // reported by the real pass
        for (size_t i = 0; i < insns.size(); ++i) {
            const Insn *in = insns[i];
            Emitter dry{&none, {}, 0, &ignored};
            dry.targets = &targets;
            falls[i] = dry.emit(*in);
            if (!falls[i] || dry.exits) leaders.insert(in->addr + in->length);
        }
        leaders.insert(targets.begin(), targets.end());
        leaders.insert(all.begin(), all.end());
        for (const auto &h : g_hooks) leaders.insert(h.first);
    }
    // Basic blocks in address order: block_of[i] = the block of insns[i].
    std::vector<uint32_t> block_addr, block_len;
    std::vector<uint32_t> block_of(insns.size());
    std::vector<char> block_start(insns.size());
    for (size_t i = 0; i < insns.size(); ++i) {
        const Insn &in = *insns[i];
        block_start[i] = i == 0 || leaders.count(in.addr) || insns[i - 1]->addr + insns[i - 1]->length != in.addr;
        if (block_start[i]) block_addr.push_back(in.addr), block_len.push_back(0);
        block_of[i] = uint32_t(block_addr.size() - 1);
        ++block_len.back();
    }
    size_t block_count = 0;

    // Chunks: runs of consecutive addresses, `chunk` instructions each; or,
    // with --profile, the hot chunks then the rest cut where the fewest
    // transfers cross (see the top). order = insns indices in emission order,
    // bounds = [first, end) of order per chunk.
    std::vector<size_t> order(insns.size());
    std::vector<std::pair<size_t, size_t>> bounds;
    std::vector<uint32_t> chunk_of(insns.size());
    size_t hot_chunks = 0, hot_insns = 0;
    for (size_t i = 0; i < insns.size(); ++i) order[i] = i;
    if (!profile) {
        for (size_t start = 0; start < insns.size(); start += chunk) bounds.emplace_back(start, std::min(insns.size(), start + chunk));
    } else {
        // Chains: runs of consecutive addresses, each instruction able to fall
        // through to the next. They are never split between chunks (unless
        // longer than a chunk).
        std::vector<size_t> chain_first; // insns index of each chain's first instruction (and the end)
        std::vector<uint32_t> chain_of(insns.size());
        for (size_t i = 0; i < insns.size(); ++i) {
            if (i == 0 || !falls[i - 1] || insns[i - 1]->addr + insns[i - 1]->length != insns[i]->addr)
                chain_first.push_back(i);
            chain_of[i] = uint32_t(chain_first.size() - 1);
        }
        const uint32_t U = uint32_t(chain_first.size());
        chain_first.push_back(insns.size());
        auto chain_size = [&](uint32_t c) { return chain_first[c + 1] - chain_first[c]; };
        // Heat (instructions executed) per chain, and transfers between chains
        // (both directions together): adj[c] = (other chain, count).
        std::map<uint32_t, size_t> index; // block start -> insns index
        for (size_t i = 0; i < insns.size(); ++i)
            if (block_start[i]) index[insns[i]->addr] = i;
        std::vector<uint64_t> heat(U);
        uint64_t executed = 0;
        for (const auto &[a, n] : profile->blocks)
            if (const auto it = index.find(a); it != index.end()) {
                heat[chain_of[it->second]] += n * block_len[block_of[it->second]];
                executed += n * block_len[block_of[it->second]];
            }
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> pairs;
        for (const auto &[e, n] : profile->edges) {
            const auto a = index.find(e.first), b = index.find(e.second);
            if (a == index.end() || b == index.end()) continue;
            uint32_t x = chain_of[a->second], y = chain_of[b->second];
            if (x == y) continue;
            if (x > y) std::swap(x, y);
            pairs[{x, y}] += n;
        }
        std::vector<std::vector<std::pair<uint32_t, uint64_t>>> adj(U);
        for (const auto &[xy, n] : pairs) adj[xy.first].push_back({xy.second, n}), adj[xy.second].push_back({xy.first, n});

        // 1. Very hot chains: the hottest, by instructions executed per
        // instruction of code, until they cover `hot_share` of the profile's
        // executed instructions.
        std::vector<uint32_t> by_heat;
        for (uint32_t c = 0; c < U; ++c)
            if (heat[c]) by_heat.push_back(c);
        std::stable_sort(by_heat.begin(), by_heat.end(),
                         [&](uint32_t a, uint32_t b) { return heat[a] * chain_size(b) > heat[b] * chain_size(a); });
        std::vector<char> hot(U);
        for (uint64_t covered = 0; uint32_t c : by_heat) {
            if (covered >= executed * hot_share / 100) break;
            hot[c] = 1;
            covered += heat[c];
        }
        // Hot chains clustered greedily, heaviest transfer first, up to a
        // chunk's size; the clusters fill the hot chunks, hottest first.
        std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> by_weight;
        for (const auto &[xy, n] : pairs)
            if (hot[xy.first] && hot[xy.second]) by_weight.push_back({n, xy});
        std::stable_sort(by_weight.begin(), by_weight.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::vector<uint32_t> parent(U);
        std::vector<std::vector<uint32_t>> members(U);
        std::vector<size_t> size(U);
        std::vector<uint64_t> cluster_heat(heat);
        for (uint32_t c = 0; c < U; ++c) parent[c] = c, members[c] = {c}, size[c] = chain_size(c);
        auto find = [&](uint32_t c) {
            while (parent[c] != c) c = parent[c] = parent[parent[c]];
            return c;
        };
        for (const auto &[n, xy] : by_weight) {
            const uint32_t x = find(xy.first), y = find(xy.second);
            if (x == y || size[x] + size[y] > chunk) continue;
            parent[y] = x;
            members[x].insert(members[x].end(), members[y].begin(), members[y].end());
            members[y].clear();
            size[x] += size[y];
            cluster_heat[x] += cluster_heat[y];
        }
        std::vector<uint32_t> clusters;
        for (uint32_t c = 0; c < U; ++c)
            if (hot[c] && find(c) == c) clusters.push_back(c);
        std::stable_sort(clusters.begin(), clusters.end(), [&](uint32_t a, uint32_t b) { return cluster_heat[a] > cluster_heat[b]; });
        size_t pos = 0, first = 0;
        auto close = [&] {
            if (pos > first) bounds.emplace_back(first, pos);
            first = pos;
        };
        auto add = [&](uint32_t c) { // a chain starts a new chunk rather than straddle two
            for (size_t i = chain_first[c]; i < chain_first[c + 1]; ++i) {
                if (pos - first == chunk || (i == chain_first[c] && pos - first + chain_size(c) > chunk)) close();
                order[pos++] = i;
            }
        };
        for (uint32_t c : clusters)
            for (uint32_t m : members[c]) add(m);
        close();
        hot_chunks = bounds.size(), hot_insns = pos;

        // 2. The rest, in address order, cut by dynamic programming:
        // best[j] = the least transfer count crossing a cut for rest[0, j) in
        // chunks of at most `chunk` instructions (then the fewest chunks). A
        // chunk rest[i, j) adds the transfers between it and the chains before
        // it, kept up to date as i moves down. (Transfers to hot chains cross
        // whatever the cuts.)
        std::vector<uint32_t> rest;
        std::vector<size_t> rank(U, SIZE_MAX); // a chain's position in rest
        for (uint32_t c = 0; c < U; ++c)
            if (!hot[c]) rank[c] = rest.size(), rest.push_back(c);
        std::vector<size_t> prefix(rest.size() + 1); // instructions in rest[0, j)
        for (size_t j = 0; j < rest.size(); ++j) prefix[j + 1] = prefix[j] + chain_size(rest[j]);
        struct Best { uint64_t cost; size_t chunks, from; };
        std::vector<Best> best(rest.size() + 1, {UINT64_MAX, 0, 0});
        best[0] = {0, 0, 0};
        for (size_t j = 1; j <= rest.size(); ++j) {
            uint64_t w = 0;
            for (size_t i = j; i-- > 0;) {
                if (i + 1 < j && prefix[j] - prefix[i] > chunk) break; // one chain alone may be longer
                for (const auto &[v, n] : adj[rest[i]]) {
                    const size_t r = rank[v];
                    if (r == SIZE_MAX) continue;
                    if (r < i) w += n;      // to a chain before this chunk
                    else if (r < j) w -= n; // to a chain already in it: no longer crossing
                }
                const Best c{best[i].cost + w, best[i].chunks + 1, i};
                if (c.cost < best[j].cost || (c.cost == best[j].cost && c.chunks < best[j].chunks)) best[j] = c;
            }
        }
        std::vector<size_t> cuts;
        for (size_t j = rest.size(); j > 0; j = best[j].from) cuts.push_back(j);
        std::reverse(cuts.begin(), cuts.end());
        for (size_t j = 0, k = 0; j < rest.size(); ++j) {
            add(rest[j]);
            if (j + 1 == cuts[k]) close(), ++k;
        }
    }
    for (size_t ci = 0; ci < bounds.size(); ++ci)
        for (size_t k = bounds[ci].first; k < bounds[ci].second; ++k) chunk_of[order[k]] = uint32_t(ci);

    uint64_t native = 0;
    std::vector<std::string> missing;
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // per chunk: first, last address emitted
    for (size_t ci = 0; ci < bounds.size(); ++ci) {
        const size_t start = bounds[ci].first, end = bounds[ci].second;
        std::set<uint32_t> addrs;
        for (size_t k = start; k < end; ++k) addrs.insert(insns[order[k]]->addr);
        Emitter em{&addrs, {}, 0, &missing};
        em.blocks = blocks;
        // Block structure (every instruction is its own block without --fast_inaccuracy):
        // lead[i] starts a block, pos[i] = instructions before it in its block,
        // len[i] = its block's length.
        const size_t n = end - start;
        std::vector<char> lead(n, 1);
        std::vector<unsigned> pos(n, 0), len(n, 1);
        if (blocks) {
            for (size_t i = 1; i < n; ++i) {
                const Insn &prev = *insns[order[start + i - 1]], &in = *insns[order[start + i]];
                lead[i] = leaders.count(in.addr) || prev.addr + prev.length != in.addr;
            }
            for (size_t i = 0, b = 0; i < n; ++i) {
                if (lead[i]) b = i, ++block_count;
                pos[i] = unsigned(i - b);
            }
            for (size_t i = n; i-- > 0;) len[i] = i + 1 < n && !lead[i + 1] ? len[i + 1] : pos[i] + 1;
        }
        char name[32];
        std::snprintf(name, sizeof name, "chunk_%03zu", ci);
        em.o << "// Generated by m2recomp. Derived from the game: never commit.\n";
        if (blocks) em.o << "// --fast_inaccuracy: interrupt/event checks and the instruction count once per basic block.\n";
        if (profile) em.o << "// --profile: " << (ci < hot_chunks ? "very hot code from across the program, clustered by the block profile"
                                                       : "consecutive addresses, cut where the block profile says") << ".\n";
        em.o << "#include \"runtime/gen_support.h\"\n\n" << "namespace gen {\n\n";
        if (blocks) em.o << kBlockBoundary;
        em.o << "void " << name << "(Env &e) {\n"
             << "    rt::Cpu &c = e.c;\n    rt::Lockstep &ls = e.ls;\n"
             << "    uint32_t *const R = c.m_r;\n    uint32_t &AC = c.m_AC;\n"
             << "dispatch:\n    if (ls.finished()) return;\n    switch (c.m_IP) {\n";
        for (size_t i = 0; i < n; ++i) {
            const uint32_t a = insns[order[start + i]]->addr;
            em.o << "    case " << hex(a) << ": ";
            // Entered mid-block: the block's count is added at its end.
            if (!lead[i]) em.o << "ls.count -= " << pos[i] << "u; ";
            em.o << "goto " << lbl(a) << ";\n";
        }
        em.o << "    default: return;\n    }\n";
        for (size_t k = start; k < end; ++k) {
            const size_t idx = order[k];
            const Insn &in = *insns[idx];
            const size_t i = k - start;
            em.leader = lead[i] != 0;
            em.block_end = blocks && (i + 1 == n || lead[i + 1]) ? len[i] : 0;
            em.prof_block = block_start[idx] ? long(block_of[idx]) : -1;
            const bool falls_through = em.emit(in);
            const uint32_t next = in.addr + in.length;
            const bool next_is_following = k + 1 < end && insns[order[k + 1]]->addr == next;
            if (falls_through && !next_is_following) em.line(em.jump(next));
        }
        em.o << "}\n\n} // namespace gen\n";
        std::ofstream(out + "/" + name + ".cpp") << em.o.str();
        native += em.native;
        ranges.emplace_back(insns[order[start]]->addr, insns[order[end - 1]]->addr);
    }

    // --profile: what the boundaries gained, by the profile's own transfers.
    if (profile) {
        std::map<uint32_t, size_t> index;
        for (size_t i = 0; i < insns.size(); ++i)
            if (block_start[i]) index[insns[i]->addr] = i;
        uint64_t total = 0, cross_before = 0, cross_after = 0;
        for (const auto &[e, n] : profile->edges) {
            const auto a = index.find(e.first), b = index.find(e.second);
            if (a == index.end() || b == index.end()) continue;
            total += n;
            cross_before += n * (a->second / chunk != b->second / chunk);
            cross_after += n * (chunk_of[a->second] != chunk_of[b->second]);
        }
        std::printf("m2recomp: --profile: %zu hot chunks (%zu instructions) of %zu; transfers between blocks that leave "
                    "their chunk: %.2f%% with a cut every %zu instructions, %.2f%% laid out by the profile\n",
                    hot_chunks, hot_insns, bounds.size(), total ? 100.0 * double(cross_before) / double(total) : 0.0, chunk,
                    total ? 100.0 * double(cross_after) / double(total) : 0.0);
    }

    // Dispatch table and instruction index.
    std::ostringstream t;
    t << "// Generated by m2recomp. Derived from the game: never commit.\n";
    if (blocks) t << "// --fast_inaccuracy: interrupt/event checks and the instruction count once per basic block.\n";
    if (chain) t << "// --fast_inaccuracy: gen::run chains chunks itself.\n";
    if (profile) t << "// m2recomp --profile: chunks laid out by the block profile, found by binary search.\n";
    t << "#include \"runtime/gen_support.h\"\n\n"
      << "#include <algorithm>\n\nnamespace gen {\n\n";
    for (size_t i = 0; i < ranges.size(); ++i) t << "void chunk_" << (i < 10 ? "00" : i < 100 ? "0" : "") << i << "(Env &);\n";
    t << "\nnamespace {\nconst uint32_t kAddrs[] = {\n";
    for (const Insn *in : insns) t << "    " << hex(in->addr) << ",\n";
    if (profile) {
        // The hot chunks hold addresses from all over the program: the chunk
        // of each kAddrs entry instead of address ranges.
        t << "};\nconst uint16_t kChunkOf[] = {\n";
        for (size_t i = 0; i < insns.size(); ++i) t << "    " << chunk_of[i] << ",\n";
        t << "};\nvoid (*const kChunkFns[])(Env &) = {\n";
        for (size_t i = 0; i < ranges.size(); ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "chunk_%03zu", i);
            t << "    " << name << ",\n";
        }
        t << "};\n"
          << "// The chunk holding address a, or -1.\n"
          << "int chunk_of(uint32_t a) {\n"
          << "    const uint32_t *p = std::lower_bound(std::begin(kAddrs), std::end(kAddrs), a);\n"
          << "    return p != std::end(kAddrs) && *p == a ? kChunkOf[p - std::begin(kAddrs)] : -1;\n"
          << "}\n} // namespace\n\n"
          << "bool has_code(uint32_t a) { return std::binary_search(std::begin(kAddrs), std::end(kAddrs), a); }\n\n";
        if (chain)
            // As below: back in the same chunk means no code there.
            t << "void run(Env &e) {\n"
              << "    int prev = -1;\n"
              << "    do {\n"
              << "        const int k = chunk_of(e.c.m_IP);\n"
              << "        if (k < 0 || k == prev) return;\n"
              << "        kChunkFns[k](e);\n"
              << "        prev = k;\n"
              << "    } while (!e.ls.finished());\n"
              << "}\n\n";
        else
            t << "void run(Env &e) {\n    if (const int k = chunk_of(e.c.m_IP); k >= 0) kChunkFns[k](e);\n}\n\n";
    } else {
        t << "};\nstruct Range { uint32_t first, last; void (*fn)(Env &); };\nconst Range kChunks[] = {\n";
        for (size_t i = 0; i < ranges.size(); ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "chunk_%03zu", i);
            t << "    {" << hex(ranges[i].first) << ", " << hex(ranges[i].second) << ", " << name << "},\n";
        }
        t << "};\n} // namespace\n\n"
          << "bool has_code(uint32_t a) { return std::binary_search(std::begin(kAddrs), std::end(kAddrs), a); }\n\n";
        if (chain) {
            // A chunk returns only when the lockstep is finished or m_IP is not
            // in its switch. Back in the same chunk means no code there: return,
            // and GameLoop's has_code reports the address as before.
            t << "namespace {\n"
              << "const Range *chunk_of(uint32_t a) {\n"
              << "    const Range *r = std::upper_bound(std::begin(kChunks), std::end(kChunks), a,\n"
              << "                                      [](uint32_t x, const Range &c) { return x < c.first; });\n"
              << "    if (r == std::begin(kChunks)) return nullptr;\n"
              << "    --r;\n"
              << "    return a <= r->last ? r : nullptr;\n"
              << "}\n"
              << "} // namespace\n\n"
              << "void run(Env &e) {\n"
              << "    const Range *prev = nullptr;\n"
              << "    do {\n"
              << "        const Range *r = chunk_of(e.c.m_IP);\n"
              << "        if (!r || r == prev) return;\n"
              << "        r->fn(e);\n"
              << "        prev = r;\n"
              << "    } while (!e.ls.finished());\n"
              << "}\n\n";
        } else {
            t << "void run(Env &e) {\n    const uint32_t a = e.c.m_IP;\n"
              << "    for (const Range &r : kChunks)\n        if (a >= r.first && a <= r.last) { r.fn(e); return; }\n}\n\n";
        }
    }
    // The block profile's tables (m2run --profile), in M2_PROFILE builds only.
    t << "#ifdef M2_PROFILE\nnamespace {\nconst uint32_t kBlocks[][2] = { // start address, instructions\n";
    for (size_t b = 0; b < block_addr.size(); ++b) t << "    {" << hex(block_addr[b]) << ", " << block_len[b] << "u},\n";
    t << "};\n} // namespace\n"
      << "size_t block_count() { return std::size(kBlocks); }\n"
      << "uint32_t block_addr(size_t b) { return kBlocks[b][0]; }\n"
      << "uint32_t block_length(size_t b) { return kBlocks[b][1]; }\n"
      << "#endif\n\n";
    t
      << "uint64_t native_instructions() { return " << native << "ull; }\n\n} // namespace gen\n";
    if (!missing.empty()) {
        std::fprintf(stderr, "m2recomp: FAILED: %zu reachable instructions have no native template:\n", missing.size());
        for (const std::string &m : missing) std::fprintf(stderr, "  %s\n", m.c_str());
        return 1;
    }
    std::ofstream(out + "/gen_table.cpp") << t.str();

    std::printf("m2recomp: %zu instructions from %zu seeds -> %zu chunks in %s, all native; %zu indirect sites\n",
                insns.size(), all.size(), ranges.size(), out.c_str(), r.indirect_sites.size());
    if (blocks)
        std::printf("m2recomp: --fast_inaccuracy: %zu basic blocks, %.2f instructions per block (not interrupt-exact), "
                    "direct chunk chaining\n",
                    block_count, block_count ? double(insns.size()) / double(block_count) : 0.0);
    return 0;
}
