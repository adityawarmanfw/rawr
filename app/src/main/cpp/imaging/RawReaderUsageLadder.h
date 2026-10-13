#pragma once

#include <cstdint>

namespace rawrcam::imaging {

// Camera2 RAW reader usage ladder. Some HALs (observed on MediaTek) accept a
// RAW reader that requests GPU usage bits, configure the session, then never
// deliver a frame and report no error. A GPU-usage reader that stays silent
// past the deadline drops, once per process, to CPU-read-only usage; the GPU
// still reads those buffers through a buffer import. A reader that ever
// delivered a frame proves the GPU usage works, so later stalls never demote.
// Pure state: the caller supplies monotonic time and owns synchronization.
class RawReaderUsageLadder final {
   public:
    static constexpr int64_t kFirstFrameDeadlineMs = 4000;

    void readerCreated(uint64_t generation, bool requestsGpuUsage, int64_t nowMs) {
        generation_ = generation;
        createdAtMs_ = nowMs;
        pending_ = requestsGpuUsage && !cpuOnly_ && !gpuDelivered_;
    }
    void readerDestroyed(uint64_t generation) {
        if (generation == generation_) pending_ = false;
    }
    void rawArrived(uint64_t generation) {
        if (generation != generation_) return;
        if (pending_) gpuDelivered_ = true;
        pending_ = false;
    }
    // True exactly once, when the current reader missed its first frame: the
    // caller restarts the session so the next reader is created cpuOnly().
    bool advance(int64_t nowMs) {
        if (!pending_ || nowMs - createdAtMs_ < kFirstFrameDeadlineMs) return false;
        pending_ = false;
        cpuOnly_ = true;
        return true;
    }
    [[nodiscard]] bool cpuOnly() const noexcept { return cpuOnly_; }

   private:
    uint64_t generation_ = 0;
    int64_t createdAtMs_ = 0;
    bool pending_ = false;
    bool gpuDelivered_ = false;
    bool cpuOnly_ = false;
};

}  // namespace rawrcam::imaging
