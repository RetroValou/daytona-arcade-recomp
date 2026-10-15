// m2recomp: the i960 static recompiler (design doc, i960 static recompiler).
//
//   m2recomp PROGRAM.bin OUTDIR [--seeds FILE]... [--hooks FILE] [--chunk N] [--fast_inaccuracy]
//            [--no-mirror] [--alias GUEST:OFFSET:LENGTH]... [--profile] [--board model2o|model2a]
//
// The image is the program ROM at address 0. model2o mirrors its upper
// 128 KiB at 0x00220000; --no-mirror drops that (Model 2A: RAM there).
// --alias: guest addresses GUEST..GUEST+LENGTH-1 hold the image's bytes from
// OFFSET (hex), for code the game copies to RAM and runs there (srallyc:
// 0x5a0000:0x1000:0x60000, its boot copy at 0x514). The recompiled code
// assumes the game never rewrites that RAM.
//
// --hooks: "ADDRESS name" lines; the generated code calls rt::hook_<name>(c)
// just before the instruction at ADDRESS (each game's hooks: see
// src/games/hooks.h). A hook that is off does nothing.
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
// --profile (scripts/recompile.py and build_vita.py --i960-profile): counts
// the executions of every basic block (one increment at its end; the blocks
// --fast_inaccuracy would make, also without it, for the counters only), the
// dispatches, the chunk entries and the lockstep slow paths, and writes a
// table of what each block holds (bus loads and stores, ALU, multiplies,
// divides, branches, calls, FP; I960BlockInfo in src/runtime/i960_profile.h).
// The code must be built with M2_I960_PROFILE, whose runtime reports them.
// Results and counts are unchanged.
//
// --board model2o|model2a (scripts/games.py "board"; recompile.py --inline-bus,
// build_vita.py by default in its fast modes): loads and stores go through
// gen::BoardMem<board> (src/runtime/gen_support.h), the board's bus inlined: work
// RAM, program ROM and main RAM read and written in place, the rest through
// direct (not virtual) calls to rt::M2Board. Same results as the bus; the
// code then runs only on an M2Board of that board, built with M2_INLINE_BUS
// (GameLoop checks it at start), not on m2native's replay bus.
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
#include "runtime/i960_profile.h"

#include <algorithm>
#include <array>
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

// --profile: what an instruction does (I960Op counts), by the same codes
// Emitter::emit uses.
std::array<uint8_t, rt::kI960Ops> classify(const Insn &in) {
    std::array<uint8_t, rt::kI960Ops> k{};
    auto add = [&](rt::I960Op op, int n = 1) { k[op] = uint8_t(k[op] + n); };
    const uint16_t code = in.fmt == i960::Format::Reg ? in.op->code : uint16_t(in.word >> 24);
    switch (in.fmt) {
    case i960::Format::Ctrl:
        if (code == 0x08 || code == 0x0b) add(rt::I960OpBranch);
        else if (code == 0x09) add(rt::I960OpCall);
        else if (code == 0x0a) add(rt::I960OpRet), add(rt::I960OpIndirect);
        else add(rt::I960OpCond);
        break;
    case i960::Format::Cobr:
        add(rt::I960OpCompare);
        if (code >= 0x30) add(rt::I960OpCond);
        break;
    case i960::Format::Mem:
        switch (code) {
        case 0x80: case 0x88: case 0x90: case 0xc0: case 0xc8: add(rt::I960OpLoad); break;
        case 0x82: case 0x8a: case 0x92: case 0xc2: case 0xca: add(rt::I960OpStore); break;
        case 0x98: add(rt::I960OpLoad, 2); break;
        case 0xa0: add(rt::I960OpLoad, 3); break;
        case 0xb0: add(rt::I960OpLoad, 4); break;
        case 0x9a: add(rt::I960OpStore, 2); break;
        case 0xa2: add(rt::I960OpStore, 3); break;
        case 0xb2: add(rt::I960OpStore, 4); break;
        case 0x84: case 0x85: add(rt::I960OpIndirect); break;
        case 0x86: add(rt::I960OpCall), add(rt::I960OpIndirect); break;
        default: add(rt::I960OpAlu); break; // lda
        }
        break;
    case i960::Format::Reg:
        if ((code >= 0x5a0 && code <= 0x5a7) || code == 0x5ac || code == 0x5ae) add(rt::I960OpCompare);
        else if (code == 0x600) add(rt::I960OpSys), add(rt::I960OpLoad), add(rt::I960OpStore);
        else if (code == 0x602) add(rt::I960OpSys), add(rt::I960OpLoad, 4), add(rt::I960OpStore, 4);
        else if (code == 0x645 || code == 0x655 || code == 0x66d) add(rt::I960OpSys);
        else if (code == 0x670 || code == 0x701 || code == 0x741) add(rt::I960OpMul);
        else if (code == 0x671 || code == 0x708 || code == 0x70b || code == 0x748 || code == 0x749 || code == 0x74b)
            add(rt::I960OpDiv);
        else if (code == 0x676 || code == 0x677 || (code >= 0x680 && code <= 0x684) || (code >= 0x688 && code <= 0x68a) ||
                 (code >= 0x68c && code <= 0x68e) || (code >= 0x690 && code <= 0x692) ||
                 (code >= 0x698 && code <= 0x69a) || (code >= 0x69c && code <= 0x69e))
            add(rt::I960OpFpMath);
        else if (code >= 0x674) add(rt::I960OpFp);
        else add(rt::I960OpAlu);
        break;
    default: break;
    }
    return k;
}

// Enhancement hooks: address -> name (rt::hook_<name>), from --hooks.
std::map<uint32_t, std::string> g_hooks;

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
    std::string block_counter{};           // --profile: emitted at a counted block's last instruction
    bool inline_bus = false;               // --board: loads and stores through gen::BoardMem (the board's bus inlined)

    // Memory access: through Cpu::bus, or (--board) gen::BoardMem on the board B.
    std::string ld8(const std::string &a) const { return inline_bus ? "Mem::ld8(B, " + a + ")" : "c.bus->read_byte(" + a + ")"; }
    std::string ld16(const std::string &a) const {
        return inline_bus ? "Mem::ld16(B, " + a + ")" : "c.i960_read_word_unaligned(" + a + ")";
    }
    std::string ld32(const std::string &a) const {
        return inline_bus ? "Mem::ld32(B, " + a + ")" : "c.i960_read_dword_unaligned(" + a + ")";
    }
    std::string st8(const std::string &a, const std::string &v) const {
        return (inline_bus ? "Mem::st8(B, " : "c.bus->write_byte(") + a + ", uint8_t(" + v + "));";
    }
    std::string st16(const std::string &a, const std::string &v) const {
        return (inline_bus ? "Mem::st16(B, " : "c.i960_write_word_unaligned(") + a + ", uint16_t(" + v + "));";
    }
    std::string st32(const std::string &a, const std::string &v) const {
        return (inline_bus ? "Mem::st32(B, " : "c.i960_write_dword_unaligned(") + a + ", " + v + ");";
    }
    std::string flags(const std::string &a) const { return inline_bus ? "Mem::flags(B, " + a + ")" : "c.bus->flags(" + a + ")"; }

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

        std::string body, exit;   // body runs before ++count; exit transfers control
        bool falls = true;
        const std::string d = R(in.dst);
        auto set = [&](const std::string &v) { body += d + " = " + v + ";"; };
        auto two = [&](const std::string &v) { // read both sources first, as MAME does
            body += "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) + "; " + d + " = " + v + "; }";
        };
        // MAME get_1_rif / get_2_rif (l: get_..._rifl): a g/l register (a
        // single; l: a long real in the even/odd pair), fp0-fp3, or the FP
        // literals +1.0 (0x16) and +0.0 (any other).
        auto fsrc = [&](bool m, unsigned r, bool l) -> std::string {
            if (!m)
                return l ? "gen::u2d(uint64_t(" + R(r & 0x1e) + ") | uint64_t(" + R((r & 0x1e) + 1) + ") << 32)"
                         : "double(gen::u2f(" + R(r) + "))";
            if (r < 4) return "c.m_fp[" + std::to_string(r) + "]";
            return r == 0x16 ? "1.0" : "0.0";
        };
        // MAME set_rif / set_rifl: a register (through float; l: the pair), or
        // fp0-fp3. MAME stops on any other literal destination; so does this.
        auto fstore = [&](const std::string &v, bool l) -> std::optional<std::string> {
            if (!in.m3)
                return l ? "{ const uint64_t v = gen::d2u(" + v + "); " + R(in.dst & 0x1e) + " = uint32_t(v); " +
                               R((in.dst & 0x1e) + 1) + " = uint32_t(v >> 32); }"
                         : d + " = gen::f2u(float(" + v + "));";
            if (in.dst < 4) return "c.m_fp[" + std::to_string(in.dst) + "] = " + v + ";";
            return std::nullopt;
        };
        auto fset = [&](const std::string &v, bool l) {
            const auto st = fstore(v, l);
            if (st) body = *st;
            return st.has_value();
        };
        auto fop1 = [&](const std::string &v, bool l) { // t1 = src1
            const auto st = fstore(v, l);
            if (st) body = "{ const double t1 = " + fsrc(in.m1, in.src1, l) + "; " + *st + " }";
            return st.has_value();
        };
        // t1 = src1, t2 = src2 (src1_real false: src1 is an integer the
        // expression reads itself; only t2 is a real)
        auto fop2 = [&](const std::string &v, bool l, bool src1_real = true) {
            const auto st = fstore(v, l);
            if (st)
                body = "{ " + (src1_real ? "const double t1 = " + fsrc(in.m1, in.src1, l) + "; " : std::string()) +
                       "const double t2 = " + fsrc(in.m2, in.src2, l) + "; " + *st + " }";
            return st.has_value();
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
            case 0x80: body = r + " = " + ld8(e) + ";"; break;                                        // ldob
            case 0x82: body = st8(e, r); break;                                                      // stob
            case 0x84: body = "c.m_IP = " + e + ";"; exit = "goto dispatch;"; falls = false; break; // bx
            case 0x85: body = "{ const uint32_t t = " + e + "; " + r + " = " + hex(next) + "; c.m_IP = t; }"; // balx
                       exit = "goto dispatch;"; falls = false; break;
            case 0x86: body = "{ const uint32_t t = " + e + "; c.m_IP = " + hex(next) + "; c.do_call(t, 0, R[1]); }"; // callx
                       exit = "goto dispatch;"; falls = false; break;
            case 0x88: body = r + " = " + ld16(e) + ";"; break;                                       // ldos
            case 0x8a: body = st16(e, r); break;                                                     // stos
            case 0x8c: body = r + " = " + e + ";"; break;                                             // lda
            case 0x90: body = r + " = " + ld32(e) + ";"; break;                                       // ld
            case 0x92: body = st32(e, r); break;                                                     // st
            case 0x98: case 0xa0: case 0xb0: case 0x9a: case 0xa2: case 0xb2: {                      // ldl/ldt/ldq, stl/stt/stq
                const int n = (code & 0xf0) == 0x90 ? 2 : (code & 0xf0) == 0xa0 ? 3 : 4;
                const unsigned base = in.src1 & (n == 2 ? 0x1eu : 0x1cu);
                const bool store = code & 2;
                // MAME advances the address only on BURST-flagged regions.
                body = "{ uint32_t a = " + e + "; ";
                for (int i = 0; i < n; ++i) {
                    body += store ? st32("a", R(base + i)) + " " : R(base + i) + " = " + ld32("a") + "; ";
                    if (i + 1 < n) body += "if (" + flags("a") + " & rt::Cpu::BURST) a += 4; ";
                }
                body += "}";
                break;
            }
            case 0xc0: body = r + " = uint32_t(int32_t(int8_t(" + ld8(e) + ")));"; break;               // ldib
            case 0xc2: body = st8(e, r); break;                                                       // stib
            case 0xc8: body = r + " = uint32_t(int32_t(int16_t(" + ld16(e) + ")));"; break;             // ldis
            case 0xca: body = st16(e, r); break;                                                      // stis
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
            // addc: the silicon's carry out of a 33-bit sum (HANDOFF, Open
            // decisions). MAME sums in 32 bits and never sets carry; a trace
            // diff there is expected. Overflow as MAME computes it.
            case 0x5b0: body = "{ const uint32_t t1 = " + s1(in) + ", t2 = " + s2(in) +
                               "; const uint64_t res = uint64_t(t2) + uint64_t(t1) + ((AC >> 1) & 1); " + d +
                               " = uint32_t(res & 0xffffffff); AC &= ~0x3u; AC |= (res & (uint64_t(1) << 32)) ? 0x2u : 0u; "
                               "AC |= ((res ^ t1) & (res ^ t2) & 0x80000000) ? 1u : 0u; }"; break;
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
            case 0x66d: body = "c.flushreg();"; break; // flushreg (MAME's, src/runtime/cpu.cpp)
            case 0x670: body = "{ const uint64_t p = uint64_t(" + s1(in) + ") * uint64_t(" + s2(in) + "); " + d + " = uint32_t(p); " +
                               R(in.dst + 1) + " = uint32_t(p >> 32); }"; break; // emul
            case 0x671: {                                                  // ediv
                const std::string src2 = in.m2 ? "uint64_t(" + lit(in.src2) + ")"
                                               : "(uint64_t(" + R(in.src2) + ") | (uint64_t(" + R(in.src2 + 1) + ") << 32))";
                body = "{ const uint64_t a = " + s1(in) + ", b = " + src2 + "; const uint32_t rem = uint32_t(b % a), quo = uint32_t(b / a); " +
                       d + " = rem; " + R(in.dst + 1) + " = quo; }";
                break;
            }
            // FP, as MAME computes it: reals in host double (fp0-fp3 are
            // doubles, c.m_fp), singles in g/l registers through float, long
            // reals in register pairs; the operand forms of get_1_rif /
            // get_2_rif / set_rif and their ...rifl forms (see fsrc / fset).
            // Both sources are read before the destination is written.
            case 0x674: if (!fset("double(int32_t(" + s1(in) + "))", false)) return unsupported(in); break; // cvtir
            case 0x675: if (!fset("double(int32_t(" + s1(in) + "))", true)) return unsupported(in); break;  // cvtilr
            case 0x676: if (!fop2("t2 * std::pow(2.0, double(int32_t(" + s1(in) + ")))", true, false)) return unsupported(in); break; // scalerl
            case 0x677: if (!fop2("t2 * std::pow(2.0, double(int32_t(" + s1(in) + ")))", false, false)) return unsupported(in); break; // scaler
            case 0x680: case 0x690: if (!fop2("std::atan2(t2, t1)", code == 0x690)) return unsupported(in); break; // atanr(l)
            case 0x681: if (!fop2("t2 * std::log2(t1 + 1.0)", false)) return unsupported(in); break;              // logepr
            case 0x682: case 0x692: if (!fop2("t2 * std::log2(t1)", code == 0x692)) return unsupported(in); break; // logr(l)
            case 0x683: if (!fop2("std::fmod(t2, t1)", false)) return unsupported(in); break;                     // remr
            case 0x685: case 0x695: {                                                                              // cmpr(l)
                const bool l = code == 0x695;
                body = "{ const double t1 = " + fsrc(in.m1, in.src1, l) + ", t2 = " + fsrc(in.m2, in.src2, l) +
                       "; AC = (AC & ~7u) | gen::cc_d(t1, t2); }";
                break;
            }
            case 0x688: case 0x698: if (!fop1("std::sqrt(t1)", code == 0x698)) return unsupported(in); break;      // sqrtr(l)
            case 0x689: case 0x699: if (!fop1("std::pow(2.0, t1) - 1.0", code == 0x699)) return unsupported(in); break; // expr(l)
            case 0x68a: case 0x69a: if (!fop1("std::logb(t1)", code == 0x69a)) return unsupported(in); break;      // logbnr(l)
            case 0x68b: case 0x69b: if (!fop1("gen::round_to_int(t1, AC)", code == 0x69b)) return unsupported(in); break; // roundr(l)
            case 0x68c: case 0x69c: if (!fop1("std::sin(t1)", code == 0x69c)) return unsupported(in); break;       // sinr(l)
            case 0x68d: case 0x69d: if (!fop1("std::cos(t1)", code == 0x69d)) return unsupported(in); break;       // cosr(l)
            case 0x68e: case 0x69e: if (!fop1("std::tan(t1)", code == 0x69e)) return unsupported(in); break;       // tanr(l)
            case 0x6c0: case 0x6c1: case 0x6c2: case 0x6c3: {                // cvtri, cvtril, cvtzri, cvtzril
                if (in.m3) return unsupported(in);
                const bool round = code == 0x6c0 || code == 0x6c1, wide = code & 1;
                const std::string v = round ? "gen::round_to_int(t1, AC)" : "t1";
                body = "{ const double t1 = " + fsrc(in.m1, in.src1, false) + "; ";
                if (wide) // MAME set_ri64: the destination pair from dst as given
                    body += "const uint64_t v = uint64_t(int64_t(" + v + ")); " + d + " = uint32_t(v); " + R(in.dst + 1) +
                            " = uint32_t(v >> 32); }";
                else
                    body += d + " = uint32_t(int32_t(" + v + ")); }";
                break;
            }
            case 0x6c9: case 0x6d9: if (!fop1("t1", code == 0x6d9)) return unsupported(in); break;  // movr, movrl
            case 0x6e2: if (!fop2("t2 >= 0.0 ? std::fabs(t1) : -std::fabs(t1)", true)) return unsupported(in); break; // cpysre
            case 0x701: two("t2 * t1"); break;                             // mulo
            case 0x708: two("t2 % t1"); break;                             // remo (MAME: undefined on 0)
            case 0x70b: two("t1 == 0 ? 0u : t2 / t1"); break;              // divo (MAME: 0 on divide by zero)
            case 0x741: two("uint32_t(int32_t(t2) * int32_t(t1))"); break; // muli
            case 0x748: two("uint32_t(int32_t(t2) % int32_t(t1))"); break; // remi
            case 0x749: two("uint32_t([](int32_t a, int32_t b) { int32_t r = a - (a / b) * b; "
                            "if ((b ^ a) < 0 && r != 0) r += b; return r; }(int32_t(t2), int32_t(t1)))"); break; // modi (sign of src1)
            case 0x74b: two("uint32_t(int32_t(t2) / int32_t(t1))"); break; // divi
            case 0x78b: case 0x79b: if (!fop2("t2 / t1", code == 0x79b)) return unsupported(in); break; // divr(l)
            case 0x78c: case 0x79c: if (!fop2("t2 * t1", code == 0x79c)) return unsupported(in); break; // mulr(l)
            case 0x78d: case 0x79d: if (!fop2("t2 - t1", code == 0x79d)) return unsupported(in); break; // subr(l)
            case 0x78f: case 0x79f: if (!fop2("t2 + t1", code == 0x79f)) return unsupported(in); break; // addr(l)
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
        if (!block_counter.empty()) line(block_counter);
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

// --board's name as rt::BoardType (src/runtime/game.h).
std::string board_type(const std::string &name) { return name == "model2o" ? "rt::BoardType::Model2o" : "rt::BoardType::Model2a"; }

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
        std::fprintf(stderr, "usage: m2recomp PROGRAM.bin OUTDIR [--seeds FILE]... [--hooks FILE] [--chunk N] [--fast_inaccuracy]"
                              " [--no-mirror] [--alias GUEST:OFFSET:LENGTH]... [--profile] [--board model2o|model2a]\n");
        return 2;
    }
    const std::vector<uint8_t> img = load(argv[1]);
    const std::string out = argv[2];
    std::vector<uint32_t> seeds;
    size_t chunk = 1500;
    bool blocks = false, chain = false, mirror = true, profile = false;
    std::string board; // --board: rt::BoardType's name, inlined bus
    struct Alias {
        uint32_t guest, offset, length;
    };
    std::vector<Alias> aliases;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) {
            auto s = load_seeds(argv[++i]);
            seeds.insert(seeds.end(), s.begin(), s.end());
        } else if (!std::strcmp(argv[i], "--hooks") && i + 1 < argc) {
            load_hooks(argv[++i]);
        } else if (!std::strcmp(argv[i], "--chunk") && i + 1 < argc) {
            chunk = std::strtoul(argv[++i], nullptr, 0);
        } else if (!std::strcmp(argv[i], "--no-mirror")) {
            mirror = false;
        } else if (!std::strcmp(argv[i], "--alias") && i + 1 < argc) {
            Alias al{};
            if (std::sscanf(argv[++i], "%x:%x:%x", &al.guest, &al.offset, &al.length) != 3 || !al.length ||
                uint64_t(al.offset) + al.length > img.size()) {
                std::fprintf(stderr, "m2recomp: bad --alias %s (GUEST:OFFSET:LENGTH, hex, inside the image)\n", argv[i]);
                return 2;
            }
            aliases.push_back(al);
        } else if (!std::strcmp(argv[i], "--profile")) {
            profile = true;
        } else if (!std::strcmp(argv[i], "--board") && i + 1 < argc) {
            board = argv[++i];
            if (board != "model2o" && board != "model2a") {
                std::fprintf(stderr, "m2recomp: bad --board %s (model2o or model2a)\n", board.c_str());
                return 2;
            }
        } else if (!std::strcmp(argv[i], "--fast_inaccuracy")) {
            blocks = true; // 1. block bookkeeping
            chain = true;  // 2. direct chaining
        } else {
            std::fprintf(stderr, "m2recomp: unknown option %s (--blocks is now --fast_inaccuracy)\n", argv[i]);
            return 2;
        }
    }

    auto read = [&](uint32_t a) -> std::optional<uint32_t> {
        uint64_t off;
        if (a < img.size()) off = a;
        else if (mirror && a >= 0x220000 && a < 0x240000) off = a - 0x200000;
        else {
            const auto al = std::find_if(aliases.begin(), aliases.end(),
                                         [a](const Alias &x) { return a >= x.guest && a - x.guest < x.length; });
            if (al == aliases.end()) return std::nullopt;
            off = uint64_t(al->offset) + (a - al->guest);
        }
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

    // --fast_inaccuracy: block starts. An analysis pass over every instruction records
    // the direct transfer targets and the instructions that end a block
    // (--profile: the counted blocks, the same with or without --fast_inaccuracy).
    std::set<uint32_t> leaders;
    if (blocks || profile) {
        std::set<uint32_t> targets, none;
        std::vector<std::string> ignored; // reported by the real pass
        for (const Insn *in : insns) {
            Emitter dry{&none, {}, 0, &ignored};
            dry.targets = &targets;
            if (!dry.emit(*in) || dry.exits) leaders.insert(in->addr + in->length);
        }
        leaders.insert(targets.begin(), targets.end());
        leaders.insert(all.begin(), all.end());
        for (const auto &h : g_hooks) leaders.insert(h.first);
    }
    size_t block_count = 0;
    std::vector<rt::I960BlockInfo> prof_blocks; // --profile: every counted block, numbered across the chunks

    uint64_t native = 0;
    std::vector<std::string> missing;
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // per chunk: first, last address
    for (size_t ci = 0, start = 0; start < insns.size(); ++ci, start += chunk) {
        const size_t end = std::min(insns.size(), start + chunk);
        std::set<uint32_t> addrs;
        for (size_t k = start; k < end; ++k) addrs.insert(insns[k]->addr);
        Emitter em{&addrs, {}, 0, &missing};
        em.blocks = blocks;
        em.inline_bus = !board.empty();
        // Block structure (every instruction is its own block without --fast_inaccuracy):
        // lead[i] starts a block, pos[i] = instructions before it in its block,
        // len[i] = its block's length.
        const size_t n = end - start;
        std::vector<char> lead(n, 1), plead(n, 1); // plead: --profile's block starts
        std::vector<unsigned> pos(n, 0), len(n, 1);
        if (blocks || profile)
            for (size_t i = 1; i < n; ++i) {
                const Insn &prev = *insns[start + i - 1], &in = *insns[start + i];
                plead[i] = leaders.count(in.addr) || prev.addr + prev.length != in.addr;
            }
        if (blocks) {
            lead = plead;
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
        if (profile) em.o << "// --profile: block, dispatch and slow-path counters (M2_I960_PROFILE).\n";
        if (em.inline_bus) em.o << "// --board " << board << ": the board's bus inlined (gen::BoardMem).\n";
        em.o << "#include \"runtime/gen_support.h\"\n";
        if (profile) em.o << "#include \"runtime/i960_profile.h\"\n";
        if (em.inline_bus)
            em.o << "#ifndef M2_INLINE_BUS\n#error \"i960 code generated with m2recomp --board: build with M2_INLINE_BUS\"\n#endif\n";
        em.o << "\nnamespace gen {\n\n";
        if (em.inline_bus) em.o << "using Mem = gen::BoardMem<" << board_type(board) << ">;\n\n";
        if (blocks) {
            std::string b = kBlockBoundary;
            if (profile) {
                const std::string at = "    const uint64_t now = ls.count;\n";
                b.replace(b.find(at), 0, "    ++rt::i960gen::events;\n");
            }
            em.o << b;
        }
        em.o << "void " << name << "(Env &e) {\n"
             << "    rt::Cpu &c = e.c;\n    rt::Lockstep &ls = e.ls;\n"
             << "    uint32_t *const R = c.m_r;\n    uint32_t &AC = c.m_AC;\n";
        if (em.inline_bus) em.o << "    rt::M2Board &B = static_cast<rt::M2Board &>(*c.bus);\n";
        if (profile) em.o << "    uint64_t *const PH = rt::i960gen::block_hits;\n";
        em.o << "dispatch:\n";
        if (profile) em.o << "    ++rt::i960gen::dispatches;\n";
        em.o << "    if (ls.finished()) return;\n    switch (c.m_IP) {\n";
        for (size_t i = 0; i < n; ++i) {
            const uint32_t a = insns[start + i]->addr;
            em.o << "    case " << hex(a) << ": ";
            // Entered mid-block: the block's count is added at its end.
            if (!lead[i]) em.o << "ls.count -= " << pos[i] << "u; ";
            em.o << "goto " << lbl(a) << ";\n";
        }
        em.o << "    default: return;\n    }\n";
        for (size_t k = start; k < end; ++k) {
            const Insn &in = *insns[k];
            const size_t i = k - start;
            em.leader = lead[i] != 0;
            em.block_end = blocks && (i + 1 == n || lead[i + 1]) ? len[i] : 0;
            em.block_counter.clear();
            if (profile) {
                if (plead[i]) prof_blocks.push_back({in.addr, 0, {}});
                rt::I960BlockInfo &bi = prof_blocks.back();
                ++bi.length;
                const auto ops = classify(in);
                for (int op = 0; op < rt::kI960Ops; op++) bi.ops[op] = uint8_t(std::min(255, bi.ops[op] + ops[size_t(op)]));
                if (i + 1 == n || plead[i + 1]) em.block_counter = "++PH[" + std::to_string(prof_blocks.size() - 1) + "];";
            }
            const bool falls = em.emit(in);
            const uint32_t next = in.addr + in.length;
            const bool next_is_following = k + 1 < end && insns[k + 1]->addr == next;
            if (falls && !next_is_following) em.line("{ c.m_IP = " + hex(next) + "; goto dispatch; }");
        }
        em.o << "}\n\n} // namespace gen\n";
        std::ofstream(out + "/" + name + ".cpp") << em.o.str();
        native += em.native;
        ranges.emplace_back(insns[start]->addr, insns[end - 1]->addr);
    }

    // Dispatch table and instruction index.
    std::ostringstream t;
    t << "// Generated by m2recomp. Derived from the game: never commit.\n";
    if (blocks) t << "// --fast_inaccuracy: interrupt/event checks and the instruction count once per basic block.\n";
    if (chain) t << "// --fast_inaccuracy: gen::run chains chunks itself.\n";
    if (profile) t << "// --profile: block, dispatch and slow-path counters (M2_I960_PROFILE).\n";
    t << "#include \"runtime/gen_support.h\"\n";
    if (profile) t << "#include \"runtime/i960_profile.h\"\n";
    if (!board.empty())
        t << "#ifndef M2_INLINE_BUS\n#error \"i960 code generated with m2recomp --board: build with M2_INLINE_BUS\"\n#endif\n";
    t << "\n#include <algorithm>\n\n";
    if (profile) {
        t << "#ifndef M2_I960_PROFILE\n#error \"i960 code generated with m2recomp --profile: build with M2_I960_PROFILE\"\n#endif\n"
          << "namespace rt::i960gen {\n// Block: first address, instructions, contents (rt::I960Op order).\n"
          << "const I960BlockInfo blocks[] = {\n";
        for (const rt::I960BlockInfo &bi : prof_blocks) {
            t << "    {" << hex(bi.start) << ", " << bi.length << ", {";
            for (int k = 0; k < rt::kI960Ops; k++) t << (k ? "," : "") << unsigned(bi.ops[k]);
            t << "}},\n";
        }
        t << "};\nconst uint32_t block_count = " << prof_blocks.size() << ";\n"
          << "uint64_t block_hits[" << prof_blocks.size() << "];\nuint64_t dispatches, chunk_entries, events;\n"
          << "} // namespace rt::i960gen\n\n";
    }
    t << "namespace gen {\n\n";
    for (size_t i = 0; i < ranges.size(); ++i) t << "void chunk_" << (i < 10 ? "00" : i < 100 ? "0" : "") << i << "(Env &);\n";
    t << "\nnamespace {\nconst uint32_t kAddrs[] = {\n";
    for (const Insn *in : insns) t << "    " << hex(in->addr) << ",\n";
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
          << (profile ? "        ++rt::i960gen::chunk_entries;\n" : "")
          << "        r->fn(e);\n"
          << "        prev = r;\n"
          << "    } while (!e.ls.finished());\n"
          << "}\n\n";
    } else {
        t << "void run(Env &e) {\n    const uint32_t a = e.c.m_IP;\n"
          << "    for (const Range &r : kChunks)\n        if (a >= r.first && a <= r.last) { "
          << (profile ? "++rt::i960gen::chunk_entries; " : "") << "r.fn(e); return; }\n}\n\n";
    }
    t
      << "uint64_t native_instructions() { return " << native << "ull; }\n\n"
      << "extern const int inline_board = " << (board.empty() ? "-1" : "int(" + board_type(board) + ")") << ";\n\n"
      << "} // namespace gen\n";
    if (!missing.empty()) {
        std::fprintf(stderr, "m2recomp: FAILED: %zu reachable instructions have no native template:\n", missing.size());
        for (const std::string &m : missing) std::fprintf(stderr, "  %s\n", m.c_str());
        return 1;
    }
    std::ofstream(out + "/gen_table.cpp") << t.str();

    std::printf("m2recomp: %zu instructions from %zu seeds -> %zu chunks in %s, all native; %zu indirect sites\n",
                insns.size(), all.size(), ranges.size(), out.c_str(), r.indirect_sites.size());
    if (profile) std::printf("m2recomp: --profile: %zu blocks counted\n", prof_blocks.size());
    if (!board.empty()) std::printf("m2recomp: --board %s: bus inlined (gen::BoardMem, M2_INLINE_BUS)\n", board.c_str());
    if (blocks)
        std::printf("m2recomp: --fast_inaccuracy: %zu basic blocks, %.2f instructions per block (not interrupt-exact), "
                    "direct chunk chaining\n",
                    block_count, block_count ? double(insns.size()) / double(block_count) : 0.0);
    return 0;
}
