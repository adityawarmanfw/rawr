#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace rawrcam::encoding::jpeg {

// Unified TIMINGS formatting shared by every JPEG writer path. Pure
// functions, unit-tested.
// All stage times in ms. Totals must equal the sum of the printed lines;
// file write + fsync complete after EXIF is assembled and are logcat-only.

inline std::string formatMs1(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

[[nodiscard]] inline double imageProcessingTotalMs(double baseReadMs, double projectMs, double demosaicOrPrepMs,
                                                   double setupMs, double hlMs, double fccMs, double colorMs,
                                                   double toneMs, double gapsMs, double readbackMs,
                                                   double gainmapMs = 0.0, double denoiseMs = 0.0,
                                                   double postTailMs = 0.0) noexcept {
    return baseReadMs + projectMs + demosaicOrPrepMs + setupMs + hlMs + fccMs + colorMs + toneMs + gainmapMs +
           denoiseMs + postTailMs + gapsMs + readbackMs;
}

// Label for the defringe / Inpaint Opposed tone line, empty when neither ran.
inline const char* postTailLabel(int highlightStage, bool defringeEnabled) {
    if (defringeEnabled && highlightStage == 2) return "Defringe + highlight tone";
    if (defringeEnabled) return "Defringe";
    if (highlightStage == 2) return "Highlight tone (Inpaint Opposed)";
    return "";
}

[[nodiscard]] inline double overallTotalMs(double multiframeTotalMs, double imageProcessingTotalMs,
                                           double encodeMs) noexcept {
    return multiframeTotalMs + imageProcessingTotalMs + encodeMs;
}

// Multiframe only: base readback and CFA projection, its own section in both
// the merged DNG and the JPEG.
inline std::string formatOutputProjection(double baseReadMs, double projectMs) {
    return "\nOutput projection: " + formatMs1(baseReadMs + projectMs) + " ms\n- Base readback: " +
           formatMs1(baseReadMs) + " ms\n- CFA projection: " + formatMs1(projectMs) + " ms\n";
}

inline std::string formatImageProcessing(bool directRgb, double demosaicMs,
                                         double rgbPrepMs, double setupMs, double hlMs, bool lscOn,
                                         std::uint32_t fccSteps, double fccMs, double colorMs, double toneMs,
                                         bool filmRendered, double gapsMs, double readbackMs,
                                         bool multiframe = true, double gainmapMs = 0.0, double denoiseMs = 0.0,
                                         int highlightStage = -1, double postTailMs = 0.0,
                                         bool defringeEnabled = false) {
    // highlightStage -1 keeps the legacy lines. Otherwise: 0 off, 1 colour
    // propagation, 2 Inpaint Opposed, whose reconstruction runs inside the
    // colour-processing pass; defringe and the Inpaint Opposed tone tap get
    // their own line.
    const bool described = highlightStage >= 0;
    const char* tailLabel = described ? postTailLabel(highlightStage, defringeEnabled) : "";
    const double postTail = *tailLabel ? std::max(postTailMs, 0.0) : 0.0;
    const double demosaicOrPrep = directRgb ? rgbPrepMs : demosaicMs;
    // UltraHDR only: the GPU gain map dispatch is an image-processing stage.
    // Zero keeps legacy output byte-identical (no line printed, total w/o it).
    const double gainmap = gainmapMs > 0.0 ? gainmapMs : 0.0;
    // Profiled denoise only: same zero-keeps-legacy contract as gainmap.
    const double denoise = denoiseMs > 0.0 ? denoiseMs : 0.0;
    std::ostringstream out;
    out << "\nImage Processing: "
        << formatMs1(imageProcessingTotalMs(0.0, 0.0, demosaicOrPrep, setupMs, hlMs, fccMs, colorMs, toneMs,
                                            gapsMs, readbackMs, gainmap, denoise, postTail))
        << " ms\n";
    // Stages in execution order: demosaic, render setup, then the
    // post-demosaic passes as recorded (wavelet denoise runs pre-WB; FCC
    // follows defringe), tone or film, gain map, and the readback.
    if (directRgb) {
        out << "- Demosaic: n/a (merged_rgb input)\n"
            << "- Merged RGB prep: " << formatMs1(rgbPrepMs) << " ms\n";
    } else {
        out << "- Demosaic: " << formatMs1(demosaicMs) << " ms\n";
        // Merged-RGB prep only exists on the multiframe path.
        if (multiframe) out << "- Merged RGB prep: n/a (remosaiced input)\n";
    }
    out << "- Vignette correction (LSC): " << (lscOn ? "on (time in Demosaic)" : "off") << '\n'
        << "- Render setup: " << formatMs1(setupMs) << " ms\n";
    if (denoise > 0.0) out << "- Denoise (wavelet): " << formatMs1(denoise) << " ms\n";
    out << "- Color processing" << (described && highlightStage == 2 ? " (WB + Inpaint Opposed)" : "") << ": "
        << formatMs1(colorMs) << " ms\n";
    if (!described) {
        out << "- HL reconstruction: " << formatMs1(hlMs) << " ms\n";
    } else if (highlightStage == 0) {
        // Off still clamps to the common channel ceiling inside colour processing.
        out << "- HL reconstruction: off\n";
    } else if (highlightStage == 1) {
        out << "- HL reconstruction (color propagation): " << formatMs1(hlMs) << " ms\n";
    } else {
        out << "- HL reconstruction (Inpaint Opposed): in Color processing\n";
    }
    if (*tailLabel) out << "- " << tailLabel << ": " << formatMs1(postTail) << " ms\n";
    out << "- FCC (chroma cleanup, " << fccSteps << (fccSteps == 1u ? " step" : " steps") << "): " << formatMs1(fccMs)
        << " ms\n";
    out << "- " << (filmRendered ? "Film record" : "Tonemap") << ": " << formatMs1(toneMs) << " ms\n";
    if (gainmap > 0.0) out << "- Gain map (GPU): " << formatMs1(gainmap) << " ms\n";
    out << "- Submit/fence gaps: " << formatMs1(gapsMs) << " ms\n"
        << "- Readback: " << formatMs1(readbackMs) << " ms\n";
    return out.str();
}

// JPEG encoding runs last and belongs to no subsection: it is printed as a
// top-level line after Image Processing so every section header equals the
// sum of exactly the lines beneath it.
inline std::string formatJpegEncoding(double encodeMs) {
    return std::string("\nJPEG encoding: ") + formatMs1(encodeMs) + " ms\n";
}

}  // namespace rawrcam::encoding::jpeg
