#include "develop/render/StillRenderSequence.h"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

#include "color/FilmExposure.h"
#include "develop/render/FilmRenderStage.h"
#include "develop/render/RenderReadback.h"

namespace rawrcam::develop::rendered {
namespace {
void recordPostDemosaic(RenderResources& resources, RenderCommandSession& commands,
                        const RenderedStillContext& rendered, VkImage sourceImage, VkImageView sourceView,
                        VkImage clipStateImage, VkImageView clipStateView) {
    const bool effectiveHighlightReconstruction = rendered.highlightReconstructionEnabled || rendered.ultraHdrEnabled;
    VkImageMemoryBarrier src{};
    src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    src.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    src.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    src.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src.image = sourceImage;
    src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &src);
    rawr::post::DenoiseRequest denoiseRequest{};
    denoiseRequest.strength = rendered.denoiseStrength;
    denoiseRequest.detail = rendered.denoiseDetail;
    denoiseRequest.forceY = rendered.denoiseForceY;
    denoiseRequest.maxScale = rendered.denoiseMaxScale;
    denoiseRequest.noiseA = rendered.denoiseNoiseA;
    denoiseRequest.noiseB = rendered.denoiseNoiseB;
    // One submission per wavelet tile: the whole-frame denoise is
    // ~400 ms of GPU work and stalled the viewfinder as one submit.
    resources.postDemosaic().setDenoiseTileBoundary([&] { commands.submitAndRestart("denoise-tile"); });
    try {
        resources.postDemosaic().record(
            commands.command(), sourceImage, sourceView, clipStateImage, clipStateView, rendered.whiteBalanceRgb,
            effectiveHighlightReconstruction,
            rawr::post::StillDistortionCorrection{rendered.distortionCorrectionEnabled && rendered.hasLensCalibration,
                                                  rendered.lensIntrinsic, rendered.lensDistortion},
            commands.timingPool(), false, rendered.highlightReconstructionMethod, rendered.highlightThreshold,
            rendered.highlightCompression,
            // Film ignores tonemap render exposure; mirror its folded EV.
            rendered.filmEnabled
                ? std::exp2(rawrcam::color::filmExposureEv(rendered.filmLook, rendered.tonemapParams.aePostGain))
                : std::max(rendered.tonemapParams.aePostGain, 1.0e-6f) * std::exp2(rendered.tonemapParams.exposureEV),
            rendered.lensShading.view(), rendered.cfaPattern, denoiseRequest);
    } catch (...) {
        resources.postDemosaic().setDenoiseTileBoundary({});
        throw;
    }
    resources.postDemosaic().setDenoiseTileBoundary({});
    VkImageMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    post.image = resources.postDemosaic().outputImage();
    post.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &post);
    // Bound each submission (post-demosaic, tone/film, gainmap+readback)
    // so the preview queue can run between them.
    commands.submitAndRestart("post-demosaic");
}
bool runGaloshYuv(RenderResources& resources, RenderCommandSession& commands, const RenderDeviceContext& device,
                  const RenderedStillContext& rendered, const RenderResources::Diagnostic& emit) {
    // GALOSH-YUV blind denoise (P1c): in-place on the linear SDR
    // buffer after FCC/defringe, before the tonemap/film branch
    // (both consume sdr, so one tap serves both). process() runs
    // its own submits, so flush the recorded prefix first — same
    // staleness rule as the RAW tap (SharedHighlightRuntime); the
    // restart barrier covers cross-submit visibility. HDR knee
    // keeps highlights exact for the tonemap. Blind: valid on
    // merged frames where wavelet stays off. Any failure degrades
    // to the legacy path (never fail the shot for denoise).
    // Timing slots 12-15 have no stage owner: close them here as
    // adjacent 0ms pairs FIRST (process() overwrites 14-15 with
    // real timings when it runs; repeat writes are last-wins), so
    // the fixed 0-16 fetch never wedges on unwritten queries even
    // when the tap is off or throws mid-frame.
    if (commands.timingPool()) {
        vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 12);
        vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 13);
        vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 14);
        vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 15);
    }
    const bool galoshYuvRequested = (rendered.galoshYuvMode == 1 || rendered.galoshYuvMode == 2);
    const bool galoshYuvOn =
        galoshYuvRequested && device.float16Compute && rendered.width >= 64 && rendered.height >= 64;
    bool galoshYuvSuccess = false;
    if (rendered.galoshYuvMode != 0 && !galoshYuvOn) {
        std::string reason = !galoshYuvRequested ? "invalid_mode" : !device.float16Compute ? "no_f16" : "too_small";
        emit("GALOSH_YUV_SKIP requestId=" + std::to_string(rendered.requestId) + " reason=" + reason);
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "GALOSH_YUV_SKIP requestId=%llu reason=%s",
                            (unsigned long long)rendered.requestId, reason.c_str());
    }
    if (galoshYuvOn) {
        // resources.ensureGaloshYuv() (pipeline build) runs before the prefix
        // flush so a throw there leaves the recorded prefix intact —
        // safe to degrade to legacy. The flush itself uses vkCheck
        // (driver-level: fail the shot, don't record on an ended CB).
        // process() runs after the flush on a fresh CB, so a throw
        // there also leaves a recording outer CB — safe for fallback.
        bool galoshYuvAttempt = true;
        try {
            resources.ensureGaloshYuv();
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "GALOSH_YUV_FALLBACK requestId=%llu reason=%s",
                                (unsigned long long)rendered.requestId, e.what());
            emit("GALOSH_YUV_FALLBACK requestId=" + std::to_string(rendered.requestId) + " reason=" + e.what());
            galoshYuvAttempt = false;
        }
        if (galoshYuvAttempt) {
            __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                "GALOSH_YUV_TAP requestId=%llu mode=%d wh=%ux%u sy=%.2f sc=%.2f",
                                (unsigned long long)rendered.requestId, rendered.galoshYuvMode, rendered.width,
                                rendered.height, rendered.galoshYuvStrengthY, rendered.galoshYuvStrengthC);
            commands.submitAndRestart("galosh yuv prefix");
            try {
                ::galosh::GaloshYuvParams yuv{};
                yuv.mode = static_cast<::galosh::GaloshYuvMode>(rendered.galoshYuvMode);
                yuv.strengthY = std::clamp(rendered.galoshYuvStrengthY, 0.0f, 8.0f);
                yuv.strengthC = std::clamp(rendered.galoshYuvStrengthC, 0.0f, 8.0f);
                resources.galoshYuv().process(device.queue, *device.queueSubmitMutex,
                                              resources.postDemosaic().sdrOutputView(),
                                              resources.postDemosaic().sdrOutputView(), rendered.width, rendered.height,
                                              yuv, commands.timingPool());
                galoshYuvSuccess = true;
            } catch (const std::exception& e) {
                // Logcat-visible AND callback: a throwing tap must never
                // be silent (multiframe runs without a log watcher).
                __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "GALOSH_YUV_FALLBACK requestId=%llu reason=%s",
                                    (unsigned long long)rendered.requestId, e.what());
                emit("GALOSH_YUV_FALLBACK requestId=" + std::to_string(rendered.requestId) + " reason=" + e.what());
            }
            // Post-tap visibility: process() runs fence-waited submits
            // writing the SDR image in place. Fences are host-side only;
            // same-queue visibility needs an image barrier before the
            // tonemap/film reads below. Without this the tap can appear
            // to do nothing (stale reads) and validation reports
            // READ_AFTER_WRITE. Recorded on both success and fallback so
            // partial writes are at least visible.
            if (VkImage sdrImage = resources.postDemosaic().sdrOutputImage(); sdrImage != VK_NULL_HANDLE) {
                VkImageMemoryBarrier yuvReady{};
                yuvReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                yuvReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                yuvReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                yuvReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                yuvReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                yuvReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                yuvReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                yuvReady.image = sdrImage;
                yuvReady.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &yuvReady);
            }
        }
    }

    return galoshYuvSuccess;
}
struct GainmapRecording {
    bool clipUsed;
    float clipBoost;
};
GainmapRecording recordGainmapAndReadback(RenderResources& resources, RenderCommandSession& commands,
                                          const RenderedStillContext& rendered, VkImage clipStateImage,
                                          VkImageView clipStateView, bool filmUsedGlow,
                                          const RenderResources::Diagnostic& emit) {
    // Cached for the GPU-timing log below (gri dies with its block).
    bool gainmapClipUsed = false;
    float gainmapClipBoost = 0.0f;
    if (rendered.ultraHdrEnabled) {
        VkImageMemoryBarrier mapInit{};
        mapInit.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mapInit.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mapInit.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mapInit.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        mapInit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mapInit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mapInit.image = resources.map().image;
        mapInit.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier sdrRead{};
        sdrRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sdrRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        sdrRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sdrRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sdrRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        sdrRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sdrRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sdrRead.image = resources.output().image;
        sdrRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier preGain[2] = {mapInit, sdrRead};
        vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, preGain);
        if (filmUsedGlow) {
            // Film wrote the glow quotient above (SHADER_WRITE):
            // GENERAL->GENERAL read-after-write for the gain-map read.
            VkImageMemoryBarrier glowRead{};
            glowRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            glowRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            glowRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            glowRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            glowRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            glowRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            glowRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            glowRead.image = resources.glow().image;
            glowRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &glowRead);
        }
        if (commands.timingPool())
            vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 8);
        // The clip mask (when the caller supplied one) may have been
        // written by the highlight/derive stages earlier in this same
        // command buffer: GENERAL->GENERAL read-after-write barrier.
        // Contract: half-res R16UI on the map grid (all producers
        // honor this; MFS ceil-vs-floor can oversize by 1px, never
        // undersize, so gid indexing stays in bounds).
        gainmap::GainmapRecordInfo gri{};
        gri.commandBuffer = commands.command();
        // The HDR tap is always the scene-linear post-demosaic image
        // (white-balanced camera RGB); film glow rides as a separate
        // quotient input so the tap needs no per-stock scale.
        gri.hdr = {resources.postDemosaic().outputView(), VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                   rendered.width, rendered.height};
        if (filmUsedGlow) {
            gri.glow = {resources.glow().view, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, rendered.width,
                        rendered.height};
        }
        gri.sdr = {resources.output().view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, rendered.width,
                   rendered.height};
        gri.map = {resources.map().view, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL,
                   resources.outputView().gainmapWidth, resources.outputView().gainmapHeight};
        if (clipStateView != VK_NULL_HANDLE && clipStateImage != VK_NULL_HANDLE) {
            VkImageMemoryBarrier clipRead{};
            clipRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            clipRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            clipRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            clipRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            clipRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            clipRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clipRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clipRead.image = clipStateImage;
            clipRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &clipRead);
            gri.clip = {clipStateView, VK_FORMAT_R16_UINT, VK_IMAGE_LAYOUT_GENERAL, resources.outputView().gainmapWidth,
                        resources.outputView().gainmapHeight};
        }
        // Cached for the GPU-timing log below (gri dies with its block).
        gri.params = rendered.gainmapParams;
        gainmapClipUsed = gri.clip.view != VK_NULL_HANDLE;
        gainmapClipBoost = gri.params.clipBoost;
        // HDR pixels are camera RGB: the calibrated matrix wins over
        // the AP1->sRGB default baked into GainmapParams. The glow
        // quotient needs no matrix (dimensionless post/pre ratio).
        if (rendered.hasGainmapCst) {
            std::memcpy(gri.params.hdrToLinearSrgbRowMajor, rendered.gainmapCstRowMajor.data(),
                        sizeof(gri.params.hdrToLinearSrgbRowMajor));
        }
        if (filmUsedGlow) {
            emit("STILL_GAINMAP_HDR_TAP tap=film_glow_gain");
            __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                "STILL_GAINMAP_HDR_TAP tap=film_glow_gain requestId=%llu",
                                (unsigned long long)rendered.requestId);
        }
        resources.gainmap().record(gri);
        if (commands.timingPool())
            vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 9);
    } else if (commands.timingPool()) {
        // Keep queries 8-9 written so the timing fetch below stays a
        // fixed 12-query read (0ms when gainmap is off).
        vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 8);
        vkCmdWriteTimestamp(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, commands.timingPool(), 9);
    }

    VkImageMemoryBarrier copyBarrier{};
    copyBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // Film path's last writer is the upscale blit (transfer); tonemap
    // path's is the shader. Cover both.
    copyBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarrier.dstAccessMask = rendered.surfacePreview ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    copyBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    copyBarrier.newLayout =
        rendered.surfacePreview ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarrier.image = resources.output().image;
    copyBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(
        commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        rendered.surfacePreview ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
        0, nullptr, 1, &copyBarrier);
    // Development inputs ride along so a black still can be told apart from black RAW in device logs.
    const auto& cam = rendered.cameraToWorkingColumnMajor;
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                        "STILL_RENDER_RECORDED requestId=%llu ultraHdr=%d dims=%ux%u cfa=%u film=%d "
                        "wbRgb=[%.4f,%.4f,%.4f] exposureEV=%.3f aePostGain=%.4f "
                        "camToWorkingColMajor=[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]",
                        (unsigned long long)rendered.requestId, rendered.ultraHdrEnabled ? 1 : 0, rendered.width,
                        rendered.height, rendered.cfaPattern, rendered.filmEnabled ? 1 : 0, rendered.whiteBalanceRgb[0],
                        rendered.whiteBalanceRgb[1], rendered.whiteBalanceRgb[2], rendered.tonemapParams.exposureEV,
                        rendered.tonemapParams.aePostGain, cam[0], cam[1], cam[2], cam[3], cam[4], cam[5], cam[6],
                        cam[7], cam[8]);
    if (!rendered.surfacePreview) {
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {rendered.width, rendered.height, 1};
        vkCmdCopyImageToBuffer(commands.command(), resources.output().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               resources.readbackBuffer(), 1, &bic);
    }
    if (rendered.ultraHdrEnabled) {
        VkImageMemoryBarrier mapCopy{};
        mapCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mapCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mapCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mapCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        mapCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mapCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mapCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mapCopy.image = resources.map().image;
        mapCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &mapCopy);
        VkBufferImageCopy mbc{};
        mbc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        mbc.imageExtent = {resources.outputView().gainmapWidth, resources.outputView().gainmapHeight, 1};
        vkCmdCopyImageToBuffer(commands.command(), resources.map().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               resources.gainmapReadbackBuffer(), 1, &mbc);
    }

    return {gainmapClipUsed, gainmapClipBoost};
}
}  // namespace
RenderedStillCompletion executeStillRender(RenderResources& resources, RenderCommandSession& commands,
                                           const RenderDeviceContext& device, const RenderedStillContext& rendered,
                                           VkImage sourceImage, VkImageView sourceView, VkImage clipStateImage,
                                           VkImageView clipStateView, const std::string& filesDir,
                                           const RenderResources::Diagnostic& emit) {
    RenderedStillCompletion done{};
    done.requestId = rendered.requestId;
    done.width = rendered.width;
    done.height = rendered.height;
    {
        const bool highlight = rendered.highlightReconstructionEnabled || rendered.ultraHdrEnabled;
        done.highlightStage = !highlight ? 0 : (rendered.highlightReconstructionMethod == 1u ? 2 : 1);
        done.highlightForcedByUltraHdr = !rendered.highlightReconstructionEnabled && rendered.ultraHdrEnabled;
        done.defringeEnabled = std::isfinite(rendered.defringeStrength) && rendered.defringeStrength > 0.0f;
    }
    done.presentationQuarterTurns = rendered.presentationQuarterTurns;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        resources.createPixels(rendered);
        done.engineBuildMs = resources.prepareEngines(rendered);
        const bool hasSensorClipState = clipStateView != VK_NULL_HANDLE;
        // Clip provenance: single-frame stills carry sensor CFA evidence
        // from SharedHighlightRuntime; multiframe carries the prepareRgb
        // mask (merged-derived, optionally ORed with reference sensor bits
        // via 1A). A null view falls back to PostDemosaicProcessor's
        // conservative pre-WB RGB derive.
        const bool effectiveHighlightReconstruction =
            rendered.highlightReconstructionEnabled || rendered.ultraHdrEnabled;
        if (rendered.ultraHdrEnabled && !rendered.highlightReconstructionEnabled)
            emit("STILL_GAINMAP_RECOVERY_FORCED recovery=on requestId=" + std::to_string(rendered.requestId));
        const char* clipEvidence = !hasSensorClipState                      ? "derived_normalized_rgb"
                                   : rendered.multiframeSensorClipOrApplied ? "multiframe_sensor_or"
                                   : rendered.clipFromMultiframe            ? "multiframe_derived_rgb"
                                                                            : "sensor_cfa";
        emit(std::string("STILL_HIGHLIGHT_RECOVERY_CONFIGURED mode=") +
             (rendered.highlightReconstructionMethod == 1u ? "coloropp" : "guided_channel_fit") +
             " requested=" + (rendered.highlightReconstructionEnabled ? "true" : "false") +
             " enabled=" + (effectiveHighlightReconstruction ? "true" : "false") + " evidence=" + clipEvidence +
             " domain=" + (rendered.highlightReconstructionMethod == 1u ? "pre_wb_camera_rgb" : "post_wb_camera_rgb") +
             " requestId=" + std::to_string(rendered.requestId) + " steps=" + std::to_string(rendered.fccSteps) +
             " threshold=" + std::to_string(rendered.highlightThreshold) +
             " compression=" + std::to_string(rendered.highlightCompression) +
             " defringe=" + std::to_string(done.defringeEnabled ? std::min(rendered.defringeStrength, 1.0f) : 0.0f) +
             " fccBytes=" + std::to_string(resources.postDemosaic().fccAllocatedBytes()));

        const bool preWbOk = !rendered.diagnosticsEnabled ||
                             dumpRenderImage(device, sourceImage, rendered.width, rendered.height,
                                             filesDir + "/rawrcam_diag_pre_wb_latest.rgba16f", rendered.diagnosticRoi);

        commands.create(device);
        commands.begin();
        recordPostDemosaic(resources, commands, rendered, sourceImage, sourceView, clipStateImage, clipStateView);

        const bool galoshYuvSuccess = runGaloshYuv(resources, commands, device, rendered, emit);

        VkImageMemoryBarrier dst{};
        dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dst.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dst.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dst.image = resources.output().image;
        dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands.command(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &dst);

        const auto film = renderFilmOrTone(resources, commands, rendered, sourceView, filesDir, emit);
        done.filmRendered = film.rendered;
        done.filmFallbackMemory = film.memoryRejected;
        if (film.memoryRejected) throw std::runtime_error("film_memory_requires_dng");
        const bool filmUsedGlow = film.usedGlow;

        commands.submitAndRestart("tone");

        const auto gainmap =
            recordGainmapAndReadback(resources, commands, rendered, clipStateImage, clipStateView, filmUsedGlow, emit);
        const auto tSubmit = std::chrono::steady_clock::now();
        commands.finish(rendered.requestId);
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "STILL_RENDER_FENCE_DONE requestId=%llu",
                            (unsigned long long)rendered.requestId);
        const auto tFence = std::chrono::steady_clock::now();
        // Setup is CPU-side only (resources, engines, recording); every
        // fence wait (intermediate chunks + final) covers the GPU
        // timestamp ranges plus scheduling and queue gaps, and the
        // residual below attributes exactly that remainder.
        done.renderSetupMs =
            std::max(0.0, std::chrono::duration<double, std::milli>(tSubmit - t0).count() - commands.chunkWaitMs());
        done.queueGapsMs = std::chrono::duration<double, std::milli>(tFence - tSubmit).count() + commands.chunkWaitMs();
        completeReadback(resources, commands, rendered, sourceImage, filesDir, preWbOk, clipEvidence, galoshYuvSuccess,
                         gainmap.clipUsed, gainmap.clipBoost, tFence, done, emit);
        done.success = true;

    } catch (const std::exception& e) {
        done.error = e.what();
        __android_log_print(ANDROID_LOG_ERROR, "RawrCamNative", "STILL_RENDER_FAIL requestId=%llu error=%s",
                            (unsigned long long)rendered.requestId, done.error.c_str());
    }
    done.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return done;
}
}  // namespace rawrcam::develop::rendered
