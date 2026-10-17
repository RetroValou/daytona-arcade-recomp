// Support for recompiled i960 code (tools/m2recomp output). Generated code is
// native C++: every instruction's semantics are emitted inline with operands
// resolved at recompile time. It calls into the runtime only for memory
// access, call/return frame management and interrupt entry, as a CPU would.
#pragma once

#include "runtime/cpu/cpu.h"
#include "games/hooks.h"
#include "runtime/cpu/lockstep.h"
#ifdef M2_INLINE_BUS
#include "runtime/board/m2_board.h"
#endif

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gen {

struct Env {
    rt::Cpu &c;
    rt::Lockstep &ls;
};

// Condition codes, as MAME's cmp_u / cmp_s / concmp_u / concmp_s.
inline uint32_t cc_u(uint32_t a, uint32_t b) { return a < b ? 4u : a == b ? 2u : 1u; }
inline uint32_t cc_s(int32_t a, int32_t b) { return a < b ? 4u : a == b ? 2u : 1u; }
inline uint32_t cc_d(double a, double b) { return a < b ? 4u : a == b ? 2u : a > b ? 1u : 0u; }

// FP with MAME's host-double semantics (lockstep against MAME). The shipped
// path will use the proven native paths in i960/fp.h instead.
inline double round_to_int(double v, uint32_t ac) {
    switch ((ac >> 30) & 3) {
    case 0: return std::round(v);
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}
inline uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }

#ifdef M2_FAST_GEN
// Work RAM at a fixed offset, for the rewritten code (the Dreamcast's and the
// Vita's: scripts/fast_gen.py): what the bus does for these
// addresses (plain RAM, little-endian, aligned), without the calls.
inline uint32_t wram_r32(const rt::Cpu &c, uint32_t o) { uint32_t v; std::memcpy(&v, M2_AL(c.work_ram + o, 4), 4); return v; }
inline uint16_t wram_r16(const rt::Cpu &c, uint32_t o) { uint16_t v; std::memcpy(&v, M2_AL(c.work_ram + o, 2), 2); return v; }
inline uint8_t wram_r8(const rt::Cpu &c, uint32_t o) { return c.work_ram[o]; }
inline void wram_w32(rt::Cpu &c, uint32_t o, uint32_t v) { std::memcpy(M2_AL(c.work_ram + o, 4), &v, 4); }
inline void wram_w16(rt::Cpu &c, uint32_t o, uint16_t v) { std::memcpy(M2_AL(c.work_ram + o, 2), &v, 2); }
inline void wram_w8(rt::Cpu &c, uint32_t o, uint8_t v) { c.work_ram[o] = v; }
#endif
#ifdef M2_INLINE_BUS
// The bus inlined for one board (m2recomp --board; code generated with it
// says `using Mem = gen::BoardMem<board>` and calls Mem::ld32(B, a)... with B the
// rt::M2Board behind c.bus). Three stages:
//  1. here, inline at every access: work RAM (0x00500000-0x005fffff, every
//     board), the program ROM (0-0x1fffff) and main RAM (0x00200000: 128 KB
//     on Model 2, then the ROM's upper 128 KB mirrored at 0x00220000; 256 KB
//     on 2A-CRX);
//  2. aligned dwords to the TGP (tgp_rd/tgp_wr): its FIFOs, the function
//     port, the FIFO status and buffer RAM, straight to rt::TgpBoard's inline
//     handlers, as M2Board::dev_read/dev_write call them; then
//     M2Board::read_dword_mid (word, byte): one copy, a direct call (the
//     data ROM);
//  3. M2Board::read_dword and the rest: the bus as before (devices, video;
//     the TGP's other registers and its byte and word accesses).
// The same bytes as the page table gives for these ranges (plain memory,
// little-endian); writes to them skip ram_written, which does nothing there.
// Every range starts and ends on a 128 KB boundary: an unaligned access
// that does not cross one (in()) is in one range, and its bytes one by one
// (Cpu::i960_read_dword_unaligned) are the little-endian word read here.
// The frame-wait byte (0x00500000, ldob) still goes through read_byte.
// GameLoop checks at start that the board is the one the code was made for.
template <rt::BoardType K> struct BoardMem {
    static constexpr uint32_t kMainRam = K == rt::BoardType::Model2o ? 0x20000u : 0x40000u;
    // Stage 1: plain memory behind an address, or null.
    static const uint8_t *rd(rt::M2Board &b, uint32_t a) {
        if (const uint32_t o = a - 0x00500000u; o < 0x100000u) return b.work_ram() + o;
        if (a < 0x00200000u) return b.program() + a;
        const uint32_t o = a - 0x00200000u;
        if (o < kMainRam) return b.main_ram() + o;
        if constexpr (K == rt::BoardType::Model2o)
            if (o < 0x40000u) return b.program() + o; // the ROM mirror (0x00220000 = ROM 0x20000)
        return nullptr;
    }
    static uint8_t *wr(rt::M2Board &b, uint32_t a) {
        if (const uint32_t o = a - 0x00500000u; o < 0x100000u) return b.work_ram() + o;
        if (const uint32_t o = a - 0x00200000u; o < kMainRam) return b.main_ram() + o;
        return nullptr;
    }
    // n bytes from a stay inside one 128 KB block
    static bool in(uint32_t a, uint32_t n) { return (a & 0x1ffffu) <= 0x20000u - n; }
    // Stage 2, the TGP: an aligned dword at 0x00880000-0x00980fff. The output
    // FIFO (0x00884000-0x00887fff), its status (0x00980004: 1 when empty after
    // running the TGP) and buffer RAM (0x00900000-0x0097ffff, the TGP's
    // mailbox: run it first), as dev_read; false: not one of them.
    static bool tgp_rd(rt::M2Board &b, uint32_t a, uint32_t &v) {
        if (a - 0x00880000u >= 0x101000u) return false;
        rt::TgpBoard &t = b.tgp();
        if ((a & 0xffffc000u) == 0x00884000u) return v = t.fifo_r(), true;
        if (const uint32_t o = a - 0x00900000u; o < 0x80000u) return t.sync(), v = t.buffer_r(o & 0x1ffffu), true;
        if (a == 0x00980004u) return v = t.fifo_out_empty() ? 1 : 0, true;
        return false;
    }
    // The input FIFO (0x00884000-0x00887fff; the TGP's program while it is
    // held), the function port (0x00880000-0x00883fff) and buffer RAM, as
    // dev_write with every lane written.
    static bool tgp_wr(rt::M2Board &b, uint32_t a, uint32_t v) {
        if (a - 0x00880000u >= 0x100000u) return false;
        rt::TgpBoard &t = b.tgp();
        if ((a & 0xffff8000u) == 0x00880000u) {
            if (a & 0x4000u) t.fifo_w(v);
            else t.function_port_w((a - 0x00880000u) >> 2, v);
            return true;
        }
        if (const uint32_t o = a - 0x00900000u; o < 0x80000u) return t.buffer_w(o & 0x1ffffu, v, 0xffffffffu), true;
        return false;
    }
    static uint32_t ld32(rt::M2Board &b, uint32_t a) {
        if (a & 3) [[unlikely]] {
            if (const uint8_t *p = in(a, 4) ? rd(b, a) : nullptr) { uint32_t v; std::memcpy(&v, p, 4); return v; }
            return b.read_dword_unaligned(a);
        }
        uint32_t v;
        if (const uint8_t *p = rd(b, a)) { std::memcpy(&v, p, 4); return v; }
        if (tgp_rd(b, a, v)) return v;
        return b.read_dword_mid(a);
    }
    static uint16_t ld16(rt::M2Board &b, uint32_t a) {
        if (a & 1) [[unlikely]] {
            if (const uint8_t *p = in(a, 2) ? rd(b, a) : nullptr) { uint16_t v; std::memcpy(&v, p, 2); return v; }
            return b.read_word_unaligned(a);
        }
        if (const uint8_t *p = rd(b, a)) { uint16_t v; std::memcpy(&v, p, 2); return v; }
        return b.read_word_mid(a);
    }
    static uint8_t ld8(rt::M2Board &b, uint32_t a) {
        if (a == 0x00500000u) return b.read_byte(a);
        if (const uint8_t *p = rd(b, a)) return *p;
        return b.read_byte_mid(a);
    }
    static void st32(rt::M2Board &b, uint32_t a, uint32_t v) {
        if (uint8_t *p = (a & 3) == 0 || in(a, 4) ? wr(b, a) : nullptr) return void(std::memcpy(p, &v, 4));
        if (a & 3) [[unlikely]] return b.write_dword_unaligned(a, v);
        if (tgp_wr(b, a, v)) return;
        b.write_dword(a, v);
    }
    static void st16(rt::M2Board &b, uint32_t a, uint16_t v) {
        if (uint8_t *p = (a & 1) == 0 || in(a, 2) ? wr(b, a) : nullptr) return void(std::memcpy(p, &v, 2));
        if (a & 1) [[unlikely]] return b.write_word_unaligned(a, v);
        b.write_word(a, v);
    }
    static void st8(rt::M2Board &b, uint32_t a, uint8_t v) {
        if (uint8_t *p = wr(b, a)) return void(*p = v);
        b.write_byte(a, v);
    }
    // Handler flags (ldl/ldt/ldq, stl/stt/stq): every stage 1 range and buffer
    // RAM are BURST; the TGP's ports (0x00880000-0x00887fff) are not.
    static uint16_t flags(rt::M2Board &b, uint32_t a) {
        if (a - 0x00500000u < 0x100000u || a < 0x00240000u || a - 0x00900000u < 0x80000u) return rt::Cpu::BURST;
        if (a - 0x00880000u < 0x8000u) return 0;
        return b.flags(a);
    }
};
#endif
// The board the i960 code was generated for (m2recomp --board: an
// rt::BoardType, its bus inlined), or -1 (any Bus, through Cpu::bus).
extern const int inline_board; // generated
inline float u2f(uint32_t u) { return std::bit_cast<float>(u); }
inline double u2d(uint64_t u) { return std::bit_cast<double>(u); }
inline uint64_t d2u(double d) { return std::bit_cast<uint64_t>(d); }

// Recompiled code entry points (generated): run from c.m_IP until it leaves
// recompiled code or the lockstep run ends.
bool has_code(uint32_t addr);   // generated
void run(Env &e);               // generated: dispatches into the chunk holding c.m_IP
uint64_t native_instructions(); // generated: instructions recompiled (all of them: there is no fallback)

} // namespace gen
