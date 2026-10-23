#pragma once
#include <functional>

#include "diagnostics/timing/GpuTimingTracker.h"
#include "geometry/LensDistortion.h"
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
                const std::function<void(VkCommandBuffer, VkImageView, uint32_t, uint32_t)>& sceneMeter,
                const rawrcam::geometry::LensDistortionModel* lensDistortion = nullptr,
                const color::DcpProfilePtr& dcp = {}, float dcpWeight1 = 1.0f, uint32_t lensMirror = 0);

   private:
    // Undistort for the record crop, cached per calibration and crop.
    rawr::post::StillDistortionCorrection distortionFor(const rawrcam::geometry::LensDistortionModel& lens,
                                                       uint32_t rawWidth, uint32_t rawHeight);
    struct DistortionCache {
        rawrcam::geometry::LensDistortionModel lens{};
        uint32_t rawWidth = 0, rawHeight = 0, cropX = 0, cropY = 0, cropW = 0, cropH = 0;
        rawr::post::StillDistortionCorrection correction{};
        bool valid = false;
    } distortionCache_;
    VideoProcessingResources& resources_;
    VideoOutput& output_;
};
}  // namespace rawrcam::video
