#include "capture/multiframe/MultiframeDescription.h"

#include <cmath>
#include <iomanip>
#include <sstream>

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

std::string ev2(float value) {
    std::ostringstream out;
    out << std::showpos << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::string ui1(float value) {
    std::ostringstream out;
    out << std::showpos << std::fixed << std::setprecision(1) << value;
    return out.str();
}

std::string sci0(float value) {
    std::ostringstream out;
    out << std::scientific << std::setprecision(0) << value;
    return out.str();
}

}  // namespace

std::string buildExifHeader() { return "Captured with Rawr\nMultiframe GPU merge\n"; }

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
        out << (tuning.mergeAlgorithm == 2u ? "- Merge: HDR+ frequency (tile alignment + per-frequency Wiener merge)\n"
                                            : "- Merge: HDR+ spatial (tile alignment + robust average)\n")
            << "- HDR+ strength: " << f3(tuning.hdrplusStrength) << '\n'
            << "- HDR+ tile size: " << tuning.hdrplusTileSize << '\n';
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
        << "- JPEG input: merged CFA\n"
        << "\nJPEG output parameters:\n"
        << "- Demosaic: " << output.demosaicLabel << '\n'
        << "- Dual auto contrast: " << output.dualLabel << '\n'
        << "- Chroma denoise: " << output.chromaDenoiseLabel << '\n'
        << "- FCC steps: " << output.fccSteps << '\n'
        << "- Vignette correction (LSC): " << (output.lscEnabled ? "on" : "off") << '\n'
        << "- Highlight reconstruction: " << (output.highlightReconstructionEnabled ? "on" : "off") << '\n'
        << "- JPEG quality: " << output.jpegQuality << ", " << output.subsampling << '\n'
        << "- Color profile: " << output.colorProfile << '\n';
    if (filmRendered && !filmDescription.empty()) {
        // Kotlin-curated film summary (stock/paper names live there); kept
        // verbatim so EXIF matches the app's film vocabulary.
        out << '\n' << filmDescription << '\n';
    } else {
        out << "\nTonemap parameters:\n"
            << "- Exposure: " << ev2(tonemap.exposureEV) << " EV\n"
            << "- Blacks: " << ui1(tonemap.blacks) << '\n'
            << "- Shadows: " << ui1(tonemap.shadows) << '\n'
            << "- Midtones: " << ui1(tonemap.midtones) << '\n'
            << "- Contrast: " << ui1(tonemap.contrast) << '\n'
            << "- Whites: " << ui1(tonemap.whites) << '\n'
            << "- Highlights: " << ui1(tonemap.highlights) << '\n'
            << "- Saturation: " << ui1(tonemap.saturation) << '\n'
            << "- Vibrance: " << ui1(tonemap.vibrance) << '\n';
        {
            std::ostringstream gain;
            gain << std::fixed << std::setprecision(2) << tonemap.aePostGain;
            out << "- AE post-gain: " << gain.str() << "x\n";
        }
        out << "- Shoulder anchor (internal): 2.0 EV\n";
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
    out << "- Mode: sharpest\n"
        << "- Reference: frame " << (referenceIndex + 1u) << " of " << frameCount << '\n';
    for (std::uint32_t i = 0; i < frameCount; ++i) {
        const float normalized = maxScore > 0.0f && std::isfinite(scores[i]) ? scores[i] / maxScore : 0.0f;
        out << "- F" << (i + 1u) << ": " << f3(normalized);
        if (i == referenceIndex) out << " (base)";
        out << '\n';
    }
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

std::string formatOutputProjection(double baseReadMs, double projectMs) {
    std::ostringstream out;
    out << "- Base readback: " << ms1(baseReadMs) << " ms\n"
        << "- CFA projection: " << ms1(projectMs) << " ms\n";
    return out.str();
}

}  // namespace rawrcam::capture::multiframe
