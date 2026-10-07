#pragma once
#include <functional>
#include <optional>

#include "capture/CaptureRequest.h"
#include "capture/multiframe/BracketCollector.h"
#include "capture/multiframe/MultiframeFrameRing.h"
#include "capture/multiframe/mfsr/MfsrCaptureService.h"

namespace rawrcam::capture::multiframe {
// Owns ZSL ring, frozen shutter burst and MFSR service; called on the session frame lane.
class MultiframeCaptureCoordinator final {
   public:
    using PostGain = std::function<float(const metadata::FrameMetadataSnapshot&)>;
    MultiframeCaptureCoordinator(vulkan::VulkanContext& context, std::mutex& queueMutex, std::string filesDir,
                                 PostGain postGain);
    ~MultiframeCaptureCoordinator();
    void configure(uint32_t width, uint32_t height, uint32_t cfa);
    void reset() noexcept;
    void shutdown() noexcept;
    void recordFrame(VkCommandBuffer command, VkImage rawCopy, uint64_t timestampNs,
                     const metadata::FrameMetadataSnapshot& metadata, const color::FrameColorTransform& color);
    void commitFrame(uint64_t timestampNs) noexcept;
    void discardFrame(uint64_t timestampNs) noexcept;
    void retireFrame(uint64_t timestampNs) noexcept;
    void setPersistZslRingEnabled(bool enabled) noexcept;
    void setExperimentalMultiframeEnabled(bool enabled) noexcept;
    bool experimentalMultiframeEnabled() const noexcept { return experimentalMultiframeEnabled_; }
    bool multiframeWorkActive() const noexcept { return mfsrService_ && mfsrService_->isWorkerActive(); }
    uint32_t prepareExperimentalMultiframeOnShutter(uint32_t maxFrames = 30u) noexcept;
    uint64_t startPreparedMultiframeCapture(encoding::dng::DngCaptureContext baseDng,
                                            encoding::dng::DngCaptureContext mergedDng,
                                            rawrcam::capture::JpegCaptureRequest mergedJpeg, bool dumpRzslRequested,
                                            MultiframeTuning tuning, MultiframeBaseFrameMode baseFrameMode,
                                            const tonemap::TonemapParams& tone, bool filmEnabled,
                                            const spektrafilm_native::FilmLook& filmLook,
                                            std::optional<BracketPlan>* bracketPlan = nullptr) noexcept;
    void cancelPreparedMultiframeCapture() noexcept;
    // The camera rejected the bracket requests: stop waiting for them.
    void abandonBracket(std::uint64_t bracketRequestId) noexcept;
    std::string pollMultiframeDngCompletion();
    std::string pollMultiframeJpegCompletion();
    void setFilmAssetManager(AAssetManager* assets) {
        if (mfsrService_) mfsrService_->setFilmAssetManager(assets);
    }
    void setPersistentEngineEnabled(bool enabled) {
        if (mfsrService_) mfsrService_->setPersistentEngineEnabled(enabled);
    }

   private:
    void initializeZslIfNeeded() noexcept;
    void resetZsl() noexcept;
    vulkan::VulkanContext& vulkanContext_;
    PostGain postGainFor_;
    MfsrCaptureService::Geometry config_{};
    bool configured_ = false;
    bool persistZslRingEnabled_ = false;
    bool experimentalMultiframeEnabled_ = false;
    std::unique_ptr<MultiframeFrameRing> multiframeFrameRing_;
    std::unique_ptr<MfsrCaptureService> mfsrService_;
    std::unique_ptr<PendingMultiframeCapture> pendingMultiframeCapture_;
    // HDR+ bracketed: collector awaiting this capture's tagged dark frames
    // (frame lane, under the session lock like the ring).
    std::shared_ptr<BracketCollector> activeBracket_;
    std::uint64_t nextBracketRequestId_ = 1;
    void abandonActiveBracket() noexcept;
};
}  // namespace rawrcam::capture::multiframe
