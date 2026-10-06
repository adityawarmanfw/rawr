#include "pipeline/RawDevelopRecorder.h"

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
#include "geometry/OrientationTransform.h"
#include "monitoring/ImageScopesProcessor.h"
#include "monitoring/MonitoringOverlayProcessor.h"
#include "pipeline/RawCpuUploadPool.h"
#include "presentation/SwapchainRenderer.h"
#include "raw_preview/RawPreview.hpp"
#include "tonemap/TonemapEngine.h"
#include "video_pipeline/VideoCrop.h"
#include "vulkan/RawAhbImporter.h"
#include "vulkan/Synchronization.h"

namespace rawrcam::pipeline {

RawDevelopRecorder::RawDevelopRecorder(raw_preview::RawPreview& rawPreview, tonemap::TonemapEngine& tonemap,
                                       std::function<std::shared_ptr<spektrafilm_native::SpektraFilm>()> acquireFilm,
                                       rawrcam::diagnostics::PipelineDiagnostics& diagnostics,
                                       rawrcam::diagnostics::RawIntegrityProbe& integrityProbe,
                                       rawrcam::monitoring::MonitoringOverlayProcessor& monitoringOverlay,
                                       rawrcam::monitoring::ImageScopesProcessor& imageScopes,
                                       rawrcam::presentation::SwapchainRenderer& presentation,
                                       rawrcam::diagnostics::GpuTimingTracker& performance, Audit audit)
    : rawPreview_(rawPreview),
      tonemap_(tonemap),
      acquireFilm_(std::move(acquireFilm)),
      diagnostics_(diagnostics),
      integrityProbe_(integrityProbe),
      monitor_(monitoringOverlay, imageScopes, presentation, performance),
      performance_(performance),
      audit_(std::move(audit)) {}

RawDevelopRecordResult RawDevelopRecorder::record(const RawDevelopRecordInput& in, uint32_t auditSubmitCount) const {
    auto& slot = in.slot;
    const MonitorFrame monitorFrame{slot.command,
                                    in.slotIndex,
                                    in.swapImageIndex,
                                    in.previewWidth,
                                    in.previewHeight,
                                    slot.linear.image,
                                    slot.tonemapped.image,
                                    slot.linear.view,
                                    slot.tonemapped.view,
                                    in.diagnosticMode,
                                    in.sensorOrientationDegrees,
                                    in.displayRotationDegrees,
                                    in.scopeDeviceRotationDegrees,
                                    in.renderedExposureFeedbackEnabled,
                                    in.tonemapParams.shadowLiftEV,
                                    in.tonemapParams.blackPointEV};

    const bool rawStorageViz = in.diagnosticMode == 7u;
    const bool rawSampledViz = in.diagnosticMode == 8u;
    const bool zeroCopySweep = in.diagnosticMode >= 11u && in.diagnosticMode <= 15u;
    const bool separateTransferReference = zeroCopySweep && in.raw.referenceTransferAvailable;
    // Mode 16 runs the production bridge path plus a byte-addressed
    // buffer-view parity probe of the same camera AHB (no sampler involved).
    const bool bufferImportParity = in.diagnosticMode == 16u;
    // The multiframe ring consumes the owned bridge copy, so the bridge must
    // be filled even when experimentalZeroCopy would otherwise skip the
    // transfer path entirely. Preview keeps reading the import buffer
    // (useBufferDirect) while the bridge feeds the ring.
    const bool multiframeBridgeFill = in.diagnosticMode == 0u && in.multiframeEnabled && in.experimentalZeroCopy;
    const bool rawTransferCopy = (in.diagnosticMode == 0u && !in.experimentalZeroCopy) || in.diagnosticMode == 9u ||
                                 zeroCopySweep || bufferImportParity || multiframeBridgeFill;
    const VkPipelineStageFlags rawReadStage =
        separateTransferReference
            ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            : (rawTransferCopy ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    const VkAccessFlags rawReadAccess =
        separateTransferReference ? VK_ACCESS_SHADER_READ_BIT
                                  : (rawTransferCopy ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT);

    acquireRawInputImage(slot.command, in.raw, in.queueFamily, rawReadStage, rawReadAccess);
    if (separateTransferReference) {
        rawrcam::vulkan::acquireForeignImage(slot.command, in.raw.referenceTransferImage, in.queueFamily,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    }
    if (zeroCopySweep && in.raw.linearCandidateAvailable) {
        rawrcam::vulkan::acquireForeignImage(slot.command, in.raw.linearImage, in.queueFamily,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    if (in.diagnosticMode == 15u && in.raw.externalFormatCandidateAvailable) {
        rawrcam::vulkan::acquireForeignImage(slot.command, in.raw.externalFormatImage, in.queueFamily,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    if (in.diagnosticMode == 15u && in.raw.drmCandidateAvailable) {
        rawrcam::vulkan::acquireForeignImage(slot.command, in.raw.drmImage, in.queueFamily,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    const bool repeatProbe = (auditSubmitCount < 6u) && in.diagnosticMode == 0u;
    performance_.beginFrame(slot.command, in.slotIndex, repeatProbe);

    if (rawStorageViz || rawSampledViz) {
        performance_.markRawInputReady(slot.command, in.slotIndex);
        performance_.markVideoProcessBegin(slot.command, in.slotIndex, false);
        performance_.markVideoProcessDone(slot.command, in.slotIndex);
        diagnostics_.recordRawVisualization(slot.command, in.slotIndex, in.raw.view, rawSampledViz, in.black, in.white);
        performance_.markRawDone(slot.command, in.slotIndex);
        performance_.markTonemapInputReady(slot.command, in.slotIndex);
        performance_.markTonemapDone(slot.command, in.slotIndex);
        performance_.markTonemapRepeatInputReady(slot.command, in.slotIndex);
        performance_.markTonemapRepeatDone(slot.command, in.slotIndex);
        releaseRawInputImage(slot.command, in.raw, in.queueFamily, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
        auto diagnosticMonitor = monitorFrame;
        diagnosticMonitor.overlayPresentationEnabled = false;
        monitor_.present(diagnosticMonitor);
        performance_.markScopesDone(slot.command, in.slotIndex);
        performance_.markOverlayDone(slot.command, in.slotIndex);
        performance_.markPresentationDone(slot.command, in.slotIndex);
        performance_.markFrameDone(slot.command, in.slotIndex);
        return {rawReadStage};
    }

    VkImageView rawPreviewInputView = in.raw.view;
    const VkImage transferOracleImage = separateTransferReference ? in.raw.referenceTransferImage : in.raw.image;
    // Buffer-direct ingress: demosaic reads the imported AHB words
    // (exact on every stride); the bridge copy is then redundant. Active in
    // production and in the mode-16 parity harness whenever the buffer import
    // succeeded; otherwise the legacy bridge path runs unchanged.
    const bool useBufferDirect = (in.diagnosticMode == 0u || in.diagnosticMode == 16u) && in.raw.importBufferAvailable;
    // The multiframe ring consumes the owned copy, so the bridge must run
    // whenever multiframe is enabled even though compute reads the buffer.
    const bool skipBridgeCopy = in.diagnosticMode == 0u && in.raw.importBufferAvailable && !in.multiframeEnabled;
    // Bridge was released at configure time (e.g. multiframe enabled
    // mid-session without reconfigure) but preview can still use the import
    // buffer: keep preview alive and let the ring starve into a single-frame
    // fallback (coordinator logs MULTIFRAME_RING_STARVED) instead of dropping
    // every frame until the next reconfigure.
    const bool bridgeMissingPreviewOnly =
        in.diagnosticMode == 0u && useBufferDirect && !skipBridgeCopy && slot.rawCopy.image == VK_NULL_HANDLE;
    if (in.diagnosticMode == 6u) {
        if (slot.rawCopy.image == VK_NULL_HANDLE)
            throw std::runtime_error(
                "raw_copy visualization requested without owned image (relaunch in this diagnostic mode)");
        diagnostics_.recordRawCopy(slot.command, in.slotIndex, in.raw.view);
        rawrcam::vulkan::computeWriteToComputeRead(slot.command, slot.rawCopy.image);
        releaseRawInputImage(slot.command, in.raw, in.queueFamily, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
        rawPreviewInputView = slot.rawCopy.view;
    } else if (rawTransferCopy) {
        if (auditSubmitCount < 7 && audit_) {
            audit_("PIPELINE_RAW_TRANSFER_RECORDED generation=" + std::to_string(in.generation) +
                   " timestampNs=" + std::to_string(in.timestampNs) + " raw=" + std::to_string(in.rawWidth) + "x" +
                   std::to_string(in.rawHeight));
        }
        if (in.diagnosticMode == 15u && in.raw.copyBufferAvailable)
            integrityProbe_.beginProductionRawBenchmark(slot.command, in.slotIndex);
        if (skipBridgeCopy || bridgeMissingPreviewOnly) {
            if (auditSubmitCount < 3u && audit_) {
                if (bridgeMissingPreviewOnly) {
                    audit_("MULTIFRAME_BRIDGE_MISSING generation=" + std::to_string(in.generation) +
                           " timestampNs=" + std::to_string(in.timestampNs) + " fallback=preview_only_single_frame");
                } else {
                    audit_("RAW_INGRESS_BUFFER_DIRECT generation=" + std::to_string(in.generation) +
                           " stridePixels=" + std::to_string(in.raw.importBufferStridePixels) + " bridgeSkipped=true");
                }
            }
        } else {
            if (slot.rawCopy.image == VK_NULL_HANDLE)
                throw std::runtime_error(
                    "bridge copy requested without owned image (bridge image "
                    "released; relaunch/reconfigure the session with multiframe enabled "
                    "so FRAME_SLOTS_BRIDGE_COPY is retained)");
            if (multiframeBridgeFill && auditSubmitCount < 3u && audit_) {
                audit_("MULTIFRAME_BRIDGE_FILL generation=" + std::to_string(in.generation) +
                       " timestampNs=" + std::to_string(in.timestampNs) + " zeroCopyBackground=true");
            }
            rawrcam::vulkan::recordImageCopy(slot.command, transferOracleImage, slot.rawCopy.image, in.rawWidth,
                                             in.rawHeight);
            rawrcam::vulkan::transferWriteToComputeRead(slot.command, slot.rawCopy.image);
        }
        if (zeroCopySweep) {
            if (!separateTransferReference) {
                VkImageMemoryBarrier directRead{};
                directRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                directRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                directRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                directRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                directRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                directRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                directRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                directRead.image = in.raw.image;
                directRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &directRead);
            }
        } else {
            releaseRawInputImage(slot.command, in.raw, in.queueFamily, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT);
        }
        // Without the bridge the owned copy is stale; point image consumers
        // at the fresh import (compute stages read the buffer instead).
        rawPreviewInputView = (skipBridgeCopy || bridgeMissingPreviewOnly) ? in.raw.view : slot.rawCopy.view;
    }
    if (useBufferDirect) {
        // Imported camera memory was written externally; make it visible to
        // the compute stages that read it below. Covers demosaic
        // and (in mode 16) the parity probe's own dispatch as well.
        VkBufferMemoryBarrier importBarrier{};
        importBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        importBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        importBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        importBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        importBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        importBarrier.buffer = in.raw.importBuffer;
        importBarrier.offset = 0;
        importBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &importBarrier, 0, nullptr);
    }
    if (bufferImportParity && in.raw.importBufferAvailable) {
        integrityProbe_.recordBufferImportParity(slot.command, slot.rawCopy.view, in.raw.importBuffer,
                                                 in.raw.importBufferStridePixels, in.slotIndex, in.timestampNs);
    }

    performance_.markRawInputReady(slot.command, in.slotIndex);

    performance_.markVideoProcessBegin(slot.command, in.slotIndex, static_cast<bool>(in.recordPrimaryVideo));
    if (in.recordPrimaryVideo) in.recordPrimaryVideo();
    performance_.markVideoProcessDone(slot.command, in.slotIndex);

    if (!in.presentationEnabled && in.diagnosticMode == 0u) {
        // A temporarily unavailable viewfinder must not consume the rest of
        // this frame's GPU budget. The encoder work above is already recorded.
        if (!rawTransferCopy) {
            releaseRawInputImage(slot.command, in.raw, in.queueFamily, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
        }
        performance_.markRawDone(slot.command, in.slotIndex);
        performance_.markTonemapInputReady(slot.command, in.slotIndex);
        performance_.markTonemapDone(slot.command, in.slotIndex);
        performance_.markTonemapRepeatInputReady(slot.command, in.slotIndex);
        performance_.markTonemapRepeatDone(slot.command, in.slotIndex);
        performance_.markScopesDone(slot.command, in.slotIndex);
        performance_.markOverlayDone(slot.command, in.slotIndex);
        performance_.markPresentationDone(slot.command, in.slotIndex);
        performance_.markFrameDone(slot.command, in.slotIndex);
        return {rawReadStage};
    }

    raw_preview::RawFrameParameters params{};
    for (size_t i = 0; i < 4; ++i) {
        params.blackLevel[i] = in.black[i];
        params.whiteBalance[i] = in.whiteBalance[i];
    }
    params.whiteLevel = in.white;
    params.pattern = static_cast<raw_preview::BayerPattern>(in.cfa);
    params.highlightReconstructionEnabled = in.highlightReconstructionEnabled;
    params.highlightMethod = in.highlightMethod;
    params.highlightThreshold = in.highlightThreshold;
    params.highlightCompression = in.highlightCompression;
    params.bypassHighlightTone = in.tonemapParams.renderTransform != tonemap::RenderTransform::Existing;
    // The tone tap must see the exposure of the render that follows it: film
    // ignores tonemap render exposure and applies its own folded EV.
    const std::shared_ptr<spektrafilm_native::SpektraFilm> film = acquireFilm_();
    const bool useFilm = in.diagnosticMode == 0u && in.filmEnabled && film != nullptr;
    params.highlightExposureGain =
        useFilm ? std::exp2(rawrcam::color::filmExposureEv(in.filmLook, in.tonemapParams.aePostGain))
                : std::max(in.tonemapParams.aePostGain * std::exp2(in.tonemapParams.exposureEV), 1.0e-6f);
    raw_preview::RawPreviewRecordInfo rawRecord{};
    rawRecord.commandBuffer = slot.command;
    rawRecord.inputRawR16UintView = rawPreviewInputView;
    rawRecord.inputRawImportBuffer = useBufferDirect ? in.raw.importBuffer : VK_NULL_HANDLE;
    rawRecord.inputRawStridePixels = useBufferDirect ? in.raw.importBufferStridePixels : 0u;
    rawRecord.outputLinearRgba16fView = slot.linear.view;
    // Always retain sensor-domain clipping provenance. RAW overlays and the
    // post-RGB recovery share this classification; the reconstruction toggle
    // does not alter what the sensor reported as clipped.
    rawRecord.outputCfaStateR16UintView = monitor_.rawStateView(in.slotIndex);
    rawRecord.width = in.rawWidth;
    rawRecord.height = in.rawHeight;
    rawRecord.parameters = params;
    if (in.lensShadingCorrectionEnabled && !in.metadata.lensShadingMap.empty()) {
        if (auditSubmitCount < 3u && audit_) {
            const auto mm = std::minmax_element(in.metadata.lensShadingMap.begin(), in.metadata.lensShadingMap.end());
            audit_("LENS_SHADING_GPU_ACTIVE map=" + std::to_string(in.metadata.lensShadingMapWidth) + "x" +
                   std::to_string(in.metadata.lensShadingMapHeight) + " gainMin=" + std::to_string(*mm.first) +
                   " gainMax=" + std::to_string(*mm.second) + " domain=post_black_pre_wb fused=true");
        }
        rawRecord.lensShadingMapWidth = in.metadata.lensShadingMapWidth;
        rawRecord.lensShadingMapHeight = in.metadata.lensShadingMapHeight;
        rawRecord.lensShadingMap = in.metadata.lensShadingMap.data();
        rawRecord.lensShadingMapFloatCount = in.metadata.lensShadingMap.size();
    }
    const raw_preview::RawPreviewRecordResult previewResult = rawPreview_.record(rawRecord);
    // Display input: the linear image, or its SDR highlight-compressed copy
    // under Inpaint Opposed. RAW-domain consumers keep reading slot.linear.
    // The linear test pattern (diagnostic mode 3) replaces the preview content.
    const bool displaySdr = previewResult.sdrImage != VK_NULL_HANDLE && in.diagnosticMode != 3u;
    const VkImage displayImage = displaySdr ? previewResult.sdrImage : slot.linear.image;
    const VkImageView displayView = displaySdr ? previewResult.sdrView : slot.linear.view;
    if (in.diagnosticMode == 15u && in.raw.copyBufferAvailable) {
        integrityProbe_.endProductionRawBenchmark(slot.command, in.slotIndex);
        rawrcam::diagnostics::RawIntegrityProbe::BufferPreviewParams bufferParams{};
        for (size_t i = 0; i < 4; ++i) {
            bufferParams.black[i] = params.blackLevel[i];
            bufferParams.whiteBalance[i] = params.whiteBalance[i];
        }
        bufferParams.whiteLevel = params.whiteLevel;
        bufferParams.clipThreshold = params.clipThreshold;
        bufferParams.edgeStrength = params.edgeStrength;
        bufferParams.chromaBlend = params.chromaBlend;
        bufferParams.highlightWarningThreshold = params.highlightWarningThreshold;
        bufferParams.shadowWarningThreshold = params.shadowWarningThreshold;
        bufferParams.pattern = static_cast<uint32_t>(params.pattern);
        integrityProbe_.recordBufferPreviewBenchmark(slot.command, in.slotIndex, transferOracleImage, in.raw.copyBuffer,
                                                     bufferParams);
    }
    if (auditSubmitCount < 7 && audit_) {
        audit_("PIPELINE_RAW_PREVIEW_RECORDED generation=" + std::to_string(in.generation) +
               " timestampNs=" + std::to_string(in.timestampNs) + " input=" + std::to_string(rawRecord.width) + "x" +
               std::to_string(rawRecord.height) + " whiteLevel=" + std::to_string(params.whiteLevel));
    }
    performance_.markRawDone(slot.command, in.slotIndex);

    if (in.diagnosticMode == 3u) {
        rawrcam::vulkan::computeWriteToComputeWrite(slot.command, slot.linear.image);
        diagnostics_.recordLinearPattern(slot.command, in.slotIndex);
    }

    VkImageMemoryBarrier linearBarrier{};
    linearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    linearBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    linearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    linearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    linearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    linearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    linearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    linearBarrier.image = slot.linear.image;
    linearBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier displayBarrier = linearBarrier;
    displayBarrier.image = displayImage;
    const VkImageMemoryBarrier linearBarriers[2] = {linearBarrier, displayBarrier};
    vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, displayImage == slot.linear.image ? 1u : 2u, linearBarriers);

    performance_.markTonemapInputReady(slot.command, in.slotIndex);

    // Film simulation replaces tonemap 1:1 (same GENERAL layouts, same
    // barriers/timing points). The film input is the linear image downsampled
    // by filmPreviewDivisor (2 = quarter-res default; 3/4 for hot GPUs).
    // Tonemap path below is untouched; film runs only in the production
    // diagnostic mode.
    // `film` (snapshotted above, before the RAW record) keeps the engine alive
    // for this frame even if it is retired on another thread mid-record.
    const uint32_t divisor = std::clamp(in.filmPreviewDivisor, 2u, 4u);
    const uint32_t filmWidth = std::max(1u, in.previewWidth / divisor);
    const uint32_t filmHeight = std::max(1u, in.previewHeight / divisor);
    if (useFilm) {
        // Linear -> film input (linear filter blit at the divisor scale).
        VkImageMemoryBarrier blitSrc{};
        blitSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        blitSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        blitSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        blitSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        blitSrc.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        blitSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        blitSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        blitSrc.image = displayImage;
        blitSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier blitDst{};
        blitDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        blitDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        blitDst.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        blitDst.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        blitDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        blitDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        blitDst.image = slot.filmQuarterLinear.image;
        blitDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageMemoryBarrier blitBarriers[2] = {blitSrc, blitDst};
        vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, blitBarriers);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(in.previewWidth), static_cast<int32_t>(in.previewHeight), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {static_cast<int32_t>(filmWidth), static_cast<int32_t>(filmHeight), 1};
        vkCmdBlitImage(slot.command, displayImage, VK_IMAGE_LAYOUT_GENERAL, slot.filmQuarterLinear.image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        VkImageMemoryBarrier filmReady{};
        filmReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        filmReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        filmReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        filmReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        filmReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        filmReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        filmReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        filmReady.image = slot.filmQuarterLinear.image;
        filmReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier linearRead{};
        linearRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        linearRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        linearRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        linearRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        linearRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        linearRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        linearRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        linearRead.image = displayImage;
        linearRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageMemoryBarrier readyBarriers[2] = {filmReady, linearRead};
        vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, readyBarriers);

        spektrafilm_native::SpektraFilmRecordInfo filmRecord{};
        filmRecord.commandBuffer = slot.command;
        filmRecord.input = {slot.filmQuarterLinear.view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                            filmWidth, filmHeight};
        filmRecord.output = {slot.filmQuarterOut.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, filmWidth,
                             filmHeight};
        filmRecord.frameSlot = in.slotIndex;
        filmRecord.look = in.filmLook;
        filmRecord.timeSec = static_cast<double>(in.timestampNs) / 1e9;
        filmRecord.look.filmExposureEv = rawrcam::color::filmExposureEv(in.filmLook, in.tonemapParams.aePostGain);
        // The film input buffer holds sensor-native linear; the film model
        // declares LinearRec709 input, so convert on load (same matrix the
        // tonemap path uses in-shader).
        static_assert(sizeof(filmRecord.sensorToLinearSrgb) == sizeof(in.sensorToLinearSrgb),
                      "sensor matrix size mismatch");
        std::memcpy(filmRecord.sensorToLinearSrgb, in.sensorToLinearSrgb.data(), sizeof(filmRecord.sensorToLinearSrgb));
        // False only when the fallback below ran instead of the film chain.
        bool filmOk = true;
        try {
            film->record(filmRecord);
        } catch (const std::exception& e) {
            // Never let a bad look kill preview: fall back to tonemap for
            // this frame (writes slot.tonemapped directly, like below).
            if (audit_) audit_(std::string("PIPELINE_FILM_SIM_FALLBACK error=") + e.what());
            const auto cameraToAp1 = composeCameraToAp1(in.sensorToLinearSrgb);
            tonemap::TonemapRecordInfo toneRecord{};
            toneRecord.commandBuffer = slot.command;
            toneRecord.input = {displayView, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, in.previewWidth,
                                in.previewHeight};
            toneRecord.output = {slot.tonemapped.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL,
                                 in.previewWidth, in.previewHeight};
            toneRecord.frameSlot = in.slotIndex;
            toneRecord.cameraToWorkingColumnMajor3x3 = cameraToAp1.data();
            toneRecord.params = in.tonemapParams;
            tonemap_.record(toneRecord);
            performance_.markTonemapDone(slot.command, in.slotIndex);
            performance_.markTonemapRepeatInputReady(slot.command, in.slotIndex);
            performance_.markTonemapRepeatDone(slot.command, in.slotIndex);
            filmOk = false;
        }
        if (filmOk) {
            // Upscale the film output into the half-res tonemapped
            // slot (LINEAR): every downstream consumer (scopes, overlay, probe,
            // presentation) then works unchanged, on the film image.
            VkImageMemoryBarrier filmOutReady{};
            filmOutReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            filmOutReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            filmOutReady.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            filmOutReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            filmOutReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            filmOutReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            filmOutReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            filmOutReady.image = slot.filmQuarterOut.image;
            filmOutReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier toneWrite{};
            toneWrite.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toneWrite.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            toneWrite.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toneWrite.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toneWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toneWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toneWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toneWrite.image = slot.tonemapped.image;
            toneWrite.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            const VkImageMemoryBarrier upscaleBarriers[2] = {filmOutReady, toneWrite};
            vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 2, upscaleBarriers);
            VkImageBlit upscale{};
            upscale.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            upscale.srcOffsets[1] = {static_cast<int32_t>(filmWidth), static_cast<int32_t>(filmHeight), 1};
            upscale.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            upscale.dstOffsets[1] = {static_cast<int32_t>(in.previewWidth), static_cast<int32_t>(in.previewHeight), 1};
            vkCmdBlitImage(slot.command, slot.filmQuarterOut.image, VK_IMAGE_LAYOUT_GENERAL, slot.tonemapped.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &upscale, VK_FILTER_LINEAR);
            VkImageMemoryBarrier toneRead{};
            toneRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toneRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toneRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toneRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toneRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toneRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toneRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toneRead.image = slot.tonemapped.image;
            toneRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &toneRead);
            performance_.markTonemapDone(slot.command, in.slotIndex);
            performance_.markTonemapRepeatInputReady(slot.command, in.slotIndex);
            performance_.markTonemapRepeatDone(slot.command, in.slotIndex);
            if (filmOk && auditSubmitCount < 7 && audit_) {
                audit_("PIPELINE_FILM_SIM_RECORDED generation=" + std::to_string(in.generation) +
                       " timestampNs=" + std::to_string(in.timestampNs) + " input=" + std::to_string(filmWidth) + "x" +
                       std::to_string(filmHeight));
            }
        }  // end if (filmOk): upscale film output, else fallback already ran
    } else {
        const auto cameraToAp1 = composeCameraToAp1(in.sensorToLinearSrgb);
        tonemap::TonemapRecordInfo toneRecord{};
        toneRecord.commandBuffer = slot.command;
        toneRecord.input = {displayView, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, in.previewWidth,
                            in.previewHeight};
        toneRecord.output = {slot.tonemapped.view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, in.previewWidth,
                             in.previewHeight};
        toneRecord.frameSlot = in.slotIndex;
        toneRecord.cameraToWorkingColumnMajor3x3 = cameraToAp1.data();
        toneRecord.params = in.tonemapParams;
        tonemap_.record(toneRecord);
        performance_.markTonemapDone(slot.command, in.slotIndex);
        if (repeatProbe) {
            rawrcam::vulkan::computeWriteToComputeWrite(slot.command, slot.tonemapped.image);
            performance_.markTonemapRepeatInputReady(slot.command, in.slotIndex);
            tonemap_.record(toneRecord);
            performance_.markTonemapRepeatDone(slot.command, in.slotIndex);
        } else {
            performance_.markTonemapRepeatInputReady(slot.command, in.slotIndex);
            performance_.markTonemapRepeatDone(slot.command, in.slotIndex);
        }
    }  // else: tonemap path

    monitor_.recordOverlays(monitorFrame);

    if (in.diagnosticMode == 2u) {
        rawrcam::vulkan::computeWriteToComputeWrite(slot.command, slot.tonemapped.image);
        diagnostics_.recordTonePattern(slot.command, in.slotIndex);
    }

    if (integrityProbe_.readyForCapture() &&
        rawrcam::diagnostics::RawIntegrityProbePolicy::shouldCapture(in.diagnosticMode)) {
        rawrcam::diagnostics::RawIntegrityProbe::AdvancedCandidates advanced{};
        if (in.diagnosticMode == 15u) {
            advanced.drmImage = in.raw.drmImage;
            advanced.drmView = in.raw.drmView;
            advanced.drmUsage = in.raw.drmUsage;
            advanced.drmAvailable = in.raw.drmCandidateAvailable;
            advanced.copyBuffer = in.raw.copyBuffer;
            advanced.copyBufferAvailable = in.raw.copyBufferAvailable;
            advanced.externalFormatImage = in.raw.externalFormatImage;
            advanced.externalFormatAvailable = in.raw.externalFormatCandidateAvailable;
            advanced.externalFormat = in.raw.externalFormat;
            advanced.externalFormatFeatures = in.raw.externalFormatFeatures;
            advanced.externalComponents = in.raw.externalComponents;
            advanced.externalModel = in.raw.externalSuggestedModel;
            advanced.externalRange = in.raw.externalSuggestedRange;
            advanced.externalX = in.raw.externalSuggestedX;
            advanced.externalY = in.raw.externalSuggestedY;
        }
        integrityProbe_.record(slot.command, zeroCopySweep ? in.raw.view : rawPreviewInputView, rawPreviewInputView,
                               zeroCopySweep, in.raw.linearCandidateAvailable ? in.raw.linearView : rawPreviewInputView,
                               zeroCopySweep && in.raw.linearCandidateAvailable, advanced, slot.linear.view,
                               slot.tonemapped.view, slot.linear.image, slot.tonemapped.image, in.slotIndex,
                               in.timestampNs);
    }

    if (zeroCopySweep) {
        if (separateTransferReference) {
            rawrcam::vulkan::releaseForeignImage(slot.command, in.raw.referenceTransferImage, in.queueFamily,
                                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        if (in.raw.linearCandidateAvailable) {
            rawrcam::vulkan::releaseForeignImage(slot.command, in.raw.linearImage, in.queueFamily,
                                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        if (in.diagnosticMode == 15u && in.raw.externalFormatCandidateAvailable) {
            rawrcam::vulkan::releaseForeignImage(slot.command, in.raw.externalFormatImage, in.queueFamily,
                                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        if (in.diagnosticMode == 15u && in.raw.drmCandidateAvailable) {
            rawrcam::vulkan::releaseForeignImage(slot.command, in.raw.drmImage, in.queueFamily,
                                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        releaseRawInputImage(slot.command, in.raw, in.queueFamily, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
    } else if (in.diagnosticMode != 6u && !rawTransferCopy) {
        releaseRawInputImage(slot.command, in.raw, in.queueFamily, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
    }
    // Idle video-mode crop window in preview-source pixels. Shares the exact
    // record rect (VideoCrop.h), so the idle preview frames what the recorder
    // will write and focus/face mapping can track it. Diagnostic
    // visualizations always show the full frame.
    rawrcam::video::VideoSourceRect previewCrop{};
    if (in.diagnosticMode == 0u && in.videoCropOutWidth != 0 && in.videoCropOutHeight != 0) {
        const auto full =
            rawrcam::video::videoSourceRect(in.rawWidth, in.rawHeight, in.videoCropOutWidth, in.videoCropOutHeight);
        if (full.width != 0) {
            previewCrop = rawrcam::video::scaleVideoSourceRect(full, in.rawWidth, in.rawHeight, in.previewWidth,
                                                               in.previewHeight);
        }
    }
    monitor_.present(monitorFrame, previewCrop.x, previewCrop.y, previewCrop.width, previewCrop.height);
    performance_.markPresentationDone(slot.command, in.slotIndex);
    performance_.markFrameDone(slot.command, in.slotIndex);
    return {rawReadStage};
}

std::array<float, 9> RawDevelopRecorder::composeCameraToAp1(const std::array<float, 9>& matrix) {
    return rawrcam::color::math::toColumnMajor(
        rawrcam::color::math::multiply(rawrcam::color::math::kLinearSrgbToAcesAp1, matrix));
}

void RawDevelopRecorder::recordVideoScopes(VkCommandBuffer command, uint32_t slot, VkImageView view, VkImage image,
                                           uint32_t width, uint32_t height, int sensor, int rotation) const {
    monitor_.recordVideoScopes(command, slot, view, image, width, height, sensor, rotation);
}
void RawDevelopRecorder::restoreVideoScopes(VkCommandBuffer command, uint32_t slot) const {
    monitor_.restoreVideoScopes(command, slot);
}
}  // namespace rawrcam::pipeline
