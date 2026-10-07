#pragma once
#include "pipeline/FrameCapturePort.h"
#include "pipeline/FrameDiagnosticsPort.h"
#include "pipeline/FrameLifecyclePort.h"
namespace rawrcam::session {
class SessionEngine;
class SessionFrameCallbacks final : public pipeline::FrameLifecyclePort,
                                    public pipeline::FrameCapturePort,
                                    public pipeline::FrameDiagnosticsPort {
   public:
    explicit SessionFrameCallbacks(SessionEngine* engine);
    void postDiagnostic(const std::string& line) override;
    void postAudit(const std::string& line) override;
    void notifySwapchainOutOfDate() override;
    void finalizeDetachedStill() override;
    float postGainFor(const rawrcam::metadata::FrameMetadataSnapshot& metadata) override;
    void advanceStill() override;
    bool isStillCaptureRequested() override;
    bool deferSubmittedFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                             const rawrcam::color::FrameColorTransform& colorState,
                             const tonemap::TonemapParams& tonemapParams, float aePostGain, bool filmEnabled,
                             const spektrafilm_native::FilmLook& filmLook) override;
    bool beginDeferredSnapshot(AImage* imageLease, AHardwareBuffer* ahb, std::uint64_t timestampNs,
                               int acquireFenceFd = -1) override;
    void discardScopesSlot(std::uint32_t slotIndex) override;
    void retireScopesSlot(std::uint32_t slotIndex) override;
    std::optional<RenderedFeedback> consumeExposureFeedback(std::uint32_t slotIndex) override;
    bool exposureMeterWanted() override;
    void exposureMeter(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                       const RenderedFeedback& rendered) override;
    bool overlayNeedsRawState() override;
    void recordAuditFrame(const rawrcam::metadata::FrameMetadataSnapshot& metadata,
                          const rawrcam::color::FrameColorTransform& colorState) override;
    void integrityComplete(std::uint32_t slotIndex) override;
    bool cpuCopyNeedsSample() override;
    CpuCopyOutcome sampleCpuCopy(AImage* image, AHardwareBuffer* ahb, int acquireFenceFd,
                                 std::uint64_t timestampNs) override;

    bool detachedCaptureActive() const noexcept override;
    void resetStillProcessing() noexcept override;
    void configureStillSnapshot(uint32_t width, uint32_t height, uint64_t generation) override;
    void configureMultiframe(uint32_t width, uint32_t height, uint32_t cfa) override;
    void resetMultiframe() noexcept override;
    void shutdownMultiframe() noexcept override;
    bool multiframeEnabled() const noexcept override;
    void recordMultiframeFrame(VkCommandBuffer command, VkImage rawCopy, uint64_t timestampNs,
                               const metadata::FrameMetadataSnapshot& metadata,
                               const color::FrameColorTransform& color) override;
    void commitMultiframeFrame(uint64_t timestampNs) noexcept override;
    void discardMultiframeFrame(uint64_t timestampNs) noexcept override;
    void retireMultiframeFrame(uint64_t timestampNs) noexcept override;

   private:
    SessionEngine* engine_;
};

}  // namespace rawrcam::session
