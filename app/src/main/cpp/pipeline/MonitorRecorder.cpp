#include "pipeline/MonitorRecorder.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

#include "color/ColorMath.h"
#include "color/FilmExposure.h"
#include "diagnostics/probes/PipelineDiagnostics.h"
#include "diagnostics/probes/RawIntegrityProbe.h"
#include "diagnostics/probes/RawIntegrityProbePolicy.h"
#include "diagnostics/timing/GpuTimingTracker.h"
#include "geometry/OrientationTransform.h"
#include "monitoring/ImageScopesProcessor.h"
#include "monitoring/MonitoringOverlayProcessor.h"
#include "presentation/SwapchainRenderer.h"
#include "raw_preview/RawPreview.hpp"
#include "tonemap/TonemapEngine.h"
#include "video_pipeline/VideoCrop.h"
#include "vulkan/RawAhbImporter.h"
#include "vulkan/Synchronization.h"

namespace rawrcam::pipeline {
void MonitorRecorder::recordVideoScopes(VkCommandBuffer command, uint32_t frameSlot, VkImageView monitorView,
                                        VkImage monitorImage, uint32_t width, uint32_t height,
                                        int sensorOrientationDegrees, int deviceRotationDegrees) const {
    const auto scopeState = imageScopes_.presentationState();
    bool any = false;
    for (const auto& placement : scopeState.placements) any |= placement.type != rawrcam::monitoring::ScopeType::None;
    if (!any) return;
    VkImageMemoryBarrier monitorReady{};
    monitorReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    monitorReady.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    monitorReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    monitorReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    monitorReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    monitorReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    monitorReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    monitorReady.image = monitorImage;
    monitorReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &monitorReady);
    const uint32_t sourceTurns =
        rawrcam::geometry::presentationQuarterTurns(sensorOrientationDegrees, deviceRotationDegrees);
    imageScopes_.record(command, frameSlot, monitorView, monitorImage, sourceTurns, false, width, height,
                        image_scopes::SamplingMode::Production25);
    monitorReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    monitorReady.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    monitorReady.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    monitorReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &monitorReady);
    for (uint32_t i = 0; i < scopeState.placements.size(); ++i) {
        if (scopeState.placements[i].type != rawrcam::monitoring::ScopeType::None)
            rawrcam::vulkan::computeWriteToFragmentRead(command, imageScopes_.renderedImage(frameSlot, i));
    }
}
void MonitorRecorder::restoreVideoScopes(VkCommandBuffer command, uint32_t frameSlot) const {
    const auto scopeState = imageScopes_.presentationState();
    for (uint32_t i = 0; i < scopeState.placements.size(); ++i) {
        if (scopeState.placements[i].type != rawrcam::monitoring::ScopeType::None)
            rawrcam::vulkan::fragmentReadToComputeWrite(command, imageScopes_.renderedImage(frameSlot, i));
    }
}
void MonitorRecorder::transitionImageForSampling(VkCommandBuffer command, VkImage image) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}
void MonitorRecorder::transitionImageForCompute(VkCommandBuffer command, VkImage image) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}
VkImageView MonitorRecorder::rawStateView(uint32_t slot) const { return monitoringOverlay_.rawStateView(slot); }
void MonitorRecorder::recordSceneExposure(VkCommandBuffer command, uint32_t slotIndex, VkImageView linearView,
                                          uint32_t width, uint32_t height, const std::array<float, 3>& lumaWeights,
                                          float clipLuma, const std::array<float, 4>& spot,
                                          uint32_t weighting, const std::array<float, 3>& channelCeilings) const {
    imageScopes_.recordSceneExposure(command, slotIndex, linearView, width, height, lumaWeights, clipLuma, spot,
                                     weighting, channelCeilings);
}
void MonitorRecorder::recordOverlays(const MonitorFrame& in) const {
    if (in.diagnosticMode == 0u) {
        const uint32_t sourceQuarterTurns =
            rawrcam::geometry::presentationQuarterTurns(in.sensorOrientationDegrees, in.scopeDeviceRotationDegrees);
        imageScopes_.record(in.command, in.slotIndex, in.tonemappedView, in.tonemappedImage, sourceQuarterTurns,
                            in.renderedExposureFeedbackEnabled);
        performance_.markScopesDone(in.command, in.slotIndex);
        monitoringOverlay_.record(in.command, in.slotIndex, in.linearView, in.linearImage, in.tonemappedView,
                                  in.tonemappedImage, in.shadowLiftEv, in.blackPointEv);
        performance_.markOverlayDone(in.command, in.slotIndex);
    } else {
        performance_.markScopesDone(in.command, in.slotIndex);
        performance_.markOverlayDone(in.command, in.slotIndex);
    }
}
void MonitorRecorder::present(const MonitorFrame& in, uint32_t cropX, uint32_t cropY, uint32_t cropW,
                              uint32_t cropH) const {
    if (in.diagnosticMode == 4u) {
        transitionImageForSampling(in.command, in.linearImage);
        presentation_.record(in.command, in.slotIndex, in.swapImageIndex, in.width, in.height,
                             in.sensorOrientationDegrees, in.displayRotationDegrees, in.diagnosticMode, false);
        transitionImageForCompute(in.command, in.linearImage);
    } else {
        rawrcam::vulkan::computeWriteToFragmentRead(in.command, in.tonemappedImage);
        if (in.diagnosticMode == 0u && monitoringOverlay_.enabled()) {
            rawrcam::vulkan::computeWriteToFragmentRead(in.command, monitoringOverlay_.overlayImage(in.slotIndex));
        }
        const auto scopeState = imageScopes_.presentationState();
        if (in.diagnosticMode == 0u) {
            for (uint32_t i = 0; i < scopeState.placements.size(); ++i) {
                if (scopeState.placements[i].type != rawrcam::monitoring::ScopeType::None) {
                    rawrcam::vulkan::computeWriteToFragmentRead(in.command,
                                                                imageScopes_.renderedImage(in.slotIndex, i));
                }
            }
        }
        presentation_.record(in.command, in.slotIndex, in.swapImageIndex, in.width, in.height,
                             in.sensorOrientationDegrees, in.displayRotationDegrees, in.diagnosticMode,
                             in.overlayPresentationEnabled && monitoringOverlay_.enabled(), 0u, false, cropX, cropY,
                             cropW, cropH);
        rawrcam::vulkan::fragmentReadToComputeWrite(in.command, in.tonemappedImage);
        if (in.diagnosticMode == 0u && monitoringOverlay_.enabled()) {
            rawrcam::vulkan::fragmentReadToComputeWrite(in.command, monitoringOverlay_.overlayImage(in.slotIndex));
        }
        if (in.diagnosticMode == 0u) {
            for (uint32_t i = 0; i < scopeState.placements.size(); ++i) {
                if (scopeState.placements[i].type != rawrcam::monitoring::ScopeType::None) {
                    rawrcam::vulkan::fragmentReadToComputeWrite(in.command,
                                                                imageScopes_.renderedImage(in.slotIndex, i));
                }
            }
        }
    }
}
}  // namespace rawrcam::pipeline
