#pragma once
#include "capture/multiframe/MultiframeCaptureCoordinator.h"
#include "capture/single/SingleFrameCoordinator.h"
namespace rawrcam::capture {
// Owns admission and the independent single/burst capture lifetimes.
class CaptureCoordinator final {
   public:
    CaptureCoordinator(vulkan::VulkanContext& context, std::mutex& queueMutex, std::string filesDir,
                       SingleFrameCoordinator::Diagnostic diagnostic,
                       multiframe::MultiframeCaptureCoordinator::PostGain postGain)
        : single_(context, queueMutex, filesDir, std::move(diagnostic)),
          multiframe_(context, queueMutex, std::move(filesDir), std::move(postGain)) {}
    SingleFrameCoordinator& single() noexcept { return single_; }
    const SingleFrameCoordinator& single() const noexcept { return single_; }
    multiframe::MultiframeCaptureCoordinator& multiframe() noexcept { return multiframe_; }
    const multiframe::MultiframeCaptureCoordinator& multiframe() const noexcept { return multiframe_; }
    uint64_t requestSingle(encoding::dng::DngCaptureContext dng, JpegCaptureRequest jpeg,
                           const std::function<uint64_t()>& latestIngressTimestamp) {
        const auto id = single_.requestRawStillCapture(std::move(dng), std::move(jpeg));
        if (id) shutterCutoff_ = latestIngressTimestamp();
        return id;
    }
    bool acceptsStillTimestamp(uint64_t timestampNs) const noexcept { return timestampNs > shutterCutoff_; }
    bool detachedWorkActive() const noexcept {
        return single_.detachedHqWorkActive() || multiframe_.multiframeWorkActive();
    }
    // Some accepted capture still needs the camera session (frames due, or not yet copied
    // out of camera buffers). Processing after that runs with the camera closed.
    bool holdsCameraFrames() const noexcept {
        return single_.holdsCameraFrame() || multiframe_.holdsCameraFrames() ||
               multiframe_.bracketHold().opened != multiframe_.bracketHold().done;
    }
    std::string pollDng() {
        auto result = multiframe_.pollMultiframeDngCompletion();
        return result.empty() ? single_.pollDngCompletion() : result;
    }
    std::string pollJpeg() {
        auto result = multiframe_.pollMultiframeJpegCompletion();
        return result.empty() ? single_.pollJpegCompletion() : result;
    }

   private:
    SingleFrameCoordinator single_;
    multiframe::MultiframeCaptureCoordinator multiframe_;
    uint64_t shutterCutoff_ = 0;
};
}  // namespace rawrcam::capture
