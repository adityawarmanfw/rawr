#pragma once
#include <functional>

#include "diagnostics/timing/GpuTimingTracker.h"
#include "video/VideoProcessingResources.h"
namespace rawrcam::video {
class VideoOutput;
class VideoRecorder final {
   public:
    VideoRecorder(VideoProcessingResources& resources, VideoOutput& output) : resources_(resources), output_(output) {}
    void record(VkCommandBuffer command, uint32_t frameSlot, uint32_t imageIndex, uint32_t rawWidth, uint32_t rawHeight,
                const VideoDemosaic::Frame& rawFrame, const std::array<float, 9>& cameraToAp1,
                const tonemap::TonemapParams& params, rawrcam::diagnostics::GpuTimingTracker& timing,
                const VideoProcessingConfig& activeConfig, bool monitorEnabled,
                const std::function<VkCommandBuffer(VkCommandBuffer)>& splitSubmit,
                const std::function<void(VkCommandBuffer, VkImageView, uint32_t, uint32_t)>& sceneMeter);

   private:
    VideoProcessingResources& resources_;
    VideoOutput& output_;
};
}  // namespace rawrcam::video
