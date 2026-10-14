#pragma once

#include <algorithm>
#include <sstream>
#include <string>

#include "encoding/jpeg/JpegCaptureContext.h"
#include "encoding/jpeg/JpegTimingsFormat.h"

namespace rawrcam::encoding::jpeg {

// Sub-items of Demosaic / Render setup (already counted there): how much of
// each was engine building (pipeline compile + allocation) vs GPU work.
inline std::string setupBreakdown(const JpegCaptureContext& c) {
    if (!(c.demosaicSetupMs > 0.0) && !(c.renderEngineBuildMs > 0.0)) return {};
    // Only measured parts: the Renderer's demosaic has no separate engine build.
    std::string parts;
    if (c.demosaicSetupMs > 0.0) parts += "demosaic " + formatMs1(c.demosaicSetupMs) + " ms";
    if (c.renderEngineBuildMs > 0.0) parts += (parts.empty() ? "" : ", ") + std::string("render ") +
                                               formatMs1(c.renderEngineBuildMs) + " ms";
    return "- Engine build (in stages above): " + parts + "\n";
}

// The settings description plus its TIMINGS section. One layout for every
// mode: multiframe adds its Multiframe block and the base readback / CFA
// projection lines, which single-frame and Renderer outputs do not have.
// TOTAL is the exact sum of the printed stages; UltraHDR mux, file write and
// fsync complete after EXIF assembly and stay in the device log.
inline std::string appendProcessingTimes(const JpegCaptureContext& c, double jpegEncodingMs,
                                         double gainmapEncodeMs = 0.0, bool ultraHdr = false) {
    std::ostringstream out;
    out << c.imageDescription;
    // One blank line before TIMINGS whether or not the settings block ends its last line.
    if (!c.imageDescription.empty() && c.imageDescription.back() != '\n') out << '\n';
    const bool multiframe = c.hasMultiframeTimings;
    const double baseReadMs = multiframe ? c.mfBaseReadMs : 0.0;
    const double projectMs = multiframe ? c.mfProjectMs : 0.0;
    const bool directRgb = multiframe && c.mfDirectRgb;
    // Same rule as formatImageProcessing: the tail counts only when its line prints.
    const double tailMs = c.highlightStage >= 0 && *postTailLabel(c.highlightStage, c.defringeEnabled)
                              ? std::max(c.postTailMs, 0.0)
                              : 0.0;
    const double imageTotal = imageProcessingTotalMs(
        0.0, 0.0, directRgb ? c.mfRgbPrepMs : c.demosaicMs, c.renderSetupMs, c.highlightReconstructionMs,
        c.refinementMs, c.colorProcessingMs, c.tonemapMs, c.queueGapsMs, c.readbackMs, c.gainmapMs, c.denoiseMs,
        c.galoshYuvMs, tailMs);
    const double total =
        overallTotalMs((multiframe ? c.multiframeTotalMs : 0.0) + baseReadMs + projectMs, imageTotal, jpegEncodingMs);
    out << "\nTIMINGS: " << formatMs1(total)
        << (ultraHdr ? " ms (sum of stages below; mux + final sync in device log)\n"
                     : " ms (sum of stages below; final sync in device log)\n")
        << (multiframe ? c.multiframeTimingsBlock + formatOutputProjection(baseReadMs, projectMs) : std::string())
        << formatImageProcessing(directRgb, c.demosaicMs, c.mfRgbPrepMs, c.renderSetupMs,
                                 c.highlightReconstructionMs, c.appliedLensShadingCorrection, c.appliedFccSteps,
                                 c.refinementMs, c.colorProcessingMs, c.tonemapMs, c.filmRendered, c.queueGapsMs,
                                 c.readbackMs, multiframe, c.gainmapMs, c.denoiseMs, c.galoshYuvMs, c.highlightStage,
                                 c.postTailMs, c.defringeEnabled)
        << setupBreakdown(c) << formatJpegEncoding(jpegEncodingMs);
    if (ultraHdr)
        out << "- UltraHDR gain map JPEG: " << formatMs1(gainmapEncodeMs) << " ms (included in JPEG encoding)\n";
    return out.str();
}

}  // namespace rawrcam::encoding::jpeg
