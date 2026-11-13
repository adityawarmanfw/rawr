#include "capture/multiframe/MultiframeDescription.h"

#include <cmath>
#include <iomanip>
#include <sstream>

#include "encoding/SoftwareName.h"

namespace rawrcam::capture::multiframe {
namespace {

std::string ms1(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

std::string f3(float value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

}  // namespace

std::string buildExifHeader() { return std::string(rawrcam::encoding::kCapturedWith) + "\nMultiframe GPU merge\n"; }

std::string buildParametersBlock(const MultiframeTuningView& tuning, const MultiframeOutputInfo& output,
                                 const TonemapUiValues& tonemap, const std::string& filmDescription, bool filmRendered,
                                 std::uint32_t frameCount, std::uint32_t outWidth, std::uint32_t outHeight) {
    std::ostringstream out;
    out << "\nPARAMETERS\nMultiframe parameters:\n"
        << "- Frames: " << frameCount << '\n';
    {
        std::ostringstream scale;
        scale << std::fixed << std::setprecision(2) << tuning.outputScale;
        const double megapixels = std::round(double(outWidth) * double(outHeight) / 1.0e5) / 10.0;
        std::ostringstream mp;
        mp << std::fixed << std::setprecision(1) << megapixels;
        out << "- Output: " << outWidth << 'x' << outHeight << " (" << scale.str() << "x, ~" << mp.str() << " MP)\n";
    }
    if (tuning.mergeAlgorithm == 4u) {
        out << "- Merge: Rexture (kernel-regression merge of the whole burst; base = best of the last 0.25 s)\n";
    } else {
        out << "- Merge: HDR+ frequency (tile alignment + per-frequency Wiener merge)\n"
            << "- HDR+ strength: " << f3(tuning.hdrplusStrength) << '\n'
            << "- HDR+ tile size: " << tuning.hdrplusTileSize << '\n';
    }
    out << "\nMultiframe Reconstruction inputs:\n"
        << "- Noise profile: " << output.noiseProfile << '\n'
        << "- JPEG input: " << output.jpegInput << '\n';
    out << '\n' << encoding::formatOutputParameters(output) << '\n' << encoding::formatAdaptiveTone(tonemap);
    if (filmRendered && !filmDescription.empty()) {
        // Kotlin-curated film summary (stock/paper names live there); kept
        // verbatim so EXIF matches the app's film vocabulary.
        out << '\n' << filmDescription << '\n';
    } else {
        out << '\n' << encoding::formatToneParameters(tonemap);
    }
    return out.str();
}

std::string buildSharpnessSection(MultiframeBaseFrameMode mode, const std::vector<float>& scores,
                                  std::uint32_t referenceIndex, std::uint32_t frameCount, bool pressSelected) {
    std::ostringstream out;
    out << "\nBase Frame Selection:\n";
    if (pressSelected) {
        out << "- Mode: shutter press (best sharpness and exposure of the last 0.25 s)\n"
            << "- Reference: frame " << (referenceIndex + 1u) << " of " << frameCount << '\n';
        return out.str();
    }
    if (mode != MultiframeBaseFrameMode::Sharpest || scores.size() != frameCount) {
        out << "- Mode: middle\n"
            << "- Reference: chronological middle (frame " << (referenceIndex + 1u) << " of " << frameCount << ")\n";
        if (mode == MultiframeBaseFrameMode::Sharpest) {
            out << "- Note: scoring unavailable, middle fallback\n";
        }
        return out.str();
    }
    float maxScore = 0.0f;
    for (float score : scores) {
        if (std::isfinite(score) && score > maxScore) maxScore = score;
    }
    // Per-frame scores stay in RZSL; the description keeps their mean,
    // relative to the sharpest frame (the base).
    double sum = 0.0;
    for (float score : scores)
        sum += maxScore > 0.0f && std::isfinite(score) ? double(score) / double(maxScore) : 0.0;
    out << "- Mode: sharpest\n"
        << "- Reference: frame " << (referenceIndex + 1u) << " of " << frameCount << " (base)\n"
        << "- Average sharpness: " << f3(float(sum / double(frameCount))) << " (relative to base)\n";
    return out.str();
}

std::string formatMultiframeTimings(const MultiframeStageTimings& stages, MultiframeBaseFrameMode mode) {
    std::ostringstream out;
    out << "\nMultiframe: " << ms1(stages.multiframeTotalMs()) << " ms\n"
        << "- Initialization: " << ms1(stages.initMs) << " ms (" << stages.initNote << ")\n";
    if (mode == MultiframeBaseFrameMode::Sharpest && stages.sharpnessMs > 0.0) {
        out << "- Sharpness: " << ms1(stages.sharpnessMs) << " ms\n";
    } else {
        out << "- Sharpness: skipped (middle)\n";
    }
    // Each subtotal is followed by exactly the stages it sums.
    out << "- Alignment: " << ms1(stages.alignmentTotalMs()) << " ms\n"
        << "  - Reference prepare: " << ms1(stages.refPrepareMs) << " ms\n"
        << "  - Companion prepare: " << ms1(stages.companionPrepareMs) << " ms\n"
        << "  - Block/LK align: " << ms1(stages.alignMs) << " ms\n"
        << "- Merge: " << ms1(stages.mergeTotalMs()) << " ms\n"
        << "  - Noise estimate: " << ms1(stages.noiseMs) << " ms\n"
        << "  - Reference stats: " << ms1(stages.refStatsMs) << " ms\n"
        << "  - Kernel stats: " << ms1(stages.kernelMs) << " ms\n"
        << "  - Robustness: " << ms1(stages.robustnessMs) << " ms\n"
        << "  - Accumulation: " << ms1(stages.accumMs) << " ms\n"
        << "  - Finalize: " << ms1(stages.finalizeMs) << " ms\n";
    return out.str();
}

}  // namespace rawrcam::capture::multiframe
