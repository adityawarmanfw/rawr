#pragma once

#include <android/native_window.h>
#include <jni.h>
#include <tonemap/TonemapEngine.h>
#include <vulkan/vulkan.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "diagnostics/timing/GpuTimingTracker.h"
#include "imaging/FrameLimits.h"
#include "video/VideoOutput.h"
#include "video/VideoProcessingResources.h"
#include "video/VideoRecorder.h"
#include "video_pipeline/VideoDemosaic.h"
#include "video_pipeline/VideoProcessingConfig.h"
#include "vulkan/ImageResources.h"

static_assert(rawrcam::video::VideoDemosaic::kFramesInFlight == rawrcam::imaging::kRealtimeFramesInFlight,
              "VideoDemosaic slot count must follow the realtime frames-in-flight contract");

namespace rawrcam::vulkan {
class VulkanContext;
}

namespace rawrcam::video {

// Owns RAW development and tonemap into the encoder Surface and a monitor
// image. The monitor never feeds the encoder.
class VideoSession final {
   public:
    VideoSession(vulkan::VulkanContext& context, std::mutex& transitionMutex, std::function<VkExtent2D()> rawExtent,
                 std::function<std::optional<tonemap::lut::LutChain>()> renderLut,
                 std::function<void(const std::string&)> diagnostic);
    void startPrewarm();
    void stopPrewarm();
    void requestVideoPrewarm(uint32_t width, uint32_t height, uint32_t bitDepth);
    void releaseVideoProcessing();
    void markVideoPrewarmDirty();
    void waitForVideoPrewarmIdle();
    void renderLutChanged() noexcept { ++videoLutGeneration_; }
    const std::string& prewarmStatus() const noexcept { return videoPrewarmStatus_; }
    ~VideoSession();
    VideoSession(const VideoSession&) = delete;
    VideoSession& operator=(const VideoSession&) = delete;

    using ProcessingKey = VideoProcessingResources::ProcessingKey;
    using Prepared = VideoProcessingResources::Prepared;
    // The key a recording at this size and bit depth will use, assuming the
    // encoder Surface offers the preferred format.
    ProcessingKey predictedKey(uint32_t width, uint32_t height, uint32_t rawWidth, uint32_t rawHeight,
                               uint32_t bitDepth) const;
    bool hasProcessing(const ProcessingKey& key) const noexcept { return resources_.hasProcessing(key); }
    // Thread-safe: touches no VideoOutput state.
    static Prepared prepare(rawrcam::vulkan::VulkanContext& context, const ProcessingKey& key,
                            const VideoProcessingConfig& config, const tonemap::lut::LutChain* renderLut);
    // Replaces the cached processing; ignored while recording.
    void install(Prepared&& prepared);
    // Frees the cached processing (leaving Video mode). Ignored while recording.
    void releaseProcessing() noexcept;
    const VideoProcessingConfig& desiredProcessingConfig() const noexcept { return desiredConfig_; }

    // The encoder receives dithered RGBA8 (8-bit) or A2B10G10R10 (10-bit)
    // when the Surface offers them as storage images, otherwise RGBA16F.
    bool start(JNIEnv* env, jobject surface, uint32_t width, uint32_t height, uint32_t rawWidth, uint32_t rawHeight,
               uint32_t bitDepth, const tonemap::lut::LutChain* renderLut);
    void updateRenderLut(const tonemap::lut::LutChain* renderLut);
    void setProcessingConfig(const VideoProcessingConfig& config) noexcept;
    const VideoProcessingConfig& activeProcessingConfig() const noexcept { return activeConfig_; }
    bool processingRestartRequired() const noexcept {
        return ready() && (desiredConfig_.fccSteps != activeConfig_.fccSteps ||
                           desiredConfig_.defringeStrength != activeConfig_.defringeStrength ||
                           desiredConfig_.defringeEdgeThreshold != activeConfig_.defringeEdgeThreshold ||
                           desiredConfig_.defringeLumaFloor != activeConfig_.defringeLumaFloor);
    }
    void stop() noexcept;
    bool ready() const noexcept { return output_.ready(); }
    VkExtent2D extent() const noexcept { return output_.extent(); }
    // VkFormat ids the encoder Surface offered at start(), for diagnostics.
    const std::string& offeredFormats() const noexcept { return output_.offeredFormats(); }
    const std::string& startTimingJson() const noexcept { return startTimingJson_; }
    const char* outputFormatName() const noexcept { return output_.outputFormatName(); }
    VkImageView monitorView(uint32_t slot) const noexcept { return resources_.monitorView(slot); }
    VkImage monitorImage(uint32_t slot) const noexcept { return resources_.monitorImage(slot); }
    // Waits briefly for an encoder buffer; still none is a video drop. The
    // wait stays short because it holds the frame submit path.
    bool acquire(uint32_t frameSlot, uint32_t* imageIndex);
    uint32_t encoderImageCount() const noexcept { return output_.encoderImageCount(); }
    void record(VkCommandBuffer command, uint32_t frameSlot, uint32_t imageIndex, uint32_t rawWidth, uint32_t rawHeight,
                const VideoDemosaic::Frame& rawFrame, const std::array<float, 9>& cameraToAp1,
                const tonemap::TonemapParams& params, rawrcam::diagnostics::GpuTimingTracker& timing,
                bool monitorEnabled = true, const std::function<VkCommandBuffer(VkCommandBuffer)>& splitSubmit = {});
    VkSemaphore available(uint32_t frameSlot) const { return output_.available(frameSlot); }
    VkSemaphore rendered(uint32_t frameSlot) const { return output_.rendered(frameSlot); }
    VkResult present(VkQueue queue, uint32_t frameSlot, uint32_t imageIndex, uint64_t presentTimeNs);
    bool stampsPresentTime() const noexcept { return output_.stampsPresentTime(); }
    int64_t startedAtNs() const noexcept { return output_.startedAtNs(); }
    // Average GPU ms per recording stage since start(), as a JSON object.
    std::string stageTimingJson() const;
    // Brackets the viewfinder/scopes submission that reads the monitor image.
    void beginMonitorTiming(VkCommandBuffer command, uint32_t frameSlot);
    void endMonitorTiming(VkCommandBuffer command, uint32_t frameSlot);

   private:
    bool prewarmVideo(uint32_t width, uint32_t height, uint32_t bitDepth);
    void videoPrewarmLoop();
    vulkan::VulkanContext& context_;
    std::mutex& transitionMutex_;
    std::function<VkExtent2D()> rawExtent_;
    std::function<std::optional<tonemap::lut::LutChain>()> renderLut_;
    std::function<void(const std::string&)> diagnostic_;
    VideoProcessingResources resources_;
    VideoOutput output_;
    VideoRecorder recorder_{resources_, output_};
    std::optional<tonemap::TonemapParams> recordingTone_;
    VideoProcessingConfig desiredConfig_{}, activeConfig_{};
    std::string startTimingJson_ = "{}";
    std::mutex prewarmMutex_;
    std::condition_variable prewarmCv_;
    bool prewarmWanted_ = false, prewarmDirty_ = false, prewarmBusy_ = false, prewarmStop_ = false;
    uint32_t prewarmWidth_ = 0, prewarmHeight_ = 0, prewarmBitDepth_ = 10;
    std::thread prewarmThread_;
    uint64_t videoLutGeneration_ = 0;
    std::string videoPrewarmStatus_ = "none";
};
}  // namespace rawrcam::video
