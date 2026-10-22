// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// Model 2 geometry ports as the i960 sees them, transplanted from MAME's
// src/mame/sega/model2.cpp (copro_ctl1_w, copro_function_port_w,
// copro_fifo_r/w, geo_r/w, geo_prg_w, geo_ctl1_w, push_geo_data,
// copro_tgp_memory_r/w) at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notice above kept as the licence requires). MAME's device
// FIFOs, halts and stalls are replaced by running the recompiled TGP on
// demand. See THIRD_PARTY.md.

#include "runtime/tgp/tgp_board.h"

#include "games/game.h"
#include "runtime/sharc/sharc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt {

namespace {
uint32_t crc32_words(const std::vector<uint32_t> &w) {
    uint32_t c = 0xffffffffu;
    for (uint32_t v : w)
        for (int b = 0; b < 4; b++) {
            c ^= (v >> (8 * b)) & 0xff;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
        }
    return ~c;
}

std::vector<uint32_t> words(const std::vector<uint8_t> &b) {
    std::vector<uint32_t> w(b.size() / 4);
    std::memcpy(w.data(), b.data(), w.size() * 4);
    return w;
}
} // namespace

// Model 2B's copro: the SHARC's data space above its internal memory (MAME
// model2b copro_sharc_map): 0x400000-0xbfffff the input FIFO (an empty FIFO
// reads 0, as MAME's), 0xc00000-0x13fffff the output FIFO, 0x1400000-0x1bfffff
// buffer RAM, 0x1c00000-0x1dfffff the copro data ROM. FLAG0 (input FIFO empty)
// follows the FIFO, as MAME's callbacks set it; FLAG1 stays 0 (MAME sets it to
// the input FIFO's fullness at 16 words, from the output FIFO's callbacks: our
// FIFO has no limit).
class SharcCopro : public sharc::Bus {
public:
    explicit SharcCopro(TgpBoard &b) : b_(b), sharc_(*this) {
        sharc_.set_flag_input(1, 0);
#if defined(M2_FAST_SHARC) && !defined(M2_DC_MEMORY)
        sharc_.set_direct_board({&b_.in_, &b_.out_, b_.buffer_, b_.copro_data_.data(), uint32_t(b_.copro_data_.size() - 1),
                                 &b_.stop_on_output_});
#endif
#ifdef M2_SHARC_PROFILE
        counts_.assign(0x8000, 0); // every program address the recompiled code can count
        sharc_.set_profile(counts_.data());
#endif
    }
    uint32_t read(uint32_t address) override {
        if (address >= 0x400000 && address < 0xc00000) {
            if (b_.in_.empty()) return M2SHARC_EV(fifo_in_empty), 0;
            M2SHARC_EV(fifo_in);
            const uint32_t v = b_.in_.front();
            b_.in_.pop_front();
            if (b_.in_.empty()) sharc_.set_flag_input(0, 1);
            return v;
        }
        if (address >= 0x1400000 && address < 0x1c00000) return M2SHARC_EV(buffer_r), b_.buffer_[address & 0x7fff];
        if (address >= 0x1c00000 && address < 0x1e00000) return M2SHARC_EV(rom_r), b_.mem_r(0x800000 | (address - 0x1c00000));
        return M2SHARC_EV(other), 0;
    }
    void write(uint32_t address, uint32_t data) override {
        if (address >= 0xc00000 && address < 0x1400000) {
            M2SHARC_EV(fifo_out);
            b_.out_.push_back(data);
            if (b_.stop_on_output_) sharc_.end_run(); // need_output: the word the i960 waits for
        }
        else if (address >= 0x1400000 && address < 0x1c00000) M2SHARC_EV(buffer_w), b_.buffer_[address & 0x7fff] = data;
        else M2SHARC_EV(other);
    }
    sharc::adsp21062_device &core() { return sharc_; }
#ifdef M2_SHARC_PROFILE
    const uint32_t *counts() const { return counts_.data(); }
#endif

private:
    TgpBoard &b_;
    sharc::adsp21062_device sharc_;
#ifdef M2_SHARC_PROFILE
    std::vector<uint32_t> counts_; // executions by program address - 0x20000
#endif
};

#ifdef M2_SHARC_PROFILE
// One caller's call (sharc_profile.h): the clock pair, the instructions.
namespace {
struct SharcCall {
    sharc::SharcCaller who;
    const sharc::adsp21062_device &s;
    uint64_t at = s.instructions(), t0 = sharc::sharc_profile.clock ? sharc::sharc_profile.clock() : 0;
    SharcCall(sharc::SharcCaller w, const sharc::adsp21062_device &core) : who(w), s(core) {}
    ~SharcCall() {
        sharc::SharcProfile &p = sharc::sharc_profile;
        ++p.calls[who];
        p.instructions[who] += s.instructions() - at;
        if (p.clock) p.ticks[who] += p.clock() - t0;
    }
};
} // namespace
#define M2SHARC_CALL(who) SharcCall sharc_call_{sharc::who, sharc_->core()}
#define M2SHARC_RUN(who) (++sharc::sharc_profile.runs[sharc::who])
#else
#define M2SHARC_CALL(who) ((void)0)
#define M2SHARC_RUN(who) ((void)0)
#endif

TgpBoard::~TgpBoard() = default;

uint64_t TgpBoard::sharc_instructions() const { return sharc_ ? sharc_->core().instructions() : 0; }

#ifdef M2_SHARC_PROFILE
const uint32_t *TgpBoard::sharc_counts() const { return sharc_ ? sharc_->counts() : nullptr; }
#endif

void TgpBoard::sharc_iop_w(uint32_t offset, uint32_t data) {
    sharc_starved_ = false;
    if (sharc_) sharc_->core().external_iop_write(offset, data);
}

// Until the SHARC polls an empty input FIFO (its wait loop, run_generated's
// "waiting"), or idles. 50 M instructions without waiting: a fault.
void TgpBoard::run_sharc() {
    const uint64_t t0 = clock_ ? clock_() : 0;
    M2SHARC_CALL(SharcToWait);
    run_sharc_to_wait();
    if (clock_) sharc_ticks_ += clock_() - t0;
}

void TgpBoard::run_sharc_to_wait() {
    sharc::adsp21062_device &s = sharc_->core();
    for (uint64_t done = 0; done < 50'000'000;) {
        s.set_flag_input(0, in_.empty() ? 1 : 0);
        const uint64_t at = s.instructions();
        M2SHARC_RUN(SharcToWait);
        const bool waiting = s.run_generated(1'000'000);
        done += s.instructions() - at;
        if (waiting && in_.empty()) { sharc_starved_ = true; return; }
        if (waiting && s.instructions() == at) return; // idle, or polling with input left: nothing more now
    }
    throw TgpFatal("the SHARC ran 50 M instructions without waiting for input");
}

TgpBoard::TgpBoard(const std::vector<uint8_t> &tables, const std::vector<uint8_t> &copro_data)
    : tables_(words(tables)), copro_data_(words(copro_data)) {
    if (tables_.size() != 0x10000 || copro_data_.size() != 0x200000) throw TgpFatal("bad TGP table or copro data image");
    if (game().board == BoardType::Model2b) sharc_ = std::make_unique<SharcCopro>(*this);
    tgp_.bus = this;
#ifdef M2_FAST_TGP
    tgp_.in_fifo = &in_;
    tgp_.out_fifo = &out_;
#endif
    tgp_.tables = tables_.data();
    // MAME machine_reset: "initialize bufferram to a sane default".
    for (auto &w : buffer_) w = 0x07800f0f;
}

#ifdef M2_DC_MEMORY
TgpBoard::TgpBoard(const std::vector<uint8_t> &tables, RomSource &rom) : tables_(words(tables)), rom_(&rom) {
    if (tables_.size() != 0x10000 || rom.size(RomRegion::CoproData) != 0x800000)
        throw TgpFatal("bad TGP table or copro data image");
    tgp_.bus = this;
#ifdef M2_FAST_TGP
    tgp_.in_fifo = &in_;
    tgp_.out_fifo = &out_;
#endif
    tgp_.tables = tables_.data();
    for (auto &w : buffer_) w = 0x07800f0f;
}
#endif

void TgpBoard::coproctl_w(uint32_t data, uint32_t mask) {
    if (sharc_ && (data ^ coproctl_) == 0x80000000) {
        // Model 2B (MAME model2b copro_halt: nothing; copro_boot: the SHARC's
        // halt line released). The SHARC checks its program memory against the
        // recompiled program on its first run.
        if (data & 0x80000000) upload_count_ = 0, booted_ = false;
        else sharc_->core().set_halted(false), booted_ = true;
        sharc_starved_ = false;
    } else if ((data ^ coproctl_) == 0x80000000) {
        if (data & 0x80000000) { // start upload: the TGP is held
            upload_.clear();
            booted_ = false;
        } else { // boot: the TGP starts from reset on the uploaded program
            if (upload_.size() != tgpgen::program_words || crc32_words(upload_) != tgpgen::program_crc32)
                throw TgpFatal("the i960 uploaded a TGP program other than the one recompiled");
            for (size_t i = 0; i < upload_.size(); i++) tgp_.prog[i] = upload_[i];
            tgp_.reset();
            booted_ = true;
        }
    }
    coproctl_ = (coproctl_ & ~mask) | (data & mask);
}

void TgpBoard::upload_w(uint32_t data) {
    if (sharc_) { // MAME model2b copro_fifo_w in boot mode
        sharc_->core().external_dma_write(upload_count_++, data & 0xffff);
        return;
    }
    if (upload_.size() >= 0x1000) throw TgpFatal("TGP program upload past 4K words");
    upload_.push_back(data);
}

void TgpBoard::run_tgp() {
    if (!booted_) return;
    if (sharc_) return run_sharc();
#ifdef M2_TGP_PROFILE
    TgpProfile &prof = tgp_profile;
    const uint64_t count = tgp_.count, start = prof.clock ? prof.clock() : 0;
#endif
    tgpgen::run(tgp_, UINT64_MAX); // until it waits on an empty input FIFO
#ifdef M2_TGP_PROFILE
    if (prof.clock) prof.ticks += prof.clock() - start;
    ++prof.runs;
    if (tgp_.count == count) ++prof.empty_runs;
#endif
}

// The TGP: until it waits (all its input processed). The SHARC (Model 2B) only
// until the word the i960 waits for is there: MAME's SHARC runs behind the
// i960, in slices, and the i960 stalled on the empty FIFO goes on once that
// word arrives. One run_generated call, ended by the output FIFO's write
// (end_run: at the recompiled code's next check, a plain run's end or a branch
// taken; 16-instruction calls before, ~1 us each on the Vita, 5,700 a frame). Run to the end instead, it processed commands the i960 had
// pushed before clearing the SHARC's work area in buffer RAM (Fighting
// Vipers' fight, 19 Oct 2026: word 0x7c5f, written by the SHARC, cleared by
// the i960 right after, read back by the SHARC 3,000 input words later; in
// MAME the clear comes first).
void TgpBoard::need_output() {
    if (!sharc_) return run_tgp();
    if (!booted_) return;
    const uint64_t t0 = clock_ ? clock_() : 0;
    M2SHARC_CALL(SharcNeedOutput);
    stop_on_output_ = true;
    for (uint64_t done = 0; out_.empty() && done < 50'000'000;) {
        sharc::adsp21062_device &s = sharc_->core();
        s.set_flag_input(0, in_.empty() ? 1 : 0);
        const uint64_t at = s.instructions();
        M2SHARC_RUN(SharcNeedOutput);
        const bool waiting = s.run_generated(1'000'000);
        done += s.instructions() - at;
        if (waiting && (in_.empty() || s.instructions() == at)) { // waits for input
            sharc_starved_ = in_.empty();
            break;
        }
    }
    stop_on_output_ = false;
    if (clock_) sharc_ticks_ += clock_() - t0;
}

void TgpBoard::run_sharc_slice(uint64_t instructions) {
    // Polling an empty input FIFO, it can only poll again: nothing to run (the
    // polls are not counted; ~1 us a call on the Vita, 380 a frame).
    if (!sharc_ || !booted_ || (sharc_starved_ && in_.empty())) return;
    sharc::adsp21062_device &s = sharc_->core();
    const uint64_t t0 = clock_ ? clock_() : 0;
    M2SHARC_CALL(SharcSlice);
    M2SHARC_RUN(SharcSlice);
    s.set_flag_input(0, in_.empty() ? 1 : 0);
    sharc_starved_ = s.run_generated(instructions) && in_.empty();
    if (clock_) sharc_ticks_ += clock_() - t0;
}

void TgpBoard::sync_buffer_read() {
    if (sharc_) run_sharc_slice(256);
    else run_tgp();
}

void TgpBoard::refill() {
    need_output();
    if (out_.empty()) throw TgpFatal("the i960 read the TGP's output FIFO, but the TGP has consumed all its input and produced nothing");
}

bool TgpBoard::fifo_pop(uint32_t &v) {
    if (in_.empty()) return false;
    v = in_.front();
    in_.pop_front();
    return true;
}

uint32_t TgpBoard::mem_r(uint32_t adr) {
#ifdef M2_TGP_PROFILE
    ++(adr & 0x800000 ? tgp_profile.bank_r_rom : adr & 0x400000 ? tgp_profile.bank_r_buffer : tgp_profile.bank_r_none);
#endif
#ifdef M2_DC_MEMORY
    if (adr & 0x800000) return rom_->dword(RomRegion::CoproData, (adr & 0x1fffff) * 4);
#else
    if (adr & 0x800000) return copro_data_[adr & (copro_data_.size() - 1)];
#endif
    if (adr & 0x400000) return buffer_[adr & 0x7fff];
    return 0;
}

void TgpBoard::mem_w(uint32_t adr, uint32_t v) {
#ifdef M2_TGP_PROFILE
    ++tgp_profile.bank_w;
#endif
    if (adr & 0x400000) buffer_[adr & 0x7fff] = v;
}

void TgpBoard::buffer_w(uint32_t byte_offset, uint32_t data, uint32_t mask) {
    // The TGP runs alongside the i960 in MAME: by the time the i960 writes to
    // buffer RAM, the TGP has processed the input it already had. Manx TT
    // writes the TGP's next display list pointer to word 0x6000 right after
    // the command that reads the previous one (19 Oct 2026): ours, run on
    // demand, would read the new pointer twice.
    // Not the SHARC (Model 2B): it runs behind the i960 (need_output).
    if (!sharc_ && !in_.empty()) run_tgp();
    uint32_t &w = buffer_[(byte_offset >> 2) & 0x7fff];
    w = (w & ~mask) | (data & mask);
}

void TgpBoard::push_geo(uint32_t data) {
    const uint32_t i = geo_write_start_ / 4;
    if (i >= 0x8000) throw TgpFatal("geometrizer push past the end of buffer RAM");
    buffer_[i] = data;
    geo_write_start_ += 4;
}

void TgpBoard::geoctl_w(uint32_t data) { geoctl_ = data; }

void TgpBoard::geo_prg_w(uint32_t data) {
    if (geoctl_ & 0x80000000) return; // geometrizer program upload: its own microcode, not modelled
    push_geo(data);
}

uint32_t TgpBoard::geo_r(uint32_t offset) const {
    const uint32_t address = offset * 4;
    if (address == 0x2008) return geo_write_start_;
    if (address == 0x3008) return geo_read_start_;
    return 0;
}

void TgpBoard::geo_w(uint32_t offset, uint32_t data) {
    const uint32_t address = offset * 4;
    if (address < 0x1000) {
        if (data & 0x80000000) {
            push_geo((data & 0x800fffff) | (((address >> 4) & 0x3f) << 23));
        } else if ((address & 0xf) == 0) {
            uint32_t r = (data & 0x000fffff) | (((address >> 4) & 0x3f) << 23);
            if (((address >> 4) & 0xc0) && ((address >> 4) & 0x3f) == 1) r |= ((address >> 10) & 3) << 29;
            push_geo(r);
        }
    } else if (address == 0x1008) {
        geo_write_start_ = data & 0xfffff;
    } else if (address == 0x3008) {
        geo_read_start_ = data & 0xfffff;
    } else {
        throw TgpFatal("geometrizer write to an unknown register");
    }
}

} // namespace rt

namespace rt {

namespace {
bool buffer_ram(uint32_t a) { return a >= 0x00900000 && a <= 0x0097ffff; }
} // namespace

bool TgpBoardModel::claims(uint32_t a) const {
    return (a >= 0x00800000 && a <= 0x00807fff) || (a >= 0x00884000 && a <= 0x00887fff) ||
           (a >= 0x00880000 && a <= 0x00883fff) || buffer_ram(a) || (a >= 0x00980000 && a <= 0x0098000b) ||
           (b_.has_sharc() && ((a >= 0x008c0000 && a <= 0x008c0fff) || a == 0x00980014 || a == 0x00980020)); // Model 2B
}

DeviceModel::Check TgpBoardModel::read(uint32_t addr, uint32_t mask, uint32_t &value) {
    if (buffer_ram(addr)) {
        ++buffer_reads;
        value = b_.buffer_r(addr & 0x1ffff);
        return Measured;
    }
    if (addr >= 0x00884000 && addr <= 0x00887fff) {
        // MAME's copro_fifo_r is a 32-bit handler: a 16-bit read still pops a
        // whole word and the bus keeps the lane asked for (races do this).
        (void)mask;
        ++fifo_words;
        value = b_.fifo_r();
        return Strict;
    }
    if (addr >= 0x00800000 && addr <= 0x00803fff) { value = b_.geo_r((addr - 0x00800000) >> 2); return Strict; }
    if (addr >= 0x00804000 && addr <= 0x00807fff) { value = 0xffffffffu; return Strict; } // geo_prg_r
    if (addr == 0x00980000) { value = b_.coproctl_r(); return Strict; }
    if (addr == 0x00980014) { value = b_.copro_status_r(); return Strict; } // Model 2B
    if (addr == 0x00980004) {
        ++status_reads;
        value = b_.fifo_out_empty() ? 1 : 0;
        return Measured;
    }
    value = 0; // write-only ports (function port, geoctl) read as unmapped
    return Strict;
}

void TgpBoardModel::write(uint32_t addr, uint32_t data, uint32_t mask) {
    if (buffer_ram(addr)) { b_.buffer_w(addr & 0x1ffff, data, mask); return; }
    if (addr == 0x00980000) { b_.coproctl_w(data, mask); return; }
    // The other ports are MAME 32-bit handlers without a mask: a narrower
    // write reaches them as the whole dword, lanes not written being zero.
    data &= mask;
    if (addr >= 0x00884000 && addr <= 0x00887fff) b_.fifo_w(data);
    else if (addr >= 0x00880000 && addr <= 0x00883fff) b_.function_port_w((addr - 0x00880000) >> 2, data);
    else if (addr >= 0x00804000 && addr <= 0x00807fff) b_.geo_prg_w(data);
    else if (addr >= 0x00800000 && addr <= 0x00803fff) b_.geo_w((addr - 0x00800000) >> 2, data);
    else if (addr == 0x00980008) b_.geoctl_w(data);
    else if (addr >= 0x008c0000 && addr <= 0x008c0fff) b_.sharc_iop_w((addr & 0xfff) >> 2, data); // Model 2B
    // 0x00980020 (Model 2B's upload bank register) ignores writes, as MAME's noprw
    // 0x00980004 (read-only status) ignores writes
}

void TgpBoardModel::measured_mismatch(uint32_t addr, uint32_t ours, uint32_t mame) {
    char b[96];
    std::snprintf(b, sizeof b, "%08x: ours %08x, MAME %08x (TGP at %llu instructions)", addr, ours, mame,
                  (unsigned long long)b_.tgp_instructions());
    static FILE *log = std::getenv("M2NATIVE_MISMATCH_LOG") ? std::fopen(std::getenv("M2NATIVE_MISMATCH_LOG"), "w") : nullptr;
    if (log) std::fprintf(log, "%s\n", b);
    if (buffer_ram(addr)) {
        if (!buffer_mismatch++) first_buffer_mismatch = b;
    } else {
        if (!status_mismatch++) first_status_mismatch = b;
    }
}

} // namespace rt
