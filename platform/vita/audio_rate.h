#pragma once
// Reference audio output pacing (audio.h), without SDL so the host tests run it.
//
// The emulation never runs ahead of real time (FrameClock drops lost time), so the
// sound board produces at most real-time audio. A cushion consumed by a slow frame
// was therefore never rebuilt before: the queue stayed nearly empty and every later
// slow frame cut the output (zero-filled callbacks: crackles). Now:
//   * AudioRate keeps the queue near kTargetFrames by playing it very slightly
//     slower or faster (resampling ratio 1 -/+ a few per mille, smoothed). Slower
//     goes down to -kMaxSlowDown when the emulation stays below full speed (the
//     queue keeps falling); faster never exceeds +kMaxSpeedUp (an excess, e.g.
//     after re-priming, drains over a few seconds instead of raising the pitch).
//   * After an underrun the output is silent until kPrimeFrames are queued again:
//     one short gap instead of a crackle at every callback.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace vita {

class AudioRate {
public:
    // Cushion in the queue: 64 ms at 48 kHz (+ the 512-frame device buffer, 10.7 ms).
    // Absorbs late frames up to ~60 ms without a gap; longer hitches give one short gap.
    static constexpr int kTargetFrames = 3072;
    static constexpr int kPrimeFrames = 3072;   // queued before playing (again, after an underrun)
    // Largest speed changes. 1% is about a sixth of a semitone; the slow-down limit is
    // only reached when the emulation stays below full speed (56 fps = -2.6%, 54.6 fps
    // = -5%): the sound then follows the game's speed instead of crackling.
    static constexpr double kMaxSlowDown = 0.05;
    static constexpr double kMaxSpeedUp = 0.005;
    static constexpr double kGain = 0.05;       // proportional: ratio change per relative level error
    static constexpr double kIntegral = 0.0002;   // integral: learns a lasting speed difference (slow emulation)
    static constexpr double kSmoothing = 0.05;  // level average per update (~20 callbacks, ~0.2 s)
    static constexpr double kSlew = 0.0005;     // ratio change per update (~4.7% per second at most)

    // Playback (re)starts with this queue level. The ratio is kept: after an underrun
    // the emulation is still as slow as before.
    void reset(int level) { average_ = double(level); }
    // Once per audio callback with the queued frames; returns the input frames consumed
    // per output frame (< 1: slower, the queue refills; > 1: faster).
    double update(int level) {
        average_ += (double(level) - average_) * kSmoothing;
        const double error = (average_ - double(kTargetFrames)) / double(kTargetFrames);
        integral_ = std::clamp(integral_ + error * kIntegral, -kMaxSlowDown, kMaxSpeedUp);
        const double want = 1.0 + std::clamp(error * kGain + integral_, -kMaxSlowDown, kMaxSpeedUp);
        ratio_ += std::clamp(want - ratio_, -kSlew, kSlew);
        return ratio_;
    }
    double ratio() const { return ratio_; }
    double average() const { return average_; }

private:
    double average_ = double(kTargetFrames);
    double ratio_ = 1.0;
    double integral_ = 0.0;
};

// Interleaved stereo float frames waiting for the audio callback, at the sound
// board's own rate (the callback's StereoResampler converts them to the device
// rate). Fixed capacity, no allocation after init(); the caller serializes access
// (audio.h: the SDL device lock).
class FrameRing {
public:
    void init(int capacity_frames) {
        data_.assign(size_t(capacity_frames) * 2u, 0.0f);
        capacity_ = capacity_frames;
        head_ = count_ = 0;
    }
    int available() const { return count_; }
    void clear() { head_ = count_ = 0; }
    // Queues what fits; returns the frames queued (fewer than asked when full).
    int push(const float *in, int frames) { return write(in, frames); }
    int push_silence(int frames) { return write(nullptr, frames); }
    // Dequeues up to `frames`; returns the frames written to out.
    int pop(float *out, int frames) {
        frames = std::min(frames, count_);
        for (int done = 0; done < frames;) {
            const int run = std::min(frames - done, capacity_ - head_);
            std::copy_n(&data_[size_t(head_) * 2u], size_t(run) * 2u, out + size_t(done) * 2u);
            head_ = (head_ + run) % capacity_;
            done += run;
        }
        count_ -= frames;
        return frames;
    }

private:
    int write(const float *in, int frames) {
        frames = std::min(frames, capacity_ - count_);
        int tail = (head_ + count_) % capacity_;
        for (int done = 0; done < frames;) {
            const int run = std::min(frames - done, capacity_ - tail);
            float *to = &data_[size_t(tail) * 2u];
            if (in) std::copy_n(in + size_t(done) * 2u, size_t(run) * 2u, to);
            else std::fill_n(to, size_t(run) * 2u, 0.0f);
            tail = (tail + run) % capacity_;
            done += run;
        }
        count_ += frames;
        return frames;
    }
    std::vector<float> data_;
    int capacity_ = 0, head_ = 0, count_ = 0;
};

// Cubic (Catmull-Rom) interpolation of interleaved stereo float frames at a
// variable ratio (input frames per output frame): the board's rate to the device's
// (44.1 or 44.6 kHz PCM, 55.6 kHz FM to 48 kHz) times AudioRate's speed correction.
// Measured on VF2's fight audio, 44.1 to 48 kHz (PC, 6,000 frames): within 0.3 dB of
// SDL2's resampler up to 12 kHz, 1.1-1.5 dB less from 12 to 20 kHz (linear: 2.5-3.1);
// SDL_AudioStreamPut cost 15 us a frame there, this 5-7 us.
// At ratio 1 it is an exact copy delayed by three frames (63 us).
class StereoResampler {
public:
    void reset() {
        for (auto &frame : x_) frame[0] = frame[1] = 0.0f;
        frac_ = 0.0;
    }
    // Input frames that run() will consume to produce out_frames (same arithmetic).
    int needed(int out_frames, double ratio) const {
        double frac = frac_;
        int consumed = 0;
        for (int i = 0; i < out_frames; ++i) {
            frac += ratio;
            while (frac >= 1.0) { frac -= 1.0; ++consumed; }
        }
        return consumed;
    }
    // in: needed(out_frames, ratio) frames. Returns the frames consumed.
    int run(const float *in, int in_frames, float *out, int out_frames, double ratio) {
        int used = 0;
        for (int i = 0; i < out_frames; ++i) {
            const float t = float(frac_);
            for (int c = 0; c < 2; ++c) {
                const float p0 = x_[0][c], p1 = x_[1][c], p2 = x_[2][c], p3 = x_[3][c];
                const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
                const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
                const float d = 0.5f * (p2 - p0);
                out[2 * i + c] = ((a * t + b) * t + d) * t + p1;
            }
            frac_ += ratio;
            while (frac_ >= 1.0) {
                frac_ -= 1.0;
                for (int c = 0; c < 2; ++c) {
                    x_[0][c] = x_[1][c];
                    x_[1][c] = x_[2][c];
                    x_[2][c] = x_[3][c];
                    if (used < in_frames) x_[3][c] = in[2 * used + c]; // else: holds the last frame
                }
                if (used < in_frames) ++used;
            }
        }
        return used;
    }

private:
    float x_[4][2] = {}; // the last four input frames, oldest first
    double frac_ = 0.0;
};

} // namespace vita
