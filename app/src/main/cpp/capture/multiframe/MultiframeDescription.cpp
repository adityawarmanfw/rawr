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

std::string sci0(float value) {
    std::ostringstream out;
    out << std::scientific << std::setprecision(0) << value;
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
    if (tuning.mergeAlgorithm != 0u) {
        out << (tuning.mergeAlgorithm == 3u   ? "- Merge: HDR+ bracketed (frequency merge, darkest-frame reference)\n"
                : tuning.mergeAlgorithm == 2u ? "- Merge: HDR+ frequency (tile alignment + per-frequency Wiener merge)\n"
                                              : "- Merge: HDR+ spatial (tile alignment + robust average)\n")
            << "- HDR+ strength: " << f3(tuning.hdrplusStrength) << '\n'
            << "- HDR+ tile size: " << tuning.hdrplusTileSize << '\n';
        if (tuning.mergeAlgorithm == 3u && tuning.bracketCapture == 1u) {
            out << "- Highlight headroom: up to " << f3(tuning.bracketEv) << " EV, lifted "
                << (tuning.bracketLiftEv > 0.0f ? f3(tuning.bracketLiftEv) + " EV" : std::string("none")) << '\n'
                << "- Long frames: " << tuning.bracketFrames << " x "
                << (tuning.longFrameEv > 0.0f ? "+" + f3(tuning.longFrameEv) + " EV" : std::string("Camera2 AE"))
                << '\n';
        } else if (tuning.mergeAlgorithm == 3u) {
            out << "- Bracket: " << tuning.bracketFrames << " x " << f3(tuning.bracketEv) << " EV\n"
                << "- Bracket lift: "
                << (tuning.bracketLiftEv > 0.0f ? f3(tuning.bracketLiftEv) + " EV" : std::string("none (no dark frames)"))
                << '\n';
        }
    } else {
        out << "- Merge: Wronski kernel regression\n"
            << "- LK iterations: " << tuning.lkIterations << '\n'
            << "- Hessian epsilon: " << sci0(tuning.hessianEpsilon) << '\n'
            << "- kDetail: " << f3(tuning.kDetail) << '\n'
            << "- kDenoise: " << f3(tuning.kDenoise) << '\n'
            << "- dThreshold: " << f3(tuning.dThreshold) << '\n'
            << "- dTransition: " << f3(tuning.dTransition) << '\n'
            << "- kStretch: " << f3(tuning.kStretch) << '\n'
            << "- kShrink: " << f3(tuning.kShrink) << '\n'
            << "- Flat sigma: " << f3(tuning.flatSigma) << (tuning.flatSigma < 0.0f ? " (coupled)" : " (decoupled)") << '\n'
            << "- Detail floor sigma: " << f3(tuning.detailFloorSigma) << (tuning.detailFloorSigma <= 0.0f ? " (off)" : "")
            << '\n'
            << "- Scale bandwidth gain: " << f3(tuning.scaleBandwidthGain)
            << (tuning.scaleBandwidthGain <= 0.0f ? " (off)" : "") << '\n'
            << "- Coverage Neff: " << f3(tuning.coverageNeffLo) << " .. " << f3(tuning.coverageNeffHi) << '\n'
            << "- Coverage mass: " << f3(tuning.coverageMassLo) << " .. " << f3(tuning.coverageMassHi) << '\n';
        out << "- Robustness T/S1/S2: " << f3(tuning.robustnessT) << " / " << f3(tuning.robustnessS1) << " / "
            << f3(tuning.robustnessS2) << '\n'
            << "- Motion threshold: " << f3(tuning.motionThreshold) << '\n'
            << "- Fallback cleanup chroma/luma: " << f3(tuning.fallbackChromaGain) << " / " << f3(tuning.fallbackLumaGain)
            << '\n';
    }
    out << "\nMultiframe Reconstruction inputs:\n"
        << "- Noise profile: " << output.noiseProfile << '\n'
        << "- JPEG input: merged CFA\n";
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
                                  std::uint32_t referenceIndex, std::uint32_t frameCount) {
    std::ostringstream out;
    out << "\nBase Frame Selection:\n";
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
