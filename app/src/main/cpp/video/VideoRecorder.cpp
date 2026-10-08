#include "video/VideoRecorder.h"

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
#include "video/VideoOutput.h"
#include "vulkan/VulkanContext.h"
namespace rawrcam::video {
void VideoRecorder::record(VkCommandBuffer command, uint32_t frameSlot, uint32_t imageIndex, uint32_t rawWidth,
                           uint32_t rawHeight, const VideoDemosaic::Frame& rawFrame,
                           const std::array<float, 9>& cameraToAp1, const tonemap::TonemapParams& params,
                           rawrcam::diagnostics::GpuTimingTracker& timing, const VideoProcessingConfig& activeConfig,
                           bool monitorEnabled, const std::function<VkCommandBuffer(VkCommandBuffer)>& splitSubmit) {
    if (!resources_.tonemap_ || imageIndex >= output_.images_.size() || frameSlot >= resources_.monitor_.size() ||
        rawWidth < output_.extent_.width || rawHeight < output_.extent_.height)
        throw std::runtime_error("video output unavailable or RAW geometry invalid");
    std::array<VkImageMemoryBarrier, 4> begins{};
    uint32_t beginCount = 0;
    auto addBegin = [&](VkImage image, VkImageLayout oldLayout, VkAccessFlags access) {
        auto& b = begins[beginCount++];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = oldLayout;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = access;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    };
    addBegin(output_.images_[imageIndex],
             output_.initialized_[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED, 0);
    if (monitorEnabled)
        addBegin(resources_.monitor_[frameSlot].image,
                 resources_.monitorInitialized_[frameSlot] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                           : VK_IMAGE_LAYOUT_UNDEFINED,
                 resources_.monitorInitialized_[frameSlot] ? VK_ACCESS_SHADER_READ_BIT : 0);
    else if (!resources_.dummyMonitorInitialized_)
        addBegin(resources_.dummyMonitor_.image, VK_IMAGE_LAYOUT_UNDEFINED, 0);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, beginCount, begins.data());
    if (!monitorEnabled) resources_.dummyMonitorInitialized_ = true;
    // The slot's previous submission has retired before the slot is reused.
    resources_.collectStageTiming(frameSlot);
    const VkQueryPool stagePool = resources_.stagePools_[frameSlot];
    vkCmdResetQueryPool(command, stagePool, 0, resources_.kStageQueries);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, stagePool, 12);
    if (rawFrame.rawBuffer && rawFrame.rawStridePixels < rawWidth)
        throw std::runtime_error("video RAW buffer stride is invalid");
    if (rawFrame.rawBuffer) {
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.buffer = rawFrame.rawBuffer;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &barrier, 0, nullptr);
    }
    auto developedFrame = rawFrame;
    if (!activeConfig.lensShadingEnabled) {
        developedFrame.lensShading = nullptr;
        developedFrame.lensShadingCount = 0;
    }
    resources_.demosaic_->record(command, frameSlot, developedFrame);
    timing.markVideoDemosaicDone(command, frameSlot);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, stagePool, 13);
    // Submitting the frame in parts lets the GPU run the encoder's own
    // input conversion between them instead of queueing it behind one long
    // command buffer.
    if (splitSubmit) command = splitSubmit(command);
    const std::array<float, 3> gains{rawFrame.wb[0], 0.5f * (rawFrame.wb[1] + rawFrame.wb[2]), rawFrame.wb[3]};
    auto& stage = *resources_.post_[frameSlot];
    rawr::post::DenoiseRequest denoise{};
    if (rawFrame.noiseProfileValid) {
        denoise.strength = activeConfig.waveletDenoiseStrength;
        denoise.detail = activeConfig.waveletDenoiseDetail;
        denoise.forceY = activeConfig.waveletDenoiseForceY;
        denoise.maxScale = activeConfig.waveletDenoiseScales;
        denoise.noiseA = rawFrame.noiseA;
        denoise.noiseB = rawFrame.noiseB;
    }
    rawr::shading::LensShadingMapView shading{};
    if (developedFrame.lensShading) {
        shading = {developedFrame.lensShadingWidth, developedFrame.lensShadingHeight, developedFrame.lensShading,
                   developedFrame.lensShadingCount};
    }
    const rawr::highlight::ColoroppSensorGeometry geometry{rawWidth, rawHeight, resources_.demosaic_->cropX(),
                                                           resources_.demosaic_->cropY(),
                                                           resources_.demosaic_->sensorScale()};
    stage.record(command, resources_.demosaic_->outputImage(frameSlot), resources_.demosaic_->outputView(frameSlot),
                 resources_.demosaic_->clipStateImage(frameSlot), resources_.demosaic_->clipStateView(frameSlot), gains,
                 activeConfig.highlightEnabled, {}, stagePool, false, activeConfig.highlightMethod,
                 activeConfig.highlightThreshold, activeConfig.highlightCompression,
                 std::max(params.aePostGain * std::exp2(params.exposureEV), 1.0e-6f), shading, rawFrame.cfa, denoise,
                 geometry, rawFrame.cameraToLinearSrgb.data());
    timing.markVideoPostDone(command, frameSlot);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, stagePool, 14);
    if (splitSubmit) command = splitSubmit(command);
    VkImageMemoryBarrier postReady{};
    postReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    postReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    postReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    postReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    postReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    postReady.image = stage.sdrOutputImage();
    postReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &postReady);
    tonemap::TonemapRecordInfo tone{};
    tone.commandBuffer = command;
    tone.input = {stage.sdrOutputView(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, output_.extent_.width,
                  output_.extent_.height};
    tone.output = {output_.views_[imageIndex], output_.outputFormat_, VK_IMAGE_LAYOUT_GENERAL, output_.extent_.width,
                   output_.extent_.height};
    tone.monitor = {monitorEnabled ? resources_.monitor_[frameSlot].view : resources_.dummyMonitor_.view,
                    VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, output_.extent_.width, output_.extent_.height};
    tone.monitorEnabled = monitorEnabled;
    if (stage.coloroppToneRequired() && params.renderTransform == tonemap::RenderTransform::Existing) {
        tone.highlightCompression = rawr::highlight::coloroppToneCompression(activeConfig.highlightCompression);
        tone.highlightExposureGain = rawr::highlight::coloroppToneExposureGain(
            std::max(params.aePostGain * std::exp2(params.exposureEV), 1.0e-6f));
    }
    tone.frameSlot = frameSlot;
    tone.cameraToWorkingColumnMajor3x3 = cameraToAp1.data();
    tone.params = params;
    resources_.tonemap_->record(tone);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, stagePool, 15);
    resources_.stagePending_[frameSlot] = true;
    VkImageMemoryBarrier end{};
    end.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    end.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    end.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    end.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    end.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    end.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    end.image = output_.images_[imageIndex];
    end.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &end);
    if (monitorEnabled) {
        VkImageMemoryBarrier monitorReady{};
        monitorReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        monitorReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        monitorReady.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        monitorReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        monitorReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        monitorReady.image = resources_.monitor_[frameSlot].image;
        monitorReady.subresourceRange = end.subresourceRange;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &monitorReady);
        resources_.monitorInitialized_[frameSlot] = true;
    }
    output_.initialized_[imageIndex] = true;
}
}  // namespace rawrcam::video
