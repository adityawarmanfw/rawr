#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "capture/multiframe/FrozenBurst.h"

namespace rawrcam::capture::multiframe {

// Post-shutter dark frames for an HDR+ bracketed capture: what to request
// from the camera. Exposure/sensitivity are the reference ZSL frame's
// CaptureResult values; the camera owner converts them to its request
// coordinate and applies evOffsets (all <= 0) per frame.
struct BracketPlan {
    std::uint64_t requestId = 0;  // tags the one-shot requests (optimizedStillRequestId)
    std::int64_t baseExposureTimeNs = 0;
    std::int32_t baseSensitivity = 0;
    std::vector<float> evOffsets;
};

// Hands tagged dark frames from the frame lane (as single-frame frozen
// bursts, pinned in the ZSL ring) to the spool worker, which waits for them
// with a deadline before the burst is made durable. Thread-safe.
class BracketCollector final {
   public:
    BracketCollector(std::uint64_t requestId, std::uint32_t expected) : requestId_(requestId), expected_(expected) {}
    std::uint64_t requestId() const noexcept { return requestId_; }
    std::uint32_t expected() const noexcept { return expected_; }
    // False once full or closed (the caller drops the frame and its pin).
    bool offer(FrozenBurst&& frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || frames_.size() >= expected_) return false;
            frames_.push_back(std::move(frame));
        }
        changed_.notify_all();
        return true;
    }
    // Camera submit failed or the capture was cancelled: stop waiting.
    void abandon() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
    }
    bool full() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size() >= expected_;
    }
    // Waits until every expected frame arrived, the collector was abandoned,
    // or the deadline passed; closes it and returns what arrived (possibly
    // a partial set).
    std::vector<FrozenBurst> take(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait_until(lock, deadline, [&] { return closed_ || frames_.size() >= expected_; });
        closed_ = true;
        return std::move(frames_);
    }

   private:
    const std::uint64_t requestId_;
    const std::uint32_t expected_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_ = false;
    std::vector<FrozenBurst> frames_;
};

}  // namespace rawrcam::capture::multiframe
