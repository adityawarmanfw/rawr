#pragma once
#include <cstdint>
#include <optional>

namespace rawrcam::camera {
// Protected by the controller mutex. A timeout is a failed session, never
// permission to submit a replacement into a possibly running sequence.
class FullSizeSequence {
   public:
    bool begin(uint64_t generation, uint64_t tag) {
        if (busy_ || failed_ || !tag) return false;
        generation_ = generation;
        tag_ = tag;
        sequence_ = -1;
        busy_ = true;
        return true;
    }
    void submitted(int sequence, int64_t nowMs, int64_t exposureNs, uint32_t count = 1) {
        sequence_ = sequence;
        deadlineMs_ = nowMs + count * (8000 + exposureNs / 1'000'000);
    }
    std::optional<uint64_t> complete(uint64_t generation, int sequence) {
        if (!busy_ || generation != generation_ || sequence_ < 0 || sequence != sequence_) return std::nullopt;
        busy_ = false;
        sequence_ = -1;
        return tag_;
    }
    void abandonPreparation() {
        if (sequence_ < 0) busy_ = false;
    }
    bool expire(int64_t nowMs) {
        if (busy_ && sequence_ >= 0 && nowMs >= deadlineMs_) failed_ = true;
        return failed_;
    }
    void fail() { failed_ = true; }
    void reset() { *this = {}; }
    bool busy() const { return busy_; }
    bool failed() const { return failed_; }

   private:
    uint64_t generation_ = 0, tag_ = 0;
    int sequence_ = -1;
    int64_t deadlineMs_ = 0;
    bool busy_ = false, failed_ = false;
};
}  // namespace rawrcam::camera
