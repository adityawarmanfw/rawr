#pragma once

#include <cstdint>

namespace rawrcam::imaging {

// Camera2 RAW reader usage ladder. Some HALs (observed on MediaTek) accept a
// RAW reader that requests GPU usage bits, configure the session, then never
// deliver a frame and report no error. A GPU-usage reader that stays silent
// past the deadline drops, once per process, to CPU-read-only usage; the GPU
// still reads those buffers through a buffer import. A reader that ever
// delivered a frame proves its GPU usage bits work, so later readers asking
// only for proven bits never demote. Proof is per bit: a sampled-image reader
// that delivered says nothing about a later GPU_DATA_BUFFER reader (SM8150:
// gralloc < 4.1 refuses that usage on RAW16 and every request fails).
// Failed captures before the first frame (observed on MT6878: gralloc refuses
// the RAW16 GPU-usage allocation, so every request fails) demote without
// waiting out the deadline, and an allocation probe can demote up front.
// Pure state: the caller supplies monotonic time and owns synchronization.
class RawReaderUsageLadder final {
   public:
    static constexpr int64_t kFirstFrameDeadlineMs = 4000;
    static constexpr uint32_t kFailedCapturesToDemote = 3;

    void readerCreated(uint64_t generation, uint64_t gpuUsage, int64_t nowMs) {
        generation_ = generation;
        createdAtMs_ = nowMs;
        usage_ = gpuUsage;
        pending_ = gpuUsage != 0 && !cpuOnly_ && (provenUsage_ & gpuUsage) != gpuUsage;
        failedCaptures_ = 0;
    }
    void readerDestroyed(uint64_t generation) {
        if (generation == generation_) pending_ = false;
    }
    void rawArrived(uint64_t generation) {
        if (generation != generation_) return;
        if (pending_) provenUsage_ |= usage_;
        pending_ = false;
    }
    void captureFailed(uint64_t generation) {
        if (generation == generation_ && pending_) ++failedCaptures_;
    }
    // The GPU usage is known not to allocate: later readers start CPU-only.
    void demote() { cpuOnly_ = true; pending_ = false; }
    // True exactly once, when the current reader missed its first frame: the
    // caller restarts the session so the next reader is created cpuOnly().
    bool advance(int64_t nowMs) {
        if (!pending_) return false;
        if (failedCaptures_ < kFailedCapturesToDemote && nowMs - createdAtMs_ < kFirstFrameDeadlineMs) return false;
        pending_ = false;
        cpuOnly_ = true;
        return true;
    }
    [[nodiscard]] bool cpuOnly() const noexcept { return cpuOnly_; }

   private:
    uint64_t generation_ = 0;
    int64_t createdAtMs_ = 0;
    uint32_t failedCaptures_ = 0;
    uint64_t usage_ = 0;
    uint64_t provenUsage_ = 0;
    bool pending_ = false;
    bool cpuOnly_ = false;
};

}  // namespace rawrcam::imaging
