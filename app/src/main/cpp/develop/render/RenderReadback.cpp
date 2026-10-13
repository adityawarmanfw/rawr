#include "develop/render/RenderReadback.h"

#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "vulkan/HostMemory.h"

namespace rawrcam::develop::rendered {
namespace {
void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}
}  // namespace

bool dumpRenderImage(const RenderDeviceContext& context, VkImage image, uint32_t width, uint32_t height,
                     const std::string& path, const std::array<uint32_t, 4>& roi) {
    if (!image || !width || !height || !context.device || !context.queue || !context.queueSubmitMutex) return false;
    const bool cropped = roi[2] != 0 && roi[3] != 0;
    const uint32_t rw = cropped ? roi[2] : width, rh = cropped ? roi[3] : height;
    if (cropped && (roi[0] >= width || roi[1] >= height || rw > width - roi[0] || rh > height - roi[1])) return false;
    const uint64_t n64 = static_cast<uint64_t>(rw) * rh * 8u;
    if (n64 > std::numeric_limits<VkDeviceSize>::max()) return false;
    const VkDeviceSize n = static_cast<VkDeviceSize>(n64);
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    void* map = nullptr;
    VkCommandPool p = VK_NULL_HANDLE;
    VkCommandBuffer c = VK_NULL_HANDLE;
    VkFence f = VK_NULL_HANDLE;
    try {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = n;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(context.device, &bi, nullptr, &b), "diag buffer");
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(context.device, b, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = vulkan::findCoherentHostMemoryType(context.physicalDevice, mr.memoryTypeBits);
        vkCheck(vkAllocateMemory(context.device, &ai, nullptr, &m), "diag memory");
        vkCheck(vkBindBufferMemory(context.device, b, m, 0), "diag bind");
        vkCheck(vkMapMemory(context.device, m, 0, n, 0, &map), "diag map");
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = context.queueFamily;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCheck(vkCreateCommandPool(context.device, &pi, nullptr, &p), "diag pool");
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = p;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(context.device, &ci, &c), "diag cmd");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCheck(vkCreateFence(context.device, &fi, nullptr, &f), "diag fence");
        VkCommandBufferBeginInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(c, &cb), "diag begin");
        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &ib);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageOffset = {cropped ? int32_t(roi[0]) : 0, cropped ? int32_t(roi[1]) : 0, 0};
        cp.imageExtent = {rw, rh, 1};
        vkCmdCopyImageToBuffer(c, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &cp);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &ib);
        vkCheck(vkEndCommandBuffer(c), "diag end");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        {
            std::lock_guard<std::mutex> q(*context.queueSubmitMutex);
            vkCheck(vkQueueSubmit(context.queue, 1, &si, f), "diag submit");
        }
        vkCheck(vkWaitForFences(context.device, 1, &f, VK_TRUE, UINT64_MAX), "diag wait");
        const std::string tmp = path + ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("diag open");
        out.write(static_cast<const char*>(map), static_cast<std::streamsize>(n));
        out.close();
        if (!out) throw std::runtime_error("diag write");
        if (::rename(tmp.c_str(), path.c_str()) != 0) {
            ::unlink(tmp.c_str());
            throw std::runtime_error("diag rename");
        }
        if (map) vkUnmapMemory(context.device, m);
        if (f) vkDestroyFence(context.device, f, nullptr);
        if (p) vkDestroyCommandPool(context.device, p, nullptr);
        if (b) vkDestroyBuffer(context.device, b, nullptr);
        if (m) vkFreeMemory(context.device, m, nullptr);
        return true;
    } catch (...) {
        if (map && m) vkUnmapMemory(context.device, m);
        if (f) vkDestroyFence(context.device, f, nullptr);
        if (p) vkDestroyCommandPool(context.device, p, nullptr);
        if (b) vkDestroyBuffer(context.device, b, nullptr);
        if (m) vkFreeMemory(context.device, m, nullptr);
        return false;
    }
}

void completeReadback(RenderResources& resources, RenderCommandSession& commands, const RenderedStillContext& rendered,
                      VkImage sourceImage, const std::string& filesDir, bool preWbOk, const char* clipEvidence,
                      bool galoshYuvSuccess, bool gainmapClipUsed, float gainmapClipBoost,
                      std::chrono::steady_clock::time_point tFence, RenderedStillCompletion& done,
                      const RenderResources::Diagnostic& emit) {
    if (rendered.diagnosticsEnabled) {
        const bool preFccOk =
            dumpRenderImage(resources.deviceContext(), sourceImage, rendered.width, rendered.height,
                            filesDir + "/rawrcam_diag_pre_fcc_latest.rgba16f", rendered.diagnosticRoi);
        const bool preToneOk = dumpRenderImage(
            resources.deviceContext(), resources.postDemosaic().outputImage(), rendered.width, rendered.height,
            filesDir + "/rawrcam_diag_pre_tonemap_latest.rgba16f", rendered.diagnosticRoi);
        std::ofstream mf(filesDir + "/rawrcam_diag_manifest_latest.txt", std::ios::trunc);
        if (mf) {
            mf << "RAWR_DEMOSAIC_DIAGNOSTIC_V1\n"
               << "requestId=" << rendered.requestId << "\n"
               << "width=" << rendered.width << "\nheight=" << rendered.height << "\n"
               << "postDemosaicDumpRoi=" << rendered.diagnosticRoi[0] << ',' << rendered.diagnosticRoi[1] << ','
               << rendered.diagnosticRoi[2] << ',' << rendered.diagnosticRoi[3]
               << " zero_extent_means_full_frame; applies_to=preWb,preFcc,preTonemap\n"
               << "packedCfa=rawrcam_diag_packed_cfa_latest.rgba16f format=RGBA16F_LE "
                  "dimensions=width/2,height/2 channels=R,G1,G2,B domain=normalized_LSC_classification_only\n"
               << "postDemosaic=rawrcam_diag_demosaic_latest.rgba16f format=RGBA16F_LE "
                  "domain=pre_WB_camera_linear\n"
               << "dualRcd=rawrcam_diag_dual_rcd_latest.rgba16f format=RGBA16F_LE domain=RCD_only_pre_blend "
                  "present_only_for_dual_diagnostics\n"
               << "dualVng=rawrcam_diag_dual_vng_latest.rgba16f format=RGBA16F_LE domain=VNG4_only_pre_blend "
                  "present_only_for_dual_diagnostics\n"
               << "dualMaskPreBlur=rawrcam_diag_dual_mask_preblur_latest.r32f format=R32F_LE "
                  "domain=raw_RCD_weight_before_gaussian present_only_for_dual_diagnostics\n"
               << "dualMask=rawrcam_diag_dual_mask_latest.r32f format=R32F_LE domain=final_blurred_RCD_weight "
                  "present_only_for_dual\n"
               << "preWb=rawrcam_diag_pre_wb_latest.rgba16f format=RGBA16F_LE "
                  "domain=pre_WB_pre_highlight_camera_linear\n"
               << "preFcc=rawrcam_diag_pre_fcc_latest.rgba16f format=RGBA16F_LE "
                  "domain=post_WB_post_highlight_stage_pre_FCC\n"
               << "preTonemap=rawrcam_diag_pre_tonemap_latest.rgba16f format=RGBA16F_LE "
                  "domain=post_FCC_tonemap_input\n"
               << "postTonemap=rawrcam_prejpeg_latest.ppm format=P6_sRGB8 domain=exact_pre_JPEG\n"
               << "highlightReconstructionRequested=" << (rendered.highlightReconstructionEnabled ? "true" : "false")
               << "\n"
               << "highlightReconstructionApplied=" << (rendered.highlightReconstructionEnabled ? "true" : "false") << "\n"
               << "highlightClipEvidence=" << clipEvidence << "\n"
               << "whiteBalanceRgb=" << rendered.whiteBalanceRgb[0] << ',' << rendered.whiteBalanceRgb[1] << ','
               << rendered.whiteBalanceRgb[2] << "\n"
               << "fccSteps=" << rendered.fccSteps << "\n"
               << "colorRenderProfile="
               << rawrcam::tonemap_integration::colorRenderProfileName(rendered.colorRenderProfile) << "\n"
               << "importedLutProfileId=" << rendered.importedLutProfileId << "\n"
               << "tonemap.exposureEV=" << rendered.tonemapParams.exposureEV << "\n"
               << "tonemap.aePostGain=" << rendered.tonemapParams.aePostGain << "\n"
               << "tonemap.blacks=" << rendered.tonemapParams.blackPointEV << "\n"
               << "tonemap.shadows=" << rendered.tonemapParams.shadowLiftEV << "\n"
               << "tonemap.midtones=" << rendered.tonemapParams.midtoneLiftEV << "\n"
               << "tonemap.contrast=" << rendered.tonemapParams.contrast << "\n"
               << "tonemap.whites=" << rendered.tonemapParams.whitePointEV << "\n"
               << "tonemap.highlights=" << rendered.tonemapParams.highlightBiasEV << "\n"
               << "tonemap.saturation=" << rendered.tonemapParams.saturation << "\n"
               << "tonemap.vibrance=" << rendered.tonemapParams.vibrance << "\n"
               << "cameraToWorkingColumnMajor=";
            for (size_t i = 0; i < rendered.cameraToWorkingColumnMajor.size(); ++i)
                mf << (i ? "," : "") << rendered.cameraToWorkingColumnMajor[i];
            mf << "\n"
               << "preWbDump=" << (preWbOk ? "PASS" : "FAIL") << "\n"
               << "preFccDump=" << (preFccOk ? "PASS" : "FAIL") << "\npreTonemapDump=" << (preToneOk ? "PASS" : "FAIL")
               << "\n";
        }
        emit(std::string("RENDER_DIAGNOSTIC_DUMP ") + (preWbOk && preFccOk && preToneOk ? "PASS" : "FAIL") +
             " requestId=" + std::to_string(rendered.requestId));
    }
    if (commands.timingPool()) {
        const auto timestamps = commands.timestamps();
        if (!timestamps) {
            emit("STILL_GPU_TIMING_UNAVAILABLE requestId=" + std::to_string(rendered.requestId));
        }
        if (timestamps) {
            const auto ms = [&](int a, int b) { return commands.milliseconds(*timestamps, a, b); };
            done.colorProcessingMs = ms(0, 1);
            // 1->3 includes the colour-propagation guide chain before the recovery pass.
            done.highlightReconstructionMs = ms(1, 3);
            // FCC (4-5) runs inside the defringe/tone bracket (16-17).
            done.postTailMs = ms(16, 17) - ms(4, 5);
            done.refinementMs = ms(4, 5);
            done.tonemapMs = ms(6, 7);
            done.denoiseMs = rendered.denoiseStrength > 0.0f ? ms(10, 11) : 0.0;
            // Gate on tap success: dummy 14/15 are written in the
            // main CB, then process() overwrites 14->15 with real
            // stamps. A throw between the two writes would otherwise
            // span a real and a dummy timestamp (wrap to huge).
            done.galoshYuvMs = galoshYuvSuccess ? ms(14, 15) : 0.0;
            done.gainmapMs = rendered.ultraHdrEnabled ? ms(8, 9) : 0.0;
            emit("STILL_GPU_TIMING requestId=" + std::to_string(rendered.requestId) +
                 " wbMs=" + std::to_string(done.colorProcessingMs) + " rtHighlightMs=" +
                 std::to_string(done.highlightReconstructionMs) + " fccMs=" + std::to_string(done.refinementMs) +
                 " tonemapMs=" + std::to_string(done.tonemapMs) + " denoiseMs=" + std::to_string(done.denoiseMs) +
                 " galoshYuvMs=" + std::to_string(done.galoshYuvMs) + " gainmapMs=" + std::to_string(done.gainmapMs) +
                 " gainmapClip=" + std::to_string(gainmapClipUsed ? 1 : 0) +
                 " gainmapBoost=" + std::to_string(gainmapClipBoost) + " colorRender=" +
                 std::string(rawrcam::tonemap_integration::colorRenderProfileName(rendered.colorRenderProfile)) +
                 " postDemosaicMs=" + std::to_string(ms(0, 5)) + " setupMs=" + std::to_string(done.renderSetupMs) +
                 " queueGapsMs=" + std::to_string(done.queueGapsMs));
        }
    }
    // Attribute the fence-wait remainder (scheduling, barriers, copies,
    // queue contention) not covered by the GPU timestamp ranges. When
    // no timing pool exists the ranges read zero and the whole wait
    // lands here, keeping the EXIF parts exact in both cases.
    done.queueGapsMs = std::max(0.0, done.queueGapsMs - (done.colorProcessingMs + done.highlightReconstructionMs +
                                                         done.refinementMs + done.tonemapMs + done.denoiseMs +
                                                         done.galoshYuvMs + done.gainmapMs + done.postTailMs));

    // Keep the canonical TonemapEngine RGBA8 byte order. Step 3 has no
    // Android Bitmap/JPEG consumer yet, so no platform-specific channel swap belongs here.
    if (!rendered.surfacePreview) {
        const auto* bytes = static_cast<const uint8_t*>(resources.outputView().pixels);
        uint64_t checksum = 1469598103934665603ull;  // FNV-1a 64-bit
        for (VkDeviceSize i = 0; i < resources.outputView().bytes; ++i) {
            checksum ^= static_cast<uint64_t>(bytes[i]);
            checksum *= 1099511628211ull;
        }
        done.pixelBytes = static_cast<uint64_t>(resources.outputView().bytes);
        done.checksum = checksum;
    }
    if (rendered.ultraHdrEnabled && resources.outputView().gainmapPixels != nullptr &&
        resources.outputView().gainmapWidth > 0 && resources.outputView().gainmapHeight > 0) {
        // Map content telemetry on the already-mapped readback: max
        // stored texel, fraction at boost level, mean. R channel of
        // RGBA8 (map is gray, R=G=B). Tells per shot whether the
        // boost fired and how much frame it covers. With clipBoost
        // disabled (0 = pure ratio) there is no boost level, so the
        // fraction is defined as 0 instead of counting everything.
        const auto* mapBytes = static_cast<const uint8_t*>(resources.outputView().gainmapPixels);
        const uint64_t n =
            static_cast<uint64_t>(resources.outputView().gainmapWidth) * resources.outputView().gainmapHeight;
        const float boostNorm =
            (std::log2(std::max(rendered.gainmapParams.clipBoost, 1e-6f)) - rendered.gainmapParams.minLog2) /
            std::max(rendered.gainmapParams.maxLog2 - rendered.gainmapParams.minLog2, 1e-6f);
        const bool boostOn = rendered.gainmapParams.clipBoost > 0.0f;
        uint8_t peak = 0;
        uint64_t boosted = 0, sum = 0;
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t v = mapBytes[i * 4];
            peak = std::max<uint8_t>(peak, v);
            sum += v;
            if (boostOn && static_cast<float>(v) / 255.0f >= boostNorm * 0.9f) ++boosted;
        }
        done.gainmapMaxStored = static_cast<float>(peak) / 255.0f;
        done.gainmapBoostedFrac = n == 0 ? 0.0f : static_cast<float>(boosted) / static_cast<float>(n);
        done.gainmapMeanStored = n == 0 ? 0.0f : static_cast<float>(sum) / static_cast<float>(n) / 255.0f;
        emit("STILL_GAINMAP_STATS requestId=" + std::to_string(rendered.requestId) + " maxStored=" +
             std::to_string(done.gainmapMaxStored) + " boostedFrac=" + std::to_string(done.gainmapBoostedFrac) +
             " meanStored=" + std::to_string(done.gainmapMeanStored) + " clipBoost=" +
             std::to_string(rendered.gainmapParams.clipBoost) + " clipView=" + std::to_string(gainmapClipUsed ? 1 : 0));
        __android_log_print(
            ANDROID_LOG_INFO, "RawrCamNative",
            "STILL_GAINMAP_STATS requestId=%llu maxStored=%.3f boostedFrac=%.4f meanStored=%.3f clipView=%d",
            (unsigned long long)rendered.requestId, (double)done.gainmapMaxStored, (double)done.gainmapBoostedFrac,
            (double)done.gainmapMeanStored, gainmapClipUsed ? 1 : 0);
    }
    // Post-fence window: diagnostic dumps (when enabled), readback
    // mapping and the integrity checksum. Completes the EXIF
    // attribution: total == setup + GPU parts + gaps + readback.
    done.readbackMs =
        std::max(0.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tFence).count());
}
}  // namespace rawrcam::develop::rendered
