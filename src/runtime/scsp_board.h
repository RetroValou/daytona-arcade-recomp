// The Model 2A/2B/2C sound board as native runtime (design doc, Audio): the
// 68000 program recompiled by tools/m2sndrecomp --scsp, on its native
// context, with the SCSP (MAME's, transplanted: runtime/scsp.h) as native
// code. MAME's model2_scsp: a 68000 at 11.2896 MHz, 512 KiB of RAM it shares
// with the SCSP at 0x000000, the SCSP's registers at 0x100000, its program
// ROM at 0x600000 and the sample ROMs from 0x800000. The i960's UART drives
// the SCSP's MIDI input; the SCSP's MIDI output goes back to that UART.
//
// As the Model 1 board: no cycle clock. Time is counted in completed 68000
// instructions at the rate MAME's 68000 runs this program (kIps); events
// land on that count (a MIDI byte arriving, an SCSP timer expiring), and the
// SCSP renders up to the current count before each register access.
#pragma once

#include "runtime/scsp.h"
#include "runtime/snd_board.h"
#include "runtime/snd_gen_support.h"

#include <utility>
#include <vector>

namespace snd {

class ScspBoard : public Board, public Devices, public ScspHost {
public:
    // 68000 instructions per second of board time: MAME's 68000 (11.2896 MHz)
    // ran 25,842,220 instructions of srallyc's program in 1,800 frames at
    // 57.5242 Hz (attract).
    static constexpr uint64_t kIps = 825876;
    static constexpr uint32_t kBaud = 31250; // MIDI-rate serial line from the i960
    static constexpr uint32_t kByteBits = 10;

    ScspBoard(const std::vector<uint8_t> &program, const std::vector<uint8_t> &samples);

    // Board
    void send(const uint8_t *bytes, size_t n) override;
    void advance(double seconds) override;
    double fm_rate() const override { return Scsp::kRate; }
    double pcm_rate() const override { return Scsp::kRate; }
    std::vector<float> take_fm() override { return {}; }
    std::vector<float> take_pcm() override { return std::exchange(out_, {}); }
    std::vector<uint8_t> take_replies() override { return std::exchange(replies_, {}); }
    uint64_t instructions() const override { return sched_.count; }
    size_t bytes_received() const override { return received_; }

    // Devices (the 68000's view, outside RAM and program ROM)
    uint16_t read16(uint32_t addr) override;
    void write16(uint32_t addr, uint16_t data, uint16_t mask) override;

    // ScspHost
    void scsp_irq(int level, bool assert) override;
    void scsp_timer(int n, double seconds) override;
    void scsp_midi_out(uint8_t byte) override { replies_.push_back(byte); }

private:
    void run_to(uint64_t count);
    void render_to(uint64_t count);

    std::vector<uint8_t> program_, samples_;
    Cpu68k cpu_;
    Sched sched_{cpu_};
    sndgen::Env env_{cpu_, sched_};
    Scsp scsp_;
    double carry_ = 0; // fractional instructions from advance()
    uint8_t irq_lines_ = 0; // the 68000's asserted input lines, bit n = level n
    uint32_t timer_gen_[3] = {};
    uint64_t line_free_ = 0; // count at which the serial line is free for the next byte
    size_t received_ = 0;
    uint64_t done_ = 0;      // samples rendered
    std::vector<float> out_;
    std::vector<uint8_t> replies_;
};

} // namespace snd
