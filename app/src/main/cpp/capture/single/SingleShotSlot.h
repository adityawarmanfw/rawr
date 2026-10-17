#pragma once

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "color/FrameColorTransform.h"
#include "imaging/RawSnapshot.h"

namespace rawrcam::capture {

// Owns still-capture intent and the independent packed RAW snapshot only.
// It does not own Camera2 requests/results, AImage acquisition, preview frame
// slots, Vulkan processing, DNG serialization, filesystem IO, or UI state.
class SingleShotSlot final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    explicit SingleShotSlot(Diagnostic diagnostic = {});

    // Preallocates the one-shot snapshot destination outside shutter latency.
    // A pending request is intentionally preserved across configuration so an
    // app request made before camera startup captures the first matched frame.
    void configure(uint32_t width, uint32_t height, uint64_t cameraGeneration);
    void disable(const std::string& reason);
    void reset() noexcept;

    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] bool captureRequested() const noexcept { return pendingRequestId_.has_value(); }

    // Returns false if another capture is pending or an unconsumed completed
    // snapshot occupies the bounded slot.
    bool requestCapture(uint64_t requestId);
    // Waits for the tagged one-shot frame; extraWait covers a long exposure
    // (frames that arrive meanwhile do not end the wait).
    void expectOptimizedFrame(uint64_t requestId, std::chrono::milliseconds extraWait = {});
    void cancelPendingCapture(const std::string& reason);

    // Called only for an already timestamp-paired RAW + CaptureResult snapshot.
    // On success, gpuAcquireFenceFd is the CPU-unlock release fence for the
    // existing Vulkan path. acquireFenceFd remains caller-owned.
    bool captureMatchedFrame(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                             const metadata::FrameMetadataSnapshot& metadata,
                             const color::FrameColorTransform& colorState, int* gpuAcquireFenceFd);

    // Claims the pending request and transfers the preallocated packed slot to
    // a deferred worker. No AHardwareBuffer access occurs here.
    std::shared_ptr<rawrcam::imaging::RawSnapshot> claimMatchedFrameForDeferredCopy(
        const metadata::FrameMetadataSnapshot& metadata, const color::FrameColorTransform& colorState);
    void recycleDeferredSlot() noexcept;

    [[nodiscard]] const rawrcam::imaging::RawSnapshot* completedCapture() const noexcept;
    [[nodiscard]] std::shared_ptr<const rawrcam::imaging::RawSnapshot> completedCaptureHandle() const noexcept;
    void releaseCompletedCapture() noexcept;

   private:
    void emit(const std::string& line) const;

    void prepareStagingSlot();

    Diagnostic diagnostic_;
    rawrcam::imaging::RawSnapshot slot_{};
    std::shared_ptr<const rawrcam::imaging::RawSnapshot> completedFrame_;
    uint32_t configuredWidth_ = 0;
    uint32_t configuredHeight_ = 0;
    std::optional<uint64_t> pendingRequestId_;
    std::optional<uint64_t> pendingCameraGeneration_;
    // Consecutive metadata frames seen with a pending request but an empty
    // staging slot (moved out, awaiting recycle). Bounds the wait so the
    // request fails loudly instead of sticking forever.
    std::uint32_t stagingWaitFrames_ = 0;
    std::optional<uint64_t> expectedOptimizedRequestId_;
    std::uint32_t optimizedWaitFrames_ = 0;
    bool optimizedLong_ = false;
    std::chrono::steady_clock::time_point optimizedDeadline_{};
    uint64_t configuredCameraGeneration_ = 0;
    bool disabled_ = false;
};

}  // namespace rawrcam::capture
