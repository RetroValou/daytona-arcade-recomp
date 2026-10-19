// m2tgprecomp: static recompiler for the TGP (Fujitsu MB86234) program.
//
//   m2tgprecomp TGP_PROGRAM.bin OUT.cpp [--fast_inaccuracy] [--profile]
//
// The TGP has no ROM of its own: at boot the i960 uploads a program (2,024
// words for Daytona, stored in the game's data ROM; scripts/m2import.py
// extracts it). This tool turns that program into native C++: one label per
// word, each instruction's semantics inlined from src/runtime/tgp/tgp.h with its
// operand fields as template constants, direct gotos for constant branch
// targets and a dispatch switch for computed ones (brul/bsul/rtif).
//
// Every word gets a label, so a computed branch always lands on native code.
// Words that are not instructions MAME implements (tables, padding, forms it
// logs as unimplemented) compile to a hard error if reached. A reachability
// pass from the reset vector and every return address then checks that no
// such word is reachable by fall-through or a constant branch; if one is, the
// recompile fails. Nothing is interpreted at run time.
//
// --fast_inaccuracy (off by default; scripts/recompile.py passes it to every
// recompiler) adds a second copy of the program for the calls without an
// instruction budget, the only kind the game makes (TgpBoard::run_tgp and
// m2tgpcheck pass UINT64_MAX):
//   * the instruction count is added once per basic block instead of after
//     every instruction (t.count is read by nothing while the TGP runs);
//   * no budget test before each branch and at the dispatch (with UINT64_MAX
//     it can never fire).
// The count stays exact at every return: a stall in the middle of a block
// (input FIFO empty, the instruction runs again later) first adds the
// instructions of the block already completed, and an entry in the middle of
// a block through the dispatch switch (a return address, a computed branch,
// the resume after a stall) first subtracts the ones it skipped. The
// instruction after a rep is its own block (it counts once per repetition).
// The TGP takes no interrupts, so results, FIFO traffic and counts are those
// of the default output. A finite budget, or a hook (M2TGP_WITH_HOOK builds
// with Tgp::hook set: m2tgpcheck's per-instruction check), takes the default
// code, unchanged.
//
// --profile (scripts/recompile.py and build_vita.py --tgp-profile): counts
// the executions of every basic block (one increment at its end) and of the
// dispatch switch, and writes a table of what each block holds (FIFO words,
// I/O and memory accesses, ALU operations by kind, branches; TgpBlockInfo in
// src/runtime/tgp/tgp_profile.h). The code must be built with M2_TGP_PROFILE,
// whose runtime reports them. Results and counts are unchanged.
//
// The output is derived from the game: write it under build/ (git-ignored),
// never commit it.

#include "runtime/tgp/tgp.h"
#include "runtime/tgp/tgp_profile.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string hx(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%xu", v);
    return b;
}

uint32_t crc32(const std::vector<uint8_t> &d) {
    uint32_t c = 0xffffffffu;
    for (uint8_t x : d) {
        c ^= x;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

struct Insn {
    bool known = false;         // an instruction MAME implements
    std::string why;            // if not
    std::string body;           // semantics; may `return` on a FIFO stall
    bool is_rep = false;
    bool falls = true;          // execution can continue at pc+1
    int const_target = -1;      // taken branch to a constant address
    bool computed = false;      // taken branch to a computed address (t.pc)
    bool cond_branch = false;   // branch is conditional (else: always taken)
    bool pushes = false;        // bsif/bsul: pc+1 is a return address
    std::array<uint8_t, rt::kTgpOps> ops{}; // what it does (--profile's block contents)
};

std::string stall_check(uint32_t reg, uint32_t pc) {
    if ((reg & 0x3f) != 0x21) return "";
    // @STALL@: --fast_inaccuracy's count correction before a stall return.
    return " if (t.stall) { t.stall = false; t.pc = " + hx(pc) + ";@STALL@ return; }";
}

Insn decode(uint32_t pc, uint32_t o) {
    Insn in;
    const uint32_t type = (o >> 26) & 0x3f;
    auto bad = [&](const std::string &w) { in.known = false; in.why = w; return in; };
    in.known = true;
    // --profile's contents: register-file FIFO and bank accesses, memory, ALU.
    auto cnt = [&](rt::TgpOp k) { ++in.ops[k]; };
    auto rr = [&](uint32_t reg) { if ((reg & 0x3f) == 0x21) cnt(rt::TgpOpFifoIn); };
    auto wr = [&](uint32_t reg) {
        if ((reg & 0x3f) == 0x22) cnt(rt::TgpOpFifoOut);
        else if ((reg & 0x3f) == 0x23) cnt(rt::TgpOpBankW);
    };
    auto alu_ops = [&](uint32_t alu) {
        switch (alu) {
        case 0x00: break;
        case 0x05: case 0x06: case 0x07: case 0x0c: case 0x13: case 0x14: cnt(rt::TgpOpFpAdd); break;
        case 0x08: case 0x0d: cnt(rt::TgpOpFpMul); break;
        case 0x09: case 0x0a: cnt(rt::TgpOpFpAdd); cnt(rt::TgpOpFpMul); break;
        case 0x10: cnt(rt::TgpOpFpDiv); break;
        case 0x0f: cnt(rt::TgpOpFpToInt); break;
        case 0x0e: cnt(rt::TgpOpIntToFp); break;
        default: cnt(rt::TgpOpIntAlu); break;
        }
    };

    if (type == 0x00 || type == 0x07) {
        const uint32_t r1 = o & 0x1ff, r2 = (o >> 9) & 0x1ff, alu = (o >> 21) & 0x1f, op = (o >> 18) & 7;
        if (!rt::Tgp::alu_known(alu)) return bad("alu op " + hx(alu) + " unimplemented in MAME");
        const std::string A = hx(alu), R1 = hx(r1), R2 = hx(r2);
        std::string b = "t.alu_pre<" + A + ">();";
        alu_ops(alu);
        if (type == 0x00) { // lab
            std::string v1, v2;
            cnt(rt::TgpOpDataR);
            cnt(op == 0 || op == 1 ? rt::TgpOpIoR : rt::TgpOpDataR);
            if (op == 0 || op == 1) {
                v1 = "t.data_r(t.ea_pre_0<" + R1 + ">())";
                v2 = "t.io_r(t.ea_pre_1<" + R2 + ">())";
            } else if (op == 3) {
                v1 = "t.data_r(t.ea_pre_0<" + R1 + ">())";
                v2 = "t.data_r(uint16_t(t.ea_pre_1<" + R2 + ">() + 0x200))";
            } else if (op == 4) {
                v1 = "t.data_r(uint16_t(t.ea_pre_0<" + R1 + ">() + 0x200))";
                v2 = "t.data_r(t.ea_pre_1<" + R2 + ">())";
            } else return bad("lab subop " + hx(op) + " unimplemented in MAME");
            b += " { const uint32_t v1 = " + v1 + "; const uint32_t v2 = " + v2 + "; t.ea_post_0<" + R1 +
                 ">(); t.ea_post_1<" + R2 + ">(); t.a = v1; t.b = v2; } t.alu_post_1<" + A + ">(); t.alu_post_2<" + A + ">();";
            in.body = b;
            return in;
        }
        // ld / mov
        const std::string P1 = " t.alu_post_1<" + A + ">();", P2 = " t.alu_post_2<" + A + ">();";
        std::string m;
        switch (op) {
        case 0: case 1:
            cnt(rt::TgpOpDataR), cnt(rt::TgpOpIoW);
            m = " { const uint32_t v = t.data_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_io_1<" + R2 + ">(v); }";
            break;
        case 2:
            cnt(rt::TgpOpIoR), cnt(rt::TgpOpDataW);
            m = " { const uint32_t v = t.io_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, false); }";
            break;
        case 3:
            cnt(rt::TgpOpDataR), cnt(rt::TgpOpDataW);
            m = " { const uint32_t v = t.data_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, true); }";
            break;
        case 4:
            cnt(rt::TgpOpDataR), cnt(rt::TgpOpDataW);
            m = " { const uint32_t v = t.data_r(uint16_t(t.ea_pre_0<" + R1 + ">() + 0x200)); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, false); }";
            break;
        case 5:
            cnt(rt::TgpOpProgR), cnt(rt::TgpOpDataW);
            m = " { const uint32_t v = t.prog_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                " t.write_mem_internal_1<" + R2 + ">(v, false); }";
            break;
        case 7:
            switch (r2 >> 6) {
            case 0:
                rr(r2), cnt(rt::TgpOpDataW);
                m = " { const uint32_t v = t.read_reg<" + R2 + ">();" + stall_check(r2, pc) + P1 +
                    " t.write_mem_internal_1<" + R1 + ">(v, false); }";
                break;
            case 1:
                rr(r2), cnt(rt::TgpOpIoW);
                m = " { const uint32_t v = t.read_reg<" + R2 + ">();" + stall_check(r2, pc) + P1 +
                    " t.write_mem_io_1<" + R1 + ">(v); }";
                break;
            case 2:
                cnt(rt::TgpOpDataR), wr(r2);
                m = " { const uint32_t v = t.data_r(uint16_t(t.ea_pre_1<" + R1 + ">() + 0x200)); t.ea_post_1<" + R1 + ">();" +
                    P1 + " t.write_reg<" + R2 + ">(v); }";
                break;
            case 3:
                cnt(rt::TgpOpDataR), wr(r2);
                m = " { const uint32_t v = t.data_r(t.ea_pre_1<" + R1 + ">()); t.ea_post_1<" + R1 + ">();" + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            case 4:
                cnt(rt::TgpOpIoR), wr(r2);
                m = " { const uint32_t v = t.io_r(t.ea_pre_1<" + R1 + ">()); t.ea_post_1<" + R1 + ">();" + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            case 5:
                cnt(rt::TgpOpProgR), wr(r2);
                m = " { const uint32_t v = t.prog_r(t.ea_pre_0<" + R1 + ">()); t.ea_post_0<" + R1 + ">();" + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            case 6:
                rr(r1), wr(r2);
                m = " { const uint32_t v = t.read_reg<" + R1 + ">();" + stall_check(r1, pc) + P1 +
                    " t.write_reg<" + R2 + ">(v); }";
                break;
            default: return bad("ld/mov subop 7/" + hx(r2 >> 6) + " unimplemented in MAME");
            }
            break;
        default: return bad("ld/mov subop " + hx(op) + " unimplemented in MAME");
        }
        in.body = b + m + P2;
        return in;
    }
    if (type == 0x0d) {
        if (((o >> 17) & 7) != 5) return bad("0d subop unimplemented in MAME");
        in.body = "t.m = " + hx(o & 0xffff) + ";"; // stmh (m is 16 bits)
        return in;
    }
    if (type == 0x0e) {
        const uint32_t s = rt::Tgp::sext24(o);
        switch ((o >> 24) & 3) {
        case 0: in.body = "t.p = (t.p & 0xff000000u) | " + hx(o & 0xffffff) + ";"; break; // lipl
        case 1: in.body = "t.a = " + hx(s) + ";"; break;
        case 2: in.body = "t.b = " + hx(s) + ";"; break;
        case 3: in.body = "t.d = " + hx(s) + ";"; break;
        }
        return in;
    }
    if (type == 0x0f) {
        const uint32_t alu = (o >> 20) & 0x1f, sub2 = (o >> 17) & 7;
        if (!rt::Tgp::alu_known(alu)) return bad("alu op " + hx(alu) + " unimplemented in MAME");
        const std::string A = hx(alu);
        std::string b = "t.alu_pre<" + A + ">();";
        alu_ops(alu);
        switch (sub2) {
        case 0: // clr0
            if (o & 0x0004) b += " t.a = 0;";
            if (o & 0x0008) b += " t.b = 0;";
            if (o & 0x0010) b += " t.d = 0;";
            break;
        case 1: break; // clr1: MAME does nothing (flag mapping unknown)
        case 2: // rep: MAME skips the ALU post and the repeat check
            in.is_rep = true;
            cnt(rt::TgpOpRep);
            if (o & 0x8000) rr(o);
            if (o & 0x8000)
                in.body = b + " { const uint8_t n = uint8_t(t.read_reg<" + hx(o & 0x3f) + ">());" + stall_check(o, pc) + " t.r = n; }";
            else
                in.body = b + " t.r = " + hx(o & 0xff) + ";";
            return in;
        case 3: break; // set: MAME does nothing (flag mapping unknown)
        // 4-7: MAME logs "unimplemented opcode 0f/x" and runs the ALU part only, as
        // clr1 and set (vf2's program has one, 0x1b4: 3c0f0000).
        default: break;
        }
        in.body = b + " t.alu_post_1<" + A + ">();";
        return in;
    }
    if (type >= 0x10 && type <= 0x1f) { // ldi
        wr((o >> 24) & 0x3f);
        in.body = "t.write_reg<" + hx((o >> 24) & 0x3f) + ">(" + hx(rt::Tgp::sext24(o)) + ");";
        return in;
    }
    if (type == 0x2f || type == 0x3f) {
        const uint32_t cond = (o >> 20) & 0x1f, sub = (o >> 17) & 7, data = o & 0xffff;
        const bool invert = o & 0x40000000;
        if (!rt::Tgp::cond_known(cond)) return bad("condition " + hx(cond) + " unimplemented in MAME");
        const std::string C = hx(cond);
        const std::string ea = hx(o & 0x1ff);
        in.cond_branch = !(cond == 0x16 && !invert);
        cnt(sub == 1 || sub == 3 || sub == 5 ? rt::TgpOpComputed : rt::TgpOpBranch);
        if (sub == 1 || sub == 3) { if (o & 0x4000) rr(o); else cnt(rt::TgpOpDataR); }
        if (sub == 6) { cnt(rt::TgpOpDataR); wr((o >> 9) & 0x3f); }
        std::string b = "const bool c = " + std::string(invert ? "!" : "") + "t.cond<" + C + ">(); if (c) {";
        switch (sub) {
        case 0: b += " }"; in.const_target = int(data); break;
        case 2: b += " t.pc = " + hx(pc + 1) + "; t.pcs_push(); }"; in.const_target = int(data); in.pushes = true; break;
        case 1: case 3: {
            if (sub == 3) in.pushes = true;
            std::string push = sub == 3 ? " t.pc = " + hx(pc + 1) + "; t.pcs_push();" : "";
            if (o & 0x4000)
                b += " const uint32_t v = t.read_reg<" + hx(o & 0x3f) + ">();" + stall_check(o, pc) + push + " t.pc = uint16_t(v); }";
            else
                b += " const uint32_t v = t.data_r(t.ea_pre_0<" + ea + ">()); t.ea_post_0<" + ea + ">();" + push +
                     " t.pc = uint16_t(v); }";
            in.computed = true;
            break;
        }
        case 5: b += " t.pcs_pop(); }"; in.computed = true; break; // rtif
        case 6: // ldif: a conditional load, no transfer
            b += " const uint32_t v = t.data_r(t.ea_pre_0<" + ea + ">()); t.ea_post_0<" + ea + ">(); t.write_reg<" +
                 hx((o >> 9) & 0x3f) + ">(v); }";
            break;
        default: return bad("branch subtype " + hx(sub) + " unimplemented in MAME");
        }
        if (sub < 2) b += " t.count_down<" + C + ">();";
        in.body = b;
        in.falls = in.cond_branch || sub == 6;
        return in;
    }
    return bad("opcode type " + hx(type) + " unimplemented in MAME");
}

} // namespace

int main(int argc, char **argv) {
    bool fast = false, profile = false, usage = argc < 3;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--fast_inaccuracy")) fast = true;
        else if (!std::strcmp(argv[i], "--profile")) profile = true;
        else usage = true;
    }
    if (usage) {
        std::fprintf(stderr, "usage: m2tgprecomp TGP_PROGRAM.bin OUT.cpp [--fast_inaccuracy] [--profile]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    const std::vector<uint8_t> raw{std::istreambuf_iterator<char>(f), {}};
    // Empty: a board without a TGP (Model 2B's SHARC); every call is "control
    // outside the recompiled program".
    if (raw.size() % 4 || raw.size() > 0x4000) {
        std::fprintf(stderr, "m2tgprecomp: %s: bad program image\n", argv[1]);
        return 2;
    }
    const uint32_t n = uint32_t(raw.size() / 4);
    std::vector<uint32_t> w(n);
    for (uint32_t i = 0; i < n; i++)
        w[i] = raw[i * 4] | raw[i * 4 + 1] << 8 | raw[i * 4 + 2] << 16 | uint32_t(raw[i * 4 + 3]) << 24;

    std::vector<Insn> ins(n);
    for (uint32_t pc = 0; pc < n; pc++) ins[pc] = decode(pc, w[pc]);

    // Reachability by fall-through and constant branches from the reset
    // vector and every return address. Computed targets (brul/bsul through
    // registers or data RAM) are not followed; they land on a label anyway.
    std::set<uint32_t> seen;
    std::vector<uint32_t> work;
    if (n) work.push_back(0);
    for (uint32_t pc = 0; pc < n; pc++)
        if (ins[pc].known && ins[pc].pushes) work.push_back(pc + 1);
    std::vector<std::string> errors;
    while (!work.empty()) {
        const uint32_t pc = work.back();
        work.pop_back();
        if (!seen.insert(pc).second) continue;
        if (pc >= n) { errors.push_back("control reaches " + hx(pc) + ", past the program"); continue; }
        const Insn &in = ins[pc];
        if (!in.known) {
            char b[160];
            std::snprintf(b, sizeof b, "%03x: %08x: %s", pc, w[pc], in.why.c_str());
            errors.push_back(b);
            continue;
        }
        if (in.falls || in.is_rep) work.push_back(pc + 1);
        if (in.const_target >= 0) work.push_back(uint32_t(in.const_target));
    }
    if (!errors.empty()) {
        std::fprintf(stderr, "m2tgprecomp: %zu reachable word(s) without a native template:\n", errors.size());
        for (auto &e : errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }

    // Basic blocks (--fast_inaccuracy). A block starts at the reset vector, at
    // every constant branch target, after every transfer of control, at an
    // instruction a rep repeats and after it, and around words that are not
    // instructions. pos = instructions before it in its block, len = the
    // block's length (meaningful at its last instruction).
    std::vector<char> lead(n + 1, 0);
    lead[0] = 1;
    lead[n] = 1;
    for (uint32_t pc = 0; pc < n; pc++) {
        const Insn &in = ins[pc];
        if (!in.known) { lead[pc] = 1; lead[pc + 1] = 1; continue; }
        if (in.const_target >= 0 && uint32_t(in.const_target) < n) lead[uint32_t(in.const_target)] = 1;
        if (in.const_target >= 0 || in.computed) lead[pc + 1] = 1;
        if (in.is_rep) { lead[pc + 1] = 1; if (pc + 2 <= n) lead[pc + 2] = 1; }
    }
    std::vector<uint32_t> pos(n, 0), len(n, 1);
    size_t blocks = 0;
    for (uint32_t pc = 0, b = 0; pc < n; pc++) {
        if (lead[pc]) b = pc, ++blocks;
        pos[pc] = pc - b;
    }
    for (uint32_t pc = n; pc-- > 0;) len[pc] = !lead[pc + 1] ? len[pc + 1] : pos[pc] + 1;

    // --profile: each word's block and each block's contents.
    std::vector<uint32_t> block_of(n);
    std::vector<rt::TgpBlockInfo> block_info;
    for (uint32_t pc = 0; pc < n; pc++) {
        if (lead[pc]) block_info.push_back({uint16_t(pc), 0, {}});
        rt::TgpBlockInfo &bi = block_info.back();
        block_of[pc] = uint32_t(block_info.size() - 1);
        ++bi.length;
        for (int k = 0; k < rt::kTgpOps; k++) bi.ops[k] = uint8_t(std::min(255, bi.ops[k] + ins[pc].ops[size_t(k)]));
    }

    auto with_stall = [](std::string body, const std::string &fix) {
        for (size_t at; (at = body.find("@STALL@")) != std::string::npos;) body.replace(at, 7, fix);
        return body;
    };

    // One copy of the program. counted: the default (count per instruction,
    // budget, hook); otherwise --fast_inaccuracy's block-counted copy.
    auto emit_program = [&](std::ostringstream &o, bool counted) {
        o << "dispatch:\n";
        if (profile) o << "    ++dispatches;\n";
        if (counted) o << "    if (t.count >= budget) return;\n";
        o << "    switch (t.pc) {\n";
        for (uint32_t pc = 0; pc < n; pc++) {
            o << "    case " << hx(pc) << ": ";
            if (!counted && ins[pc].known && pos[pc]) o << "t.count -= " << pos[pc] << "u; ";
            o << "goto L_" << std::hex << pc << std::dec << ";\n";
        }
        o << "    default: t.fatal(t.pc, \"control outside the recompiled program\");\n    }\n";
        auto go = [&](uint32_t tgt) {
            std::ostringstream g;
            if (counted)
                g << "{ if (t.count >= budget) { t.pc = " << hx(tgt) << "; return; } goto L_" << std::hex << tgt << std::dec << "; }";
            else
                g << "goto L_" << std::hex << tgt << std::dec << ";";
            return g.str();
        };
        for (uint32_t pc = 0; pc < n; pc++) {
            const Insn &in = ins[pc];
            char head[64];
            std::snprintf(head, sizeof head, "L_%x: // %03x: %08x", pc, pc, w[pc]);
            o << head << (in.known ? "" : (" (" + in.why + ")")) << "\n";
            if (!in.known) {
                o << "    t.fatal(" << hx(pc) << ", \"word is not an instruction MAME implements\");\n";
                continue;
            }
            const bool after_rep = pc > 0 && ins[pc - 1].known && ins[pc - 1].is_rep;
            const bool block_end = lead[pc + 1] != 0;
            std::string count;
            if (counted) count = "++t.count; M2TGP_HOOK(" + hx(pc) + ");";
            else if (block_end) count = len[pc] == 1 ? "++t.count;" : "t.count += " + std::to_string(len[pc]) + "u;";
            if (profile && block_end) count += " ++block_hits[" + std::to_string(block_of[pc]) + "];";
            const std::string fix = counted || !pos[pc] ? "" : " t.count += " + std::to_string(pos[pc]) + "u;";
            o << "    { " << with_stall(in.body, fix) << "\n      " << count << "\n";
            if (after_rep) o << "      if (t.r != 1) { t.r--; goto L_" << std::hex << pc << std::dec << "; }\n";
            if (in.const_target >= 0) {
                if (in.cond_branch) o << "      if (c) " << go(uint32_t(in.const_target)) << "\n";
                else o << "      " << go(uint32_t(in.const_target)) << "\n";
            } else if (in.computed) {
                if (in.cond_branch) o << "      if (c) goto dispatch;\n";
                else o << "      goto dispatch;\n";
            }
            o << "    }\n";
            if (pc + 1 == n && (in.falls || in.is_rep)) o << "    t.pc = " << hx(n) << "; goto dispatch;\n";
        }
    };

    std::ostringstream o;
    o << "// Generated by m2tgprecomp from the TGP program the i960 uploads.\n"
         "// Derived from the game: never commit.\n";
    if (fast) o << "// --fast_inaccuracy: block-counted copy for the calls without a budget.\n";
    if (profile) o << "// --profile: block and dispatch counters (M2_TGP_PROFILE).\n";
    o << "#include \"runtime/tgp/tgp.h\"\n";
    if (profile) o << "#include \"runtime/tgp/tgp_profile.h\"\n";
    o << "\n"
         "// Test builds (M2TGP_WITH_HOOK) call Tgp::hook after every instruction.\n"
         "#ifdef M2TGP_WITH_HOOK\n#define M2TGP_HOOK(pc) if (t.hook) t.hook(t, pc)\n#else\n#define M2TGP_HOOK(pc)\n#endif\n\n"
         "namespace rt::tgpgen {\n\n"
      << "const uint32_t program_crc32 = " << hx(crc32(raw)) << ";\n"
      << "const uint32_t program_words = " << n << ";\n\n";
    if (profile) {
        o << "#ifndef M2_TGP_PROFILE\n#error \"TGP code generated with m2tgprecomp --profile: build with M2_TGP_PROFILE\"\n#endif\n"
          << "// Block: first word, instructions, contents (rt::TgpOp order).\n"
          << "const TgpBlockInfo blocks[] = {\n";
        for (const rt::TgpBlockInfo &bi : block_info) {
            o << "    {" << hx(bi.start) << ", " << bi.length << ", {";
            for (int k = 0; k < rt::kTgpOps; k++) o << (k ? "," : "") << unsigned(bi.ops[k]);
            o << "}},\n";
        }
        o << "};\nconst uint32_t block_count = " << block_info.size() << ";\n"
          << "uint64_t block_hits[" << block_info.size() << "];\nuint64_t dispatches;\n\n";
    }
    if (!fast) {
        o << "void run(Tgp &t, uint64_t budget) {\n";
        emit_program(o, true);
        o << "}\n\n} // namespace rt::tgpgen\n";
    } else {
        o << "namespace {\n\n"
          << "// Default code: count after every instruction, budget, hook.\n"
          << "void run_counted(Tgp &t, uint64_t budget) {\n";
        emit_program(o, true);
        o << "}\n\n"
          << "// --fast_inaccuracy: count once per basic block, no budget.\n"
          << "void run_unbounded(Tgp &t) {\n";
        emit_program(o, false);
        o << "}\n\n} // namespace\n\n"
          << "void run(Tgp &t, uint64_t budget) {\n"
          << "#ifdef M2TGP_WITH_HOOK\n"
          << "    if (budget == UINT64_MAX && !t.hook) { run_unbounded(t); return; }\n"
          << "#else\n"
          << "    if (budget == UINT64_MAX) { run_unbounded(t); return; }\n"
          << "#endif\n"
          << "    run_counted(t, budget);\n"
          << "}\n\n} // namespace rt::tgpgen\n";
    }

    std::ofstream out(argv[2]);
    out << o.str();
    size_t known = 0;
    for (auto &i : ins) known += i.known;
    std::printf("m2tgprecomp: %u words, %zu instructions MAME implements, %zu reachable by constant flow; all native\n", n,
                known, seen.size());
    if (profile) std::printf("m2tgprecomp: --profile: %zu blocks counted\n", block_info.size());
    if (fast)
        std::printf("m2tgprecomp: --fast_inaccuracy: %zu basic blocks, %.2f words per block, unbudgeted calls count per block\n",
                    blocks, double(n) / double(blocks));
    return 0;
}
