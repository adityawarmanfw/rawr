#include "video/VideoProcessingResources.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

#include "raw_highlight/Coloropp.hpp"
#include "tonemap_video_linear.h"
#include "tonemap_video_rgb10a2.h"
#include "tonemap_video_rgba8.h"
#include "tonemap_video_linear_texture.h"
#include "tonemap_video_rgb10a2_texture.h"
#include "tonemap_video_rgba8_texture.h"
#include "vulkan/VulkanContext.h"
namespace {
int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

namespace rawrcam::video {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
constexpr const char* kStageNames[] = {"demosaic", "preWb", "wb", "hlGuide", "hl", "fcc", "postTail", "tonemap"};
}  // namespace
namespace {
std::unique_ptr<tonemap::TonemapEngine> makeTonemap(rawrcam::vulkan::VulkanContext& context, VkFormat format,
                                                    const tonemap::lut::LutChain* renderLut) {
    tonemap::TonemapCreateInfo tone{};
    tone.context.physicalDevice = context.physicalDevice();
    tone.context.device = context.device();
    // Promote only the device/path that passed repeated GPU timing gates.
    // Custom LUT chains retain buffer storage: texture variants regressed in
    // the four-stage test. Other GPUs still receive the matrix optimization.
    const bool useTexture = !renderLut && context.gpuName() == "Adreno (TM) 840" &&
        tonemap::TonemapEngine::supportsNeutralLutTexture(context.physicalDevice());
    auto selectShader = [&](bool texture) {
        tone.neutralLutTexture = texture;
        if (format == VK_FORMAT_R8G8B8A8_UNORM) {
            tone.shaderSpirv = reinterpret_cast<const uint32_t*>(texture ? tonemap_video_rgba8_texture_spv : tonemap_video_rgba8_spv);
            tone.shaderSpirvBytes = texture ? tonemap_video_rgba8_texture_spv_size : tonemap_video_rgba8_spv_size;
        } else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            tone.shaderSpirv = reinterpret_cast<const uint32_t*>(texture ? tonemap_video_rgb10a2_texture_spv : tonemap_video_rgb10a2_spv);
            tone.shaderSpirvBytes = texture ? tonemap_video_rgb10a2_texture_spv_size : tonemap_video_rgb10a2_spv_size;
        } else {
            tone.shaderSpirv = reinterpret_cast<const uint32_t*>(texture ? tonemap_video_linear_texture_spv : tonemap_video_linear_spv);
            tone.shaderSpirvBytes = texture ? tonemap_video_linear_texture_spv_size : tonemap_video_linear_spv_size;
        }
    };
    selectShader(useTexture);
    tone.lutUploadQueue = context.queue();
    tone.lutUploadQueueFamily = context.queueFamily();
    tone.outputFormat = format;
    tone.videoMonitorOutput = true;
    tone.workgroupSizeX = 8;
    tone.workgroupSizeY = 8;
    tone.maxFramesInFlight = rawrcam::imaging::kRealtimeFramesInFlight;
    tone.lutChain = renderLut;
    if (useTexture) {
        try {
            // Share the queue's submission authority with preview and recording.
            std::lock_guard<std::mutex> lock(context.primaryQueue().mutex());
            auto engine = std::make_unique<tonemap::TonemapEngine>(tone);
            __android_log_print(ANDROID_LOG_INFO, "RawrNative", "VIDEO_TONEMAP_LUT backend=neutral_texture custom=0");
            return engine;
        } catch (const std::exception& error) {
            __android_log_print(ANDROID_LOG_WARN, "RawrNative", "VIDEO_TONEMAP_LUT texture fallback: %s", error.what());
            selectShader(false);
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "RawrNative", "VIDEO_TONEMAP_LUT backend=buffer custom=%d", renderLut ? 1 : 0);
    return std::make_unique<tonemap::TonemapEngine>(tone);
}
}  // namespace

VideoProcessingResources::Prepared::~Prepared() {
    if (!device) return;
    for (auto& image : monitor) rawrcam::vulkan::destroyOwnedImage(device, image);
    rawrcam::vulkan::destroyOwnedImage(device, dummyMonitor);
    for (auto pool : stagePools)
        if (pool) vkDestroyQueryPool(device, pool, nullptr);
    for (auto pool : monitorPools)
        if (pool) vkDestroyQueryPool(device, pool, nullptr);
}
VideoProcessingResources::~VideoProcessingResources() { release(); }
void VideoProcessingResources::beginRecording() {
    if (demosaic_) demosaic_->selectDownscaleFilter();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice(), &properties);
    timestampPeriodNs_ = properties.limits.timestampPeriod;
    monitorInitialized_.fill(false);
    dummyMonitorInitialized_ = false;
    monitorPending_.fill(false);
    monitorSumMs_ = 0.0;
    monitorSamples_ = 0;
    stagePending_.fill(false);
    stageSumMs_.fill(0.0);
    stageSamples_ = 0;
    totalPeak_.reset();
}
VideoProcessingResources::Prepared VideoProcessingResources::prepare(rawrcam::vulkan::VulkanContext& context,
                                                                     const ProcessingKey& key,
                                                                     const VideoProcessingConfig& config,
                                                                     const tonemap::lut::LutChain* renderLut) {
    Prepared out;
    out.key = key;
    out.device = context.device();
    for (auto& image : out.monitor)
        image = rawrcam::vulkan::createOwnedImage(context.physicalDevice(), context.device(), key.width, key.height,
                                                  VK_FORMAT_R8G8B8A8_UNORM,
                                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    out.dummyMonitor = rawrcam::vulkan::createOwnedImage(context.physicalDevice(), context.device(), 1, 1,
                                                         VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT);
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = kStageQueries;
    for (auto& pool : out.stagePools)
        check(vkCreateQueryPool(context.device(), &queryInfo, nullptr, &pool), "video stage query pool");
    queryInfo.queryCount = 2;
    for (auto& pool : out.monitorPools)
        check(vkCreateQueryPool(context.device(), &queryInfo, nullptr, &pool), "video monitor query pool");
    out.demosaic =
        std::make_unique<VideoDemosaic>(context.gpuContext(), key.rawWidth, key.rawHeight, key.width, key.height);
    for (auto& stage : out.post) {
        stage = std::make_unique<rawr::post::PostDemosaicProcessor>(
            context.physicalDevice(), context.device(), context.queueFamily(), key.width, key.height, key.fccSteps,
            false, 0.0f, 0.0f, key.defringeStrength, key.defringeEdgeThreshold, key.defringeLumaFloor);
        // The video tonemap applies the Inpaint Opposed tone tap on load.
        stage->setDeferColoroppTone(true);
        // Build Inpaint Opposed now rather than on the first recorded frame.
        if (config.highlightEnabled && config.highlightMethod == 1u) stage->prepareColoropp();
    }
    out.tonemap = makeTonemap(context, key.outputFormat, renderLut);
    return out;
}
void VideoProcessingResources::install(Prepared&& prepared) {
    if (!prepared.tonemap) return;
    release();
    processingKey_ = prepared.key;
    tonemap_ = std::move(prepared.tonemap);
    demosaic_ = std::move(prepared.demosaic);
    post_ = std::move(prepared.post);
    for (uint32_t i = 0; i < monitor_.size(); ++i) {
        monitor_[i] = prepared.monitor[i];
        prepared.monitor[i] = {};
        stagePools_[i] = prepared.stagePools[i];
        prepared.stagePools[i] = VK_NULL_HANDLE;
        monitorPools_[i] = prepared.monitorPools[i];
        prepared.monitorPools[i] = VK_NULL_HANDLE;
    }
    dummyMonitor_ = prepared.dummyMonitor;
    prepared.dummyMonitor = {};
    monitorInitialized_.fill(false);
    dummyMonitorInitialized_ = false;
    monitorPending_.fill(false);
    stagePending_.fill(false);
}
void VideoProcessingResources::release() noexcept {
    const VkDevice device = context_.device();
    if (device && tonemap_) (void)context_.waitIdle();
    tonemap_.reset();
    for (auto& stage : post_) stage.reset();
    demosaic_.reset();
    if (device) {
        for (auto& monitor : monitor_) rawrcam::vulkan::destroyOwnedImage(device, monitor);
        rawrcam::vulkan::destroyOwnedImage(device, dummyMonitor_);
        for (auto& pool : stagePools_)
            if (pool) vkDestroyQueryPool(device, pool, nullptr);
        for (auto& pool : monitorPools_)
            if (pool) vkDestroyQueryPool(device, pool, nullptr);
    }
    stagePools_.fill(VK_NULL_HANDLE);
    monitorPools_.fill(VK_NULL_HANDLE);
    monitorInitialized_.fill(false);
    dummyMonitorInitialized_ = false;
    processingKey_ = {};
}
void VideoProcessingResources::updateRenderLut(const tonemap::lut::LutChain* renderLut, bool recording) {
    // Also refreshes cached processing between recordings.
    if (!tonemap_) return;
    if (recording) check(context_.waitIdle(), "video LUT wait");
    tonemap_ = makeTonemap(context_, processingKey_.outputFormat, renderLut);
}
void VideoProcessingResources::collectStageTiming(uint32_t frameSlot) {
    if (!stagePending_[frameSlot]) return;
    stagePending_[frameSlot] = false;
    // Queries 6-9 are never written; read the two written ranges separately.
    std::array<uint64_t, kStageQueries> t{};
    const VkDevice device = context_.device();
    if (vkGetQueryPoolResults(device, stagePools_[frameSlot], 0, 6, 6 * sizeof(uint64_t), t.data(), sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
        vkGetQueryPoolResults(device, stagePools_[frameSlot], 10, 6, 6 * sizeof(uint64_t), t.data() + 10,
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    const auto ms = [&](uint32_t from, uint32_t to) {
        return t[to] > t[from] ? double(t[to] - t[from]) * timestampPeriodNs_ / 1.0e6 : 0.0;
    };
    // preWb covers Inpaint Opposed pre-WB and wavelet denoise; postTail covers
    // defringe and the Inpaint Opposed tone tap.
    const std::array<double, kStageCount> stage{ms(12, 13), ms(13, 0), ms(0, 1),  ms(1, 2),
                                                ms(2, 3),   ms(4, 5),  ms(5, 14), ms(14, 15)};
    double total = 0.0;
    for (uint32_t i = 0; i < kStageCount; ++i) {
        stageSumMs_[i] += stage[i];
        total += stage[i];
    }
    ++stageSamples_;
    totalPeak_.add(steadyNs(), total);
}
void VideoProcessingResources::beginMonitorTiming(VkCommandBuffer command, uint32_t frameSlot) {
    if (frameSlot >= monitorPools_.size() || !monitorPools_[frameSlot]) return;
    if (monitorPending_[frameSlot]) {
        uint64_t t[2]{};
        // Skipped rather than waited on when the previous submission is still running.
        if (vkGetQueryPoolResults(context_.device(), monitorPools_[frameSlot], 0, 2, sizeof(t), t, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            t[1] > t[0]) {
            monitorSumMs_ += double(t[1] - t[0]) * timestampPeriodNs_ / 1.0e6;
            ++monitorSamples_;
        }
        monitorPending_[frameSlot] = false;
    }
    vkCmdResetQueryPool(command, monitorPools_[frameSlot], 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, monitorPools_[frameSlot], 0);
}
void VideoProcessingResources::endMonitorTiming(VkCommandBuffer command, uint32_t frameSlot) {
    if (frameSlot >= monitorPools_.size() || !monitorPools_[frameSlot]) return;
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, monitorPools_[frameSlot], 1);
    monitorPending_[frameSlot] = true;
}
std::string VideoProcessingResources::stageTimingJson() const {
    std::string json = "{\"samples\":" + std::to_string(stageSamples_);
    double total = 0.0;
    for (uint32_t i = 0; i < kStageCount; ++i) {
        const double avg = stageSamples_ ? stageSumMs_[i] / double(stageSamples_) : 0.0;
        total += avg;
        json += ",\"" + std::string(kStageNames[i]) + "\":" + std::to_string(avg);
    }
    json += ",\"monitor\":" + std::to_string(monitorSamples_ ? monitorSumMs_ / double(monitorSamples_) : 0.0);
    return json + ",\"total\":" + std::to_string(total) + ",\"totalPeak\":" +
           std::to_string(totalPeak_.peak(steadyNs())) + ",\"totalMax\":" + std::to_string(totalPeak_.max()) + "}";
}
}  // namespace rawrcam::video
