// Model 2B (Game::board Model2b): the same ports with an ADSP-21062 SHARC as
// the copro (MAME model2b_state copro_*): the recompiled SHARC program
// (m2sharcrecomp), run on demand as the TGP until it polls an empty input FIFO.
// The i960's side of the Model 2 geometry hardware: the TGP's FIFOs and
// control register, the geometrizer's command port, and buffer RAM, which
// all three write (MAME model2.cpp: copro_*, geo_*, push_geo_data). The TGP
// itself is the recompiled program (rt::tgpgen), run only when the i960
// needs its output: clockless, it returns when its input FIFO is empty.
// The inline FIFO handlers below are MAME's too (BSD-3-Clause, see
// tgp_board.cpp and THIRD_PARTY.md).
#pragma once

#include "runtime/romset/rom_source.h"
#include "runtime/tgp/tgp.h"
#include "runtime/tgp/tgp_profile.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace rt {

namespace sharc { class adsp21062_device; }
class SharcCopro;


class TgpBoard : public TgpBus {
public:
    // tables: copro_tgp_tables (0x40000 bytes); copro_data: 0x800000 bytes.
    TgpBoard(const std::vector<uint8_t> &tables, const std::vector<uint8_t> &copro_data);
    ~TgpBoard();
#ifdef M2_DC_MEMORY
    // copro_data read through pages (RomRegion::CoproData), not copied.
    TgpBoard(const std::vector<uint8_t> &tables, RomSource &rom);
#endif
#ifdef M2_FAST_TGP
    // The TGP holds pointers to in_ and out_: a copy would read the original's.
    TgpBoard(const TgpBoard &) = delete;
    TgpBoard &operator=(const TgpBoard &) = delete;
#endif

    // i960 accesses (dword offsets within each range, MAME's handlers). The
    // FIFO's are inline (the inlined bus, gen::BoardMem, calls them from the
    // generated code); the upload and the TGP run stay out of line.
    void function_port_w(uint32_t offset, uint32_t data) { // 0x00880000
        in_.push_back((data & 0x800fffff) | ((offset >> 2) & 0xff) << 23);
    }
    void fifo_w(uint32_t data) { // 0x00884000
        if (coproctl_ & 0x80000000) [[unlikely]] return upload_w(data);
        in_.push_back(data);
    }
    uint32_t fifo_r() { // 0x00884000
        if (out_.empty()) [[unlikely]] refill();
        const uint32_t v = out_.front();
        out_.pop_front();
        return v;
    }
    bool fifo_out_empty() { // 0x00980004 (after running the TGP)
        if (out_.empty()) need_output();
        return out_.empty();
    }
    void coproctl_w(uint32_t data, uint32_t mask);        // 0x00980000
    uint32_t coproctl_r() const { return coproctl_; }
    void geo_w(uint32_t offset, uint32_t data);           // 0x00800000
    uint32_t geo_r(uint32_t offset) const;
    void geo_prg_w(uint32_t data);                        // 0x00804000
    void geoctl_w(uint32_t data);                         // 0x00980008
    uint32_t buffer_r(uint32_t byte_offset) const { return buffer_[(byte_offset >> 2) & 0x7fff]; }
    void buffer_w(uint32_t byte_offset, uint32_t data, uint32_t mask);

    const uint32_t *buffer() const { return buffer_; }
    uint32_t *buffer_data() { return buffer_; }             // the geometrizer reads its display list here
    uint32_t geo_read_start() const { return geo_read_start_; }
    uint64_t tgp_instructions() const { return tgp_.count; }
    const uint32_t *table_data() const { return tables_.data(); } // copro_tgp_tables (TGP profiling's benchmark)
    bool booted() const { return booted_; }
    // Model 2B: the SHARC's IOP registers (0x008c0000, dword offset) and the
    // copro status (0x00980014: -1 until the program upload has started).
    void sharc_iop_w(uint32_t offset, uint32_t data);
    uint32_t copro_status_r() const { return upload_count_ == 0 ? 0xffffffffu : 0; }
    uint64_t sharc_instructions() const;
    bool has_sharc() const { return sharc_ != nullptr; }
    // Run the TGP until it waits on an empty input FIFO. The game polls the
    // TGP's mailbox in buffer RAM for results; the TGP must have caught up.
    void sync() { run_tgp(); }
    // The i960 reads buffer RAM: the TGP catches up (its mailbox); Model 2B's
    // SHARC, which runs behind the i960 in MAME, only moves on a slice.
    void sync_buffer_read();

    // TgpBus (the TGP's side)
    bool fifo_pop(uint32_t &v) override;
    void fifo_push(uint32_t v) override { out_.push_back(v); }
    uint32_t mem_r(uint32_t adr) override;
    void mem_w(uint32_t adr, uint32_t v) override;

private:
    void run_tgp();
    void refill();               // fifo_r on an empty output FIFO: run the TGP
    void upload_w(uint32_t data); // fifo_w while the TGP is held: its program
    void push_geo(uint32_t data);

    Tgp tgp_;
    std::vector<uint32_t> tables_, copro_data_;
#ifdef M2_DC_MEMORY
    RomSource *rom_ = nullptr;
#endif
#ifdef M2_FAST_TGP
    WordFifo in_, out_;
#else
    std::deque<uint32_t> in_, out_;
#endif
    std::vector<uint32_t> upload_;
    uint32_t coproctl_ = 0, geoctl_ = 0, geo_write_start_ = 0, geo_read_start_ = 0;
    bool booted_ = false;
    // Model 2B: the SHARC (null on the other boards) and the upload's word count.
    friend class SharcCopro;
    std::unique_ptr<SharcCopro> sharc_;
    uint32_t upload_count_ = 0;
    void run_sharc();
    void need_output(); // the i960 needs an output word: the TGP all, the SHARC until one
    void run_sharc_slice(uint64_t instructions);
    uint32_t buffer_[0x8000];
};

} // namespace rt

#include "runtime/cpu/replay_bus.h"

#include <string>

namespace rt {

// TgpBoard behind the replay bus: the i960's accesses to the geometry ports,
// TGP FIFOs and buffer RAM go to the native model. TGP output words are
// strict (must equal MAME's). Buffer RAM reads and FIFO-status polls depend
// on how far the TGP has run relative to the i960, so they are measured.
class TgpBoardModel : public DeviceModel {
    // Model 2B: the SHARC's IOP port and the copro status too (TgpBoard's sharc_).
public:
    explicit TgpBoardModel(TgpBoard &b) : b_(b) {}
    bool claims(uint32_t a) const override;
    Check read(uint32_t addr, uint32_t mask, uint32_t &value) override;
    void write(uint32_t addr, uint32_t data, uint32_t mask) override;
    void measured_mismatch(uint32_t addr, uint32_t ours, uint32_t mame) override;
    const uint8_t *region(uint32_t base) const override {
        return base == 0x00900000 ? reinterpret_cast<const uint8_t *>(b_.buffer()) : nullptr;
    }

    uint64_t fifo_words = 0, buffer_reads = 0, status_reads = 0;
    uint64_t buffer_mismatch = 0, status_mismatch = 0;
    std::string first_buffer_mismatch, first_status_mismatch;

private:
    TgpBoard &b_;
};

} // namespace rt
