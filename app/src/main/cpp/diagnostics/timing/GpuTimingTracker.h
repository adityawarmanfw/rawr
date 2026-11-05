#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "imaging/FrameLimits.h"

namespace rawrcam::diagnostics {

struct GpuTimingSnapshot {
    uint64_t submitted = 0;
    uint64_t dropped = 0;
    uint64_t timingSamples = 0;
    double avgRawMs = 0.0;
    double avgTonemapMs = 0.0;
    double avgTotalGpuMs = 0.0;
    double avgVideoProcessMs = 0.0;
    double avgVideoDemosaicMs = 0.0;
    double avgVideoPostMs = 0.0;
    double avgVideoRenderMs = 0.0;
};

class GpuTimingTracker final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    explicit GpuTimingTracker(Diagnostic diagnostic);
    ~GpuTimingTracker();

    GpuTimingTracker(const GpuTimingTracker&) = delete;
    GpuTimingTracker& operator=(const GpuTimingTracker&) = delete;

    void initialize(VkDevice device, float timestampPeriodNs, uint32_t frameSlotCount);
    void destroy();

    void beginFrame(VkCommandBuffer command, uint32_t slotIndex, bool probeRepeat);
    void markRawInputReady(VkCommandBuffer command, uint32_t slotIndex) const;
    void markVideoProcessBegin(VkCommandBuffer command, uint32_t slotIndex, bool active);
    void markVideoProcessDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markVideoDemosaicDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markVideoPostDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markRawDone(VkCommandBuffer command, uint32_t slotIndex) const;
    VkQueryPool rawCfaQueryPool() const noexcept { return stageTimingEnabled_ ? queryPool_ : VK_NULL_HANDLE; }
    uint32_t rawCfaQueryIndex(uint32_t slotIndex) noexcept;
    uint32_t rawHighlightPrepareQueryIndex(uint32_t slotIndex) const noexcept;
    uint32_t rawGuideSeedQueryIndex(uint32_t slotIndex) const noexcept;
    uint32_t rawGuidePropagationQueryIndex(uint32_t slotIndex) const noexcept;
    void markTonemapInputReady(VkCommandBuffer command, uint32_t slotIndex) const;
    void markTonemapDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markTonemapRepeatInputReady(VkCommandBuffer command, uint32_t slotIndex) const;
    void markTonemapRepeatDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markScopesDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markOverlayDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markPresentationDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void markFrameDone(VkCommandBuffer command, uint32_t slotIndex) const;
    void consumeCompleted(uint32_t slotIndex, std::size_t importedCount, std::size_t descriptorCount,
                          bool rawStateEnabled);

    void recordSubmitted() noexcept { ++submitted_; }
    void recordDropped(uint64_t count = 1) noexcept { dropped_ += count; }
    [[nodiscard]] GpuTimingSnapshot snapshot() const noexcept;
    void resetVideoTiming() noexcept {
        videoProcessNs_ = videoDemosaicNs_ = videoPostNs_ = videoRenderNs_ = 0.0;
        videoTimingSamples_ = 0;
    }

   private:
    static double avgMs(double totalNs, uint64_t samples) noexcept;

    Diagnostic diagnostic_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
    float timestampPeriodNs_ = 1.0f;
    uint32_t frameSlotCount_ = 0;
    bool stageTimingEnabled_ = false;
    uint32_t queriesPerSlot_ = 15;
    uint64_t submitted_ = 0;
    uint64_t dropped_ = 0;
    uint64_t timingSamples_ = 0;
    double rawNs_ = 0.0;
    double tonemapNs_ = 0.0;
    double totalNs_ = 0.0;
    double scopesNs_ = 0.0;
    double overlayNs_ = 0.0;
    double presentationNs_ = 0.0;
    double rawPrepNs_ = 0.0;
    double videoProcessNs_ = 0.0;
    double videoDemosaicNs_ = 0.0;
    double videoPostNs_ = 0.0;
    double videoRenderNs_ = 0.0;
    uint64_t videoTimingSamples_ = 0;
    std::array<bool, imaging::kRealtimeFramesInFlight> videoActive_{};
    std::array<bool, imaging::kRealtimeFramesInFlight> rawCfaActive_{};
    uint64_t rawCfaSamples_ = 0;
    double rawCfaNs_ = 0.0;
    double rawHighlightNs_ = 0.0;
    double rawHighlightPrepareNs_ = 0.0;
    double rawHighlightApplyNs_ = 0.0;
    double rawGuideSeedNs_ = 0.0;
    double rawGuidePropagationNs_ = 0.0;
    double rawGuideSmoothNs_ = 0.0;
    double rawShaderNs_ = 0.0;
    double toneHandoffNs_ = 0.0;
    std::array<bool, imaging::kRealtimeFramesInFlight> probeRepeat_{};
};

}  // namespace rawrcam::diagnostics
