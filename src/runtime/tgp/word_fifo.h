// The TGP's FIFOs as a growing ring (M2_FAST_TGP; the SHARC's too with
// M2_FAST_SHARC). Moved out of tgp.h so the SHARC's core can reach them.
#pragma once

#include <cstdint>
#include <vector>

namespace rt {

// The TGP's FIFOs as a growing ring (std::deque's block bookkeeping costs
// on every word: the SH-4 first, the Vita too). The same order and contents.
class WordFifo {
public:
    bool empty() const { return head_ == tail_; }
    uint32_t front() const { return buf_[head_ & mask_]; }
    void pop_front() { ++head_; }
    void push_back(uint32_t v) {
        if (tail_ - head_ == buf_.size()) grow();
        buf_[tail_++ & mask_] = v;
    }

private:
    void grow() {
        std::vector<uint32_t> bigger(buf_.size() * 2);
        for (uint32_t i = head_; i != tail_; ++i) bigger[i - head_] = buf_[i & mask_];
        tail_ -= head_;
        head_ = 0;
        buf_.swap(bigger);
        mask_ = uint32_t(buf_.size() - 1);
    }
    std::vector<uint32_t> buf_ = std::vector<uint32_t>(1024);
    uint32_t mask_ = 1023, head_ = 0, tail_ = 0; // free-running counts, masked
};

} // namespace rt
