#pragma once
#include "core_policy.h"
#include <SDL.h>
#include "runtime/sound_board.h"
#include "audio_rate.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vita {
// The SDL playback callback consumes only converted samples. The sound worker
// may call push(); join it before main changes settings, pauses or closes audio.
// SDL's device lock serializes stream conversion with the playback callback.
// Output pacing (cushion kept by a slight speed change, silent re-priming after an
// underrun): audio_rate.h.
// One rate conversion, in the callback: the board's samples are queued at their own
// rate (FrameRing: a copy, ~1 frame of audio per emulated frame) and the callback's
// StereoResampler takes them straight to the device rate, its ratio the rates' quotient
// times AudioRate's speed correction. Before, SDL_AudioStreamPut resampled them to
// 48 kHz on the sound core (its own filter: ~1.6 ms per frame there in VF2's perf.log)
// and the callback interpolated them a second time for the speed correction.
// Silent FM is not queued (Daytona's sound program leaves the YM3438 silent: 0 non-zero
// FM frames over whole races): the PCM queue sets the pace; the FM queue holds only the
// FM that was not silent, aligned on the head of the PCM queue (padded with silence when
// it starts again), and the callback adds whatever FM it has.
class Audio {
public:
    struct Stats {
        unsigned underruns = 0;  // queue ran dry during playback (one short gap each)
        double ratio = 1.0;      // playback speed (input frames per output frame)
        double queued_ms = 0.0;  // averaged queue level
    };
    Audio() = default;
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    ~Audio() { close(); }
    bool open() {
        applied_core_mask_ = 0;
        SDL_AudioSpec want{}, got{};
        want.freq = 48000; want.format = AUDIO_S16SYS;
        want.channels = 2; want.samples = 512; // 10.7 ms: smaller steps, the cushion is in the queue
        want.callback = callback; want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        if (!device_) return false;
        rate_ = got.freq;
        fm_rate_ = snd::board_fm_rate();
        pcm_rate_ = snd::board_pcm_rate();
        // Half a second each: push() clears both past a quarter (kMaxQueuedSeconds).
        fm_.init(int(fm_rate_ / 2.0));
        pcm_.init(int(pcm_rate_ / 2.0));
        restart_locked(); // the callback is not running yet: the device starts paused
        return true; // device starts paused; prime it before playing
    }
    void push(snd::Board &board) {
        const auto fm = board.take_fm(), pcm = board.take_pcm();
        if (!device_) return; // drain the board even when the device failed
        SDL_LockAudioDevice(device_);
        if (pcm_.available() > int(pcm_rate_ * kMaxQueuedSeconds) ||
            fm_.available() > int(fm_rate_ * kMaxQueuedSeconds)) {
            pcm_.clear(); fm_.clear();
            fm_active_ = false;
        }
        if (std::any_of(fm.begin(), fm.end(), [](float v) { return v != 0.0f; })) {
            if (!fm_active_) {
                // FM starts again after silence: pad it with the silence it skipped, so its
                // first sample plays with the PCM sample queued at the same time.
                const int behind = int(double(pcm_.available()) * fm_rate_ / pcm_rate_) - fm_.available();
                if (behind > 0) fm_.push_silence(behind);
                fm_active_ = true;
            }
            fm_.push(fm.data(), int(fm.size() / 2));
        } else {
            fm_active_ = false; // what is still queued plays out, aligned on the PCM
        }
        if (pcm_.push(pcm.data(), int(pcm.size() / 2)) < int(pcm.size() / 2))
            std::fprintf(stderr, "audio queue: full\n");
        const bool ready = queued_frames_locked() >= AudioRate::kPrimeFrames;
        SDL_UnlockAudioDevice(device_);
        if (!playing_ && ready) { SDL_PauseAudioDevice(device_, 0); playing_ = true; }
    }
    void pause() {
        if (!device_) return;
        SDL_PauseAudioDevice(device_, 1);
        SDL_LockAudioDevice(device_);
        fm_.clear(); pcm_.clear();
        fm_active_ = false;
        restart_locked();
        SDL_UnlockAudioDevice(device_);
        playing_ = false;
    }
    Stats stats() {
        Stats out;
        if (!device_) return out;
        SDL_LockAudioDevice(device_);
        out.underruns = underruns_;
        out.ratio = rate_control_.ratio();
        out.queued_ms = rate_control_.average() * 1000.0 / double(rate_);
        SDL_UnlockAudioDevice(device_);
        return out;
    }
    void volume(float value) {
        if (device_) SDL_LockAudioDevice(device_);
        volume_ = std::clamp(value, 0.0f, 1.0f);
        if (device_) SDL_UnlockAudioDevice(device_);
    }
    // The game's output gain (rt::Game::output_gain), on top of the volume.
    void gain(float value) {
        if (device_) SDL_LockAudioDevice(device_);
        gain_ = std::max(value, 0.0f);
        if (device_) SDL_UnlockAudioDevice(device_);
    }
    void mute(bool muted) {
        if (device_) SDL_LockAudioDevice(device_);
        muted_ = muted;
        if (device_) SDL_UnlockAudioDevice(device_);
    }
    void close() {
        if (device_) SDL_CloseAudioDevice(device_); // joins callback before freeing streams
        device_ = 0;
        playing_ = false;
    }
private:
    static constexpr int kChunk = 512;                         // output frames per step
    static constexpr int kMaxInput = kChunk * 2;               // input frames per step (ratio < 2)
    static constexpr double kMaxQueuedSeconds = 0.25;          // more is cleared (a stall)

    // Under the device lock: silent until kPrimeFrames are queued again.
    void restart_locked() {
        buffering_ = true;
        pcm_resampler_.reset();
        fm_resampler_.reset();
    }
    // The PCM queue sets the pace (the FM one is empty while the FM is silent); in
    // device-rate frames, AudioRate's unit.
    int queued_frames_locked() const { return int(double(pcm_.available()) * rate_ / pcm_rate_); }

    int applied_core_mask_ = 0; // the callback thread's fourth-core policy (core_policy.h)
    static void callback(void *userdata, Uint8 *buffer, int bytes) {
        auto &self = *static_cast<Audio *>(userdata);
        apply_core_policy(self.applied_core_mask_);
        std::memset(buffer, 0, size_t(bytes));
        auto *out = reinterpret_cast<int16_t *>(buffer);
        int frames = bytes / (2 * int(sizeof(int16_t)));
        // SDL invokes the callback under the device lock. The producer
        // uses the same lock, including stream clearing and mute changes.
        int queued = self.queued_frames_locked();
        if (self.buffering_) {
            if (queued < AudioRate::kPrimeFrames) return; // silence while the cushion builds up
            self.buffering_ = false;
            self.rate_control_.reset(queued);
        }
        const double speed = self.rate_control_.update(queued);
        // Board frames per device frame: the rates' quotient times the speed correction.
        const double pcm_ratio = speed * self.pcm_rate_ / self.rate_, fm_ratio = speed * self.fm_rate_ / self.rate_;
        while (frames > 0) {
            const int n = std::min(frames, kChunk);
            const int need = self.pcm_resampler_.needed(n, pcm_ratio);
            if (need > self.pcm_.available()) {
                // Underrun: stop cleanly (the rest stays silent) and re-prime.
                ++self.underruns_;
                self.restart_locked();
                return;
            }
            float *in = self.in_, *mixed = self.mixed_, *fm_out = self.fm_out_;
            const int got = self.pcm_.pop(in, std::min(need, kMaxInput)); // need <= kChunk * 1.2 + 1
            self.pcm_resampler_.run(in, got, mixed, n, pcm_ratio);
            // FM queued only while it was not silent: what there is lines up with this PCM.
            if (self.fm_.available() > 0) {
                const int fm_need = self.fm_resampler_.needed(n, fm_ratio);
                const int fm_got = self.fm_.pop(in, std::min(fm_need, kMaxInput));
                self.fm_resampler_.run(in, fm_got, fm_out, n, fm_ratio);
                for (int i = 0; i < n * 2; ++i) mixed[i] += fm_out[i];
            } else {
                self.fm_resampler_.reset(); // FM that starts again starts from silence
            }
            for (int i = 0; i < n * 2; ++i) {
                float sample = self.muted_ ? 0.f : mixed[i] * self.volume_ * self.gain_;
                if (!std::isfinite(sample)) sample = 0.f;
                out[i] = int16_t(std::clamp(sample, -1.f, 1.f) * 32767.f);
            }
            frames -= n; out += n * 2;
        }
    }
    SDL_AudioDeviceID device_ = 0;
    // The board's samples at its own rates, under the device lock.
    FrameRing fm_, pcm_;
    int rate_ = 48000;
    double fm_rate_ = 55556.0, pcm_rate_ = 44643.0;
    bool fm_active_ = false; // the last FM block was queued (not silent); worker side, under the device lock
    bool playing_ = false, muted_ = false;
    float volume_ = 0.8f, gain_ = 1.0f;
    // Output pacing, touched by the callback and under the device lock only.
    AudioRate rate_control_;
    StereoResampler pcm_resampler_, fm_resampler_;
    bool buffering_ = true;
    unsigned underruns_ = 0;
    float in_[kMaxInput * 2] = {}, mixed_[kChunk * 2] = {}, fm_out_[kChunk * 2] = {}; // callback scratch
};
} // namespace vita
