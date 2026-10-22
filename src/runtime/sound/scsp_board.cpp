#include "runtime/sound/scsp_board.h"

#include "games/game.h"
#include "runtime/sound/model1_board.h"
#include "runtime/sound/sound_profile.h"

#include <algorithm>
#include <cstdio>

namespace snd {

double board_fm_rate() { return rt::game().sound == rt::SoundBoardType::Scsp ? Scsp::kRate : SoundBoard::kYmClock / 144.0; }
double board_pcm_rate() { return rt::game().sound == rt::SoundBoardType::Scsp ? Scsp::kRate : SoundBoard::kPcmClock / 224.0; }

ScspBoard::ScspBoard(const std::vector<uint8_t> &program, const std::vector<uint8_t> &samples,
                     const rt::ScspVoices *voices, uint16_t music_channels)
    : program_(program), samples_(samples), voices_(voices), music_channels_(music_channels), scsp_(*this, (cpu_.ram.assign(0x80000, 0), cpu_.ram.data())) {
    if (program_.size() < 0x80000 || samples_.size() < 0x800000)
        throw rt::Fatal("SCSP sound board images missing or short (re-run the importer)");
    cpu_.map = kScspMap;
    cpu_.rom = program_.data();
    cpu_.dev = this;
    // MAME reset_model2_scsp: the 68000's vector table's first 16 bytes
    // (stack and reset PC) copied from the program ROM into RAM
    std::copy_n(program_.begin(), 16, cpu_.ram.begin());
    cpu_.reset();
}

void ScspBoard::send(const uint8_t *bytes, size_t n) {
    constexpr uint64_t kByteTime = kIps * kByteBits / kBaud; // instructions per byte on the line
    for (size_t i = 0; i < n; ++i) {
        const uint64_t at = std::max(line_free_, sched_.count) + kByteTime;
        line_free_ = at;
        const uint8_t b = bytes[i];
        sched_.at(at, [this, b] {
            render_to(sched_.count);
            scsp_.midi_in(b);
            ++received_;
        });
    }
}

void ScspBoard::advance(double seconds) {
    const double want = seconds * double(kIps) + carry_;
    const uint64_t n = uint64_t(want);
    carry_ = want - double(n);
#ifdef M2_SOUND_PROFILE
    SoundProfile &p = sound_profile();
    const uint64_t t0 = p.clock ? p.clock() : 0, count0 = sched_.count;
    run_to(sched_.count + n);
    ++p.frames;
    p.m68k_instructions += sched_.count - count0;
    if (p.clock) p.advance_ticks += p.clock() - t0;
#else
    run_to(sched_.count + n);
#endif
}

void ScspBoard::run_to(uint64_t count) {
    while (sched_.count < count) {
        sched_.end_count = count;
        if (!sndgen::has_code(cpu_.pc)) {
            char b[96];
            std::snprintf(b, sizeof b, "sound 68000: no recompiled code at %06x", cpu_.pc);
            throw rt::Fatal(b);
        }
        sndgen::run(env_);
    }
    render_to(count);
}

void ScspBoard::render_to(uint64_t count) {
    const uint64_t due = samples_at_.at(count); // count * Scsp::kRate / kIps
#ifdef M2_SOUND_PROFILE
    SoundProfile &p = sound_profile();
    ++p.render_calls;
#endif
    if (due <= done_) return;
    const size_t at = out_.size(), n = size_t(due - done_);
#ifdef M2_SOUND_PROFILE
    // One render in kRenderEvery timed (sound_profile.h).
    p.timing_render = p.clock && ++p.render_phase == p.kRenderEvery;
    if (p.timing_render) p.render_phase = 0;
    const uint64_t t0 = p.timing_render ? p.clock() : 0;
#endif
    out_.resize(at + 2 * n);
    scsp_.render(out_.data() + at, int(n));
    done_ = due;
#ifdef M2_SOUND_PROFILE
    ++p.render_runs;
    p.samples += n;
    if (p.timing_render) {
        p.render_ticks += p.clock() - t0;
        ++p.render_timed;
        p.render_timed_samples += n;
        p.timing_render = false;
    }
#endif
}

uint16_t ScspBoard::read16(uint32_t addr) {
    if (addr >= 0x100000 && addr <= 0x100fff) {
#ifdef M2_SOUND_PROFILE
        ++sound_profile().scsp_reads;
#endif
        render_to(sched_.count); // MAME: m_stream->update()
        return scsp_.read((addr - 0x100000) >> 1);
    }
    if (addr >= 0x800000) { // samples: 0x800000 on, the banks at their reset bases (an 8 MiB set is not banked)
        const uint32_t o = addr - 0x800000;
        return o + 1 < samples_.size() ? uint16_t(samples_[o] << 8 | samples_[o + 1]) : 0;
    }
    return 0;
}

void ScspBoard::write16(uint32_t addr, uint16_t data, uint16_t mask) {
    if (addr >= 0x100000 && addr <= 0x100fff) {
#ifdef M2_SOUND_PROFILE
        ++sound_profile().scsp_writes;
#endif
        render_to(sched_.count);
        scsp_.write((addr - 0x100000) >> 1, data, mask);
    }
    // 0x400000: model2snd_ctrl, sample banking for sets over 8 MiB only;
    // writes elsewhere (the program ROM) are ignored, as MAME's
}

bool ScspBoard::scsp_slot_effect(int slot) {
    if (!voices_) return false;
    const uint32_t a = voices_->table + uint32_t(slot) * voices_->entry + voices_->channel;
    if (a >= cpu_.ram.size()) return false;
    const uint8_t channel = cpu_.ram[a];
    return !(channel < 16 && (music_channels_ >> channel & 1)); // rt::Game::music_channels
}

void ScspBoard::scsp_irq(int level, bool assert) {
    if (level <= 0 || level > 7) return;
    if (assert) irq_lines_ = uint8_t(irq_lines_ | 1u << level);
    else irq_lines_ = uint8_t(irq_lines_ & ~(1u << level));
    int top = 0;
    for (int l = 7; l > 0; --l)
        if (irq_lines_ & (1u << l)) {
            top = l;
            break;
        }
    cpu_.irq_line = top;
    sched_.poke();
}

void ScspBoard::scsp_timer(int n, double seconds) {
    const uint32_t gen = ++timer_gen_[n];
    const uint64_t at = sched_.count + std::max<uint64_t>(1, uint64_t(seconds * double(kIps) + 0.5));
    sched_.at(at, [this, n, gen] {
        if (timer_gen_[n] != gen) return; // restarted since
        render_to(sched_.count);
        scsp_.timer_expired(n);
    });
}

} // namespace snd
