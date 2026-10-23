#include "presentation/PresentRecorder.h"

#include <android/log.h>

#include <algorithm>
#include <sstream>

#include "geometry/DisplayToSensorMapper.h"
#include "geometry/OrientationTransform.h"
namespace rawrcam::presentation {
namespace {
struct PresentPush {
    uint32_t quarterTurns;
    float srcAspect;
    float dstAspect;
    uint32_t diagnosticMode;
    uint32_t overlayEnabled;
    float cornerRadius;
    uint32_t exifOrientation;
    uint32_t cropEnabled;
    float cropX0;
    float cropY0;
    float cropX1;
    float cropY1;
    // Lens undistort (see present.frag); zero = off.
    uint32_t lensWarp;
    float lensCx, lensCy, lensFx, lensFy, lensFxOut, lensFyOut;
    std::array<float, 8> k;
    uint32_t lensMirror;
};
}  // namespace
void PresentRecorder::record(const RecordInfo& i) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = i.swapInitialized ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i.swapImage;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(i.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = i.renderPass;
    rp.framebuffer = i.framebuffer;
    rp.renderArea = {{0, 0}, i.swapExtent};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(i.command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(i.command, VK_PIPELINE_BIND_POINT_GRAPHICS, i.pipeline);
    VkViewport full{0, 0, static_cast<float>(i.swapExtent.width), static_cast<float>(i.swapExtent.height), 0, 1};
    VkRect2D fullScissor{{0, 0}, i.swapExtent};
    vkCmdSetViewport(i.command, 0, 1, &full);
    vkCmdSetScissor(i.command, 0, 1, &fullScissor);
    const VkDescriptorSet selected = i.diagnosticMode == 4u ? i.linearSet : i.tonemappedSet;
    vkCmdBindDescriptorSets(i.command, VK_PIPELINE_BIND_POINT_GRAPHICS, i.layout, 0, 1, &selected, 0, nullptr);
    const uint32_t presentationTurns =
        rawrcam::geometry::presentationQuarterTurns(i.sensorOrientationDegrees, i.displayRotationDegrees);
    const int rotation = static_cast<int>(presentationTurns * 90u);
    const bool quarterTurn = i.exifOrientation ? (i.exifOrientation >= 5u && i.exifOrientation <= 8u)
                                                : (presentationTurns & 1u) != 0;
    // Idle video-mode crop: sample the exact record window (centered, shared
    // with the focus/face mapper). Disabled for video-source frames (already
    // cropped), EXIF still review, and diagnostic visualizations.
    const bool cropOn = i.cropW != 0 && i.cropH != 0 && i.exifOrientation == 0u && i.diagnosticMode == 0u;
    rawrcam::geometry::SourceCropRect crop{};
    if (cropOn) {
        crop = {i.cropX, i.cropY, i.cropW, i.cropH, true};
    }
    float cropX0, cropY0, cropX1, cropY1;
    rawrcam::geometry::cropWindowFractions(i.previewWidth, i.previewHeight, crop, quarterTurn, cropX0, cropY0,
                                           cropX1, cropY1);
    const float winW = cropX1 - cropX0;
    const float winH = cropY1 - cropY0;
    const float sw = (quarterTurn ? static_cast<float>(i.previewHeight) : static_cast<float>(i.previewWidth)) * winW;
    const float sh = (quarterTurn ? static_cast<float>(i.previewWidth) : static_cast<float>(i.previewHeight)) * winH;
    if (!mapLogged_) {
        std::ostringstream d;
        d << "PRESENTATION_MAP rotation=" << rotation << " quarterTurns=" << ((rotation / 90) & 3)
          << " texture=" << i.previewWidth << 'x' << i.previewHeight << " displaySource=" << static_cast<uint32_t>(sw)
          << 'x' << static_cast<uint32_t>(sh) << " srcAspect=" << (sw / sh) << " swap=" << i.swapExtent.width << 'x'
          << i.swapExtent.height
          << " dstAspect=" << (static_cast<float>(i.swapExtent.width) / static_cast<float>(i.swapExtent.height))
          << " diagnosticMode=" << i.diagnosticMode << " crop=" << (cropOn ? "on" : "off") << ' '
          << i.cropX << ',' << i.cropY << ' ' << i.cropW << 'x' << i.cropH;
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "%s", d.str().c_str());
        if (diagnostic_) diagnostic_(d.str());
        mapLogged_ = true;
    }
    const PresentPush pc{presentationTurns,
                         sw / sh,
                         static_cast<float>(i.swapExtent.width) / static_cast<float>(i.swapExtent.height),
                         i.diagnosticMode,
                         i.overlayEnabled ? 1u : 0u,
                         0.0f,
                         i.exifOrientation,
                         cropOn ? 1u : 0u,
                         cropX0,
                         cropY0,
                         cropX1,
                         cropY1,
                         0u,
                         0.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         0.0f,
                         {},
                         0u};
    // Live viewfinder only (no EXIF review or diagnostic view). With the idle
    // video crop the warp runs after it, in full-frame UV, like the recorder.
    PresentPush pcWarp = pc;
    if (i.exifOrientation == 0u && i.diagnosticMode == 0u) pcWarp.lensMirror = i.lensMirror;
    if (i.lensWarp && i.exifOrientation == 0u && i.diagnosticMode == 0u) {
        const auto& m = *i.lensWarp;
        // Pixel-index model -> source UV (pixel i covers [i, i + 1) / size).
        pcWarp.lensWarp = 1u;
        pcWarp.lensCx = float((m.cx + 0.5) / m.width);
        pcWarp.lensCy = float((m.cy + 0.5) / m.height);
        pcWarp.lensFx = float(m.fx / m.width);
        pcWarp.lensFy = float(m.fy / m.height);
        pcWarp.lensFxOut = float(m.fx * m.outputScale / m.width);
        pcWarp.lensFyOut = float(m.fy * m.outputScale / m.height);
        for (size_t k = 0; k < 8; ++k) pcWarp.k[k] = float(m.k[k]);
    }
    static_assert(sizeof(PresentPush) <= 128, "push constants beyond the guaranteed minimum");
    vkCmdPushConstants(i.command, i.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcWarp), &pcWarp);
    vkCmdDraw(i.command, 3, 1, 0, 0);
    for (const auto& s : i.scopes) {
        if (!s.enabled || s.descriptor == VK_NULL_HANDLE || s.width <= 0 || s.height <= 0) continue;
        const float px = s.x * static_cast<float>(i.swapExtent.width),
                    py = s.y * static_cast<float>(i.swapExtent.height),
                    pw = s.width * static_cast<float>(i.swapExtent.width),
                    ph = s.height * static_cast<float>(i.swapExtent.height);
        const int32_t sx = std::clamp(static_cast<int32_t>(px), 0, static_cast<int32_t>(i.swapExtent.width) - 1),
                      sy = std::clamp(static_cast<int32_t>(py), 0, static_cast<int32_t>(i.swapExtent.height) - 1);
        const int32_t ex = std::clamp(static_cast<int32_t>(px + pw), sx + 1, static_cast<int32_t>(i.swapExtent.width)),
                      ey = std::clamp(static_cast<int32_t>(py + ph), sy + 1, static_cast<int32_t>(i.swapExtent.height));
        VkViewport vp{static_cast<float>(sx),
                      static_cast<float>(sy),
                      static_cast<float>(ex - sx),
                      static_cast<float>(ey - sy),
                      0,
                      1};
        VkRect2D sc{{sx, sy}, {static_cast<uint32_t>(ex - sx), static_cast<uint32_t>(ey - sy)}};
        vkCmdSetViewport(i.command, 0, 1, &vp);
        vkCmdSetScissor(i.command, 0, 1, &sc);
        vkCmdBindDescriptorSets(i.command, VK_PIPELINE_BIND_POINT_GRAPHICS, i.layout, 0, 1, &s.descriptor, 0, nullptr);
        const PresentPush sp{
            s.quarterTurns, s.sourceAspect, static_cast<float>(ex - sx) / static_cast<float>(ey - sy), 100u, 0u,
            s.cornerRadius,
            0u, 0u, 0.0f, 0.0f, 1.0f, 1.0f, 0u, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {}, 0u};
        vkCmdPushConstants(i.command, i.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sp), &sp);
        vkCmdDraw(i.command, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(i.command);
}
}  // namespace rawrcam::presentation
