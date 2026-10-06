#include "video/VideoSession.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

#include "diagnostics/logging/NativeLog.h"
#include "raw_highlight/Coloropp.hpp"
#include "tonemap_video_linear.h"
#include "tonemap_video_rgb10a2.h"
#include "tonemap_video_rgba8.h"
#include "vulkan/VulkanContext.h"
namespace rawrcam::video {
VideoSession::VideoSession(vulkan::VulkanContext& context, std::mutex& transitionMutex,
                           std::function<VkExtent2D()> rawExtent,
                           std::function<std::optional<tonemap::lut::LutChain>()> renderLut,
                           std::function<void(const std::string&)> diagnostic)
    : context_(context),
      transitionMutex_(transitionMutex),
      rawExtent_(std::move(rawExtent)),
      renderLut_(std::move(renderLut)),
      diagnostic_(std::move(diagnostic)),
      resources_(context),
      output_(context) {}
VideoSession::~VideoSession() {
    stopPrewarm();
    stop();
}
void VideoSession::startPrewarm() {
    prewarmThread_ = std::thread([this] { videoPrewarmLoop(); });
}
void VideoSession::stopPrewarm() {
    {
        std::lock_guard<std::mutex> lock(prewarmMutex_);
        prewarmStop_ = true;
    }
    prewarmCv_.notify_all();
    if (prewarmThread_.joinable()) prewarmThread_.join();
}
VideoSession::Prepared VideoSession::prepare(vulkan::VulkanContext& context, const ProcessingKey& key,
                                             const VideoProcessingConfig& config, const tonemap::lut::LutChain* lut) {
    return VideoProcessingResources::prepare(context, key, config, lut);
}
void VideoSession::install(Prepared&& prepared) {
    if (!ready()) resources_.install(std::move(prepared));
}
void VideoSession::releaseProcessing() noexcept {
    if (!ready()) resources_.release();
}
void VideoSession::updateRenderLut(const tonemap::lut::LutChain* lut) { resources_.updateRenderLut(lut, ready()); }
void VideoSession::stop() noexcept { output_.stop(); }
bool VideoSession::acquire(uint32_t slot, uint32_t* index) { return output_.acquire(slot, index); }
VkResult VideoSession::present(VkQueue queue, uint32_t slot, uint32_t index, uint64_t presentTimeNs) {
    return output_.present(queue, slot, index, presentTimeNs);
}
std::string VideoSession::stageTimingJson() const { return resources_.stageTimingJson(); }
void VideoSession::beginMonitorTiming(VkCommandBuffer command, uint32_t slot) {
    resources_.beginMonitorTiming(command, slot);
}
void VideoSession::endMonitorTiming(VkCommandBuffer command, uint32_t slot) {
    resources_.endMonitorTiming(command, slot);
}
VideoSession::ProcessingKey VideoSession::predictedKey(uint32_t width, uint32_t height, uint32_t rawWidth,
                                                       uint32_t rawHeight, uint32_t bitDepth) const {
    return ProcessingKey{width,
                         height,
                         rawWidth,
                         rawHeight,
                         output_.preferredFormat(bitDepth),
                         desiredConfig_.fccSteps,
                         desiredConfig_.defringeStrength,
                         desiredConfig_.defringeEdgeThreshold,
                         desiredConfig_.defringeLumaFloor};
}
void VideoSession::setProcessingConfig(const VideoProcessingConfig& config) noexcept {
    desiredConfig_ = config;
    if (!ready()) return;
    // FCC capacity and the defringe pipeline are allocated at recording start.
    // Keep those settings latched; per-frame values can change without an idle.
    activeConfig_.lensShadingEnabled = config.lensShadingEnabled;
    activeConfig_.highlightEnabled = config.highlightEnabled;
    activeConfig_.highlightMethod = config.highlightMethod;
    activeConfig_.highlightThreshold = config.highlightThreshold;
    activeConfig_.highlightCompression = config.highlightCompression;
    activeConfig_.waveletDenoiseStrength = config.waveletDenoiseStrength;
    activeConfig_.waveletDenoiseDetail = config.waveletDenoiseDetail;
    activeConfig_.waveletDenoiseForceY = config.waveletDenoiseForceY;
    activeConfig_.waveletDenoiseScales = config.waveletDenoiseScales;
}
bool VideoSession::start(JNIEnv* env, jobject javaSurface, uint32_t width, uint32_t height, uint32_t rawWidth,
                         uint32_t rawHeight, uint32_t bitDepth, const tonemap::lut::LutChain* renderLut) {
    stop();
    recordingTone_.reset();
    if (rawWidth < width || rawHeight < height) return false;
    const auto startedAt = std::chrono::steady_clock::now();
    const auto sinceStartMs = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count();
    };
    try {
        if (!output_.start(env, javaSurface, width, height, bitDepth)) return false;
        const double swapchainMs = sinceStartMs();
        activeConfig_ = desiredConfig_;
        ProcessingKey key{width,
                          height,
                          rawWidth,
                          rawHeight,
                          output_.format(),
                          activeConfig_.fccSteps,
                          activeConfig_.defringeStrength,
                          activeConfig_.defringeEdgeThreshold,
                          activeConfig_.defringeLumaFloor};
        const bool reused = hasProcessing(key);
        if (!reused) {
            resources_.release();
            resources_.install(prepare(context_, key, activeConfig_, renderLut));
        }
        resources_.beginRecording();
        __android_log_print(ANDROID_LOG_INFO, "RawrNativeVideo",
                            "VIDEO_POST_CONFIG highlight=%u fccSteps=%u defringe=%.3f denoise=%.3f "
                            "bitDepth=%u surface=%s reused=%d",
                            activeConfig_.highlightMethod, activeConfig_.fccSteps, activeConfig_.defringeStrength,
                            activeConfig_.waveletDenoiseStrength, bitDepth, outputFormatName(), reused ? 1 : 0);
        const double doneMs = sinceStartMs();
        startTimingJson_ = "{\"swapchain\":" + std::to_string(swapchainMs) +
                           ",\"processing\":" + std::to_string(doneMs - swapchainMs) +
                           ",\"reused\":" + (reused ? "true" : "false") + ",\"key\":\"" + key.describe() + "\"" +
                           ",\"total\":" + std::to_string(doneMs) + "}";
        return true;
    } catch (...) {
        stop();
        throw;
    }
}
void VideoSession::record(VkCommandBuffer command, uint32_t frameSlot, uint32_t imageIndex, uint32_t rawWidth,
                          uint32_t rawHeight, const VideoDemosaic::Frame& rawFrame,
                          const std::array<float, 9>& cameraToAp1, const tonemap::TonemapParams& params,
                          rawrcam::diagnostics::GpuTimingTracker& timing, bool monitorEnabled,
                          const std::function<VkCommandBuffer(VkCommandBuffer)>& splitSubmit) {
    if (!recordingTone_) recordingTone_ = params;
    auto frozen = params;
    frozen.renderTransform = recordingTone_->renderTransform;
    frozen.outputSpace = recordingTone_->outputSpace;
    recorder_.record(command, frameSlot, imageIndex, rawWidth, rawHeight, rawFrame, cameraToAp1, frozen, timing,
                     activeConfig_, monitorEnabled, splitSubmit);
}
bool VideoSession::prewarmVideo(uint32_t width, uint32_t height, uint32_t bitDepth) {
    VideoSession::ProcessingKey key{};
    video::VideoProcessingConfig config{};
    std::optional<tonemap::lut::LutChain> renderLut;
    uint64_t lutGeneration = 0;
    {
        std::lock_guard<std::mutex> lock(transitionMutex_);
        if (!context_.device() || ready() || (bitDepth != 8 && bitDepth != 10)) return false;
        const auto extent = rawExtent_();
        const uint32_t rawWidth = extent.width, rawHeight = extent.height;
        if (width == 0 || height == 0) {
            width = rawWidth & ~1u;
            height = rawHeight & ~1u;
        }
        if (width == 0 || height == 0 || width > rawWidth || height > rawHeight) {
            videoPrewarmStatus_ = "skipped: raw " + std::to_string(rawWidth) + "x" + std::to_string(rawHeight);
            return false;
        }
        key = predictedKey(width, height, rawWidth, rawHeight, bitDepth);
        if (hasProcessing(key)) {
            videoPrewarmStatus_ = "cached " + key.describe();
            return true;
        }
        config = desiredProcessingConfig();
        renderLut = renderLut_();
        lutGeneration = videoLutGeneration_;
    }
    try {
        auto prepared = VideoSession::prepare(context_, key, config, renderLut ? &*renderLut : nullptr);
        std::lock_guard<std::mutex> lock(transitionMutex_);
        if (ready()) return false;
        install(std::move(prepared));
        videoPrewarmStatus_ = "installed " + key.describe();
        if (lutGeneration != videoLutGeneration_) {
            const auto current = renderLut_();
            updateRenderLut(current ? &*current : nullptr);
        }
        return true;
    } catch (const std::exception& error) {
        LOGE("VIDEO_PREWARM_FAIL %s", error.what());
        std::lock_guard<std::mutex> lock(transitionMutex_);
        videoPrewarmStatus_ = std::string("failed: ") + error.what();
        diagnostic_(std::string("VIDEO_PREWARM_FAIL ") + error.what());
        return false;
    }
}
void VideoSession::requestVideoPrewarm(uint32_t width, uint32_t height, uint32_t bitDepth) {
    {
        std::lock_guard<std::mutex> lock(prewarmMutex_);
        prewarmWanted_ = true;
        prewarmDirty_ = true;
        prewarmWidth_ = width;
        prewarmHeight_ = height;
        prewarmBitDepth_ = bitDepth;
    }
    prewarmCv_.notify_all();
}
void VideoSession::markVideoPrewarmDirty() {
    {
        std::lock_guard<std::mutex> lock(prewarmMutex_);
        if (!prewarmWanted_) return;
        prewarmDirty_ = true;
    }
    prewarmCv_.notify_all();
}
void VideoSession::waitForVideoPrewarmIdle() {
    std::unique_lock<std::mutex> lock(prewarmMutex_);
    prewarmCv_.wait_for(lock, std::chrono::seconds(2), [this] { return !prewarmBusy_ || prewarmStop_; });
}
void VideoSession::videoPrewarmLoop() {
    for (;;) {
        uint32_t width = 0, height = 0, bitDepth = 10;
        {
            std::unique_lock<std::mutex> lock(prewarmMutex_);
            prewarmCv_.wait(lock, [this] { return prewarmStop_ || (prewarmWanted_ && prewarmDirty_); });
            if (prewarmStop_) return;
            prewarmDirty_ = false;
            prewarmBusy_ = true;
            width = prewarmWidth_;
            height = prewarmHeight_;
            bitDepth = prewarmBitDepth_;
        }
        (void)prewarmVideo(width, height, bitDepth);
        {
            std::lock_guard<std::mutex> lock(prewarmMutex_);
            prewarmBusy_ = false;
        }
        prewarmCv_.notify_all();
    }
}
void VideoSession::releaseVideoProcessing() {
    {
        std::lock_guard<std::mutex> lock(prewarmMutex_);
        prewarmWanted_ = false;
        prewarmDirty_ = false;
    }
    waitForVideoPrewarmIdle();
    std::lock_guard<std::mutex> lock(transitionMutex_);
    releaseProcessing();
    videoPrewarmStatus_ = "released";
}
}  // namespace rawrcam::video
