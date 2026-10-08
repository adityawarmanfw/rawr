#pragma once

#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>

#include <cstdint>
#include <string>
#include <vector>

#include "capture/multiframe/MultiframeBaseFrame.h"

namespace rawrcam::capture::multiframe {

// Shutter-time tuning bank, transported for EXIF reporting.
struct MultiframeTuningView {
    float outputScale = 1.0f;
    std::uint32_t lkIterations = 3;
    float hessianEpsilon = 1.0e-10f;
    float kDetail = 0.08f;
    float kDenoise = 5.0f;
    float dThreshold = 0.25f;
    float dTransition = 0.30f;
    float kStretch = 1.0f;
    float kShrink = 8.0f;
    float flatSigma = 0.5f;
    float detailFloorSigma = 0.0f;
    float scaleBandwidthGain = 1.0f;
    float coverageNeffLo = 1.0f;
    float coverageNeffHi = 3.0f;
    float coverageMassLo = 0.001f;
    float coverageMassHi = 0.03f;
    float robustnessT = 0.09f;
    float robustnessS1 = 1.5f;
    float robustnessS2 = 9.0f;
    float motionThreshold = 0.65f;
    float fallbackChromaGain = 0.0f;
    float fallbackLumaGain = 0.0f;
    std::uint32_t mergeAlgorithm = 0;  // 0 = Wronski, 1 = HDR+ spatial, 2 = HDR+ frequency, 3 = HDR+ bracketed
    float hdrplusStrength = 13.0f;
    std::uint32_t hdrplusTileSize = 32;
    float bracketEv = -2.0f;
    std::uint32_t bracketFrames = 2;
    float bracketLiftEv = 0.0f;  // measured: 0 = no dark frames merged
    MultiframeBaseFrameMode baseFrameMode = MultiframeBaseFrameMode::Sharpest;
};

// General-pipeline (non-multiframe) capture settings for the shared
// PARAMETERS section. Filled in run() from the JPEG context; no native deps.
struct MultiframeOutputInfo {
    std::string noiseProfile = "Default";
    std::string demosaicLabel = "n/a";
    std::string dualLabel = "n/a";
    std::string chromaDenoiseLabel = "off";
    std::uint32_t fccSteps = 1;
    bool lscEnabled = false;
    bool highlightReconstructionEnabled = true;
    int jpegQuality = 98;
    std::string subsampling = "4:2:2";
    std::string colorProfile = "RAWR NTRL";
};

// Tonemap UI values, mapped from tonemap::TonemapParams in run() so this
// module stays free of engine headers.
struct TonemapUiValues {
    float exposureEV = 0.0f;
    float blacks = 0.0f;
    float shadows = 0.0f;
    float midtones = 0.0f;
    float contrast = 0.0f;
    float whites = 0.0f;
    float highlights = 0.0f;
    float saturation = 0.0f;
    float vibrance = 0.0f;
    float aePostGain = 1.0f;
};

// Measured stage times (ms) feeding the TIMINGS sections.
struct MultiframeStageTimings {
    double baseReadMs = 0.0;
    double sharpnessMs = 0.0;
    double initMs = 0.0;
    double noiseMs = 0.0;  // burst noise estimation (merge stage)
    double refPrepareMs = 0.0;
    double refStatsMs = 0.0;
    double companionPrepareMs = 0.0;
    double alignMs = 0.0;
    double kernelMs = 0.0;
    double robustnessMs = 0.0;
    double accumMs = 0.0;
    double finalizeMs = 0.0;
    double projectMs = 0.0;
    double rgbPrepMs = 0.0;
    std::string initNote = "cold";
    [[nodiscard]] double alignmentTotalMs() const noexcept { return refPrepareMs + companionPrepareMs + alignMs; }
    [[nodiscard]] double mergeTotalMs() const noexcept {
        return noiseMs + refStatsMs + kernelMs + robustnessMs + accumMs + finalizeMs;
    }
    [[nodiscard]] double multiframeTotalMs() const noexcept {
        return initMs + sharpnessMs + alignmentTotalMs() + mergeTotalMs();
    }
};

// Pure subtotal for the DNG horizon (unit-tested).
[[nodiscard]] inline double dngSubtotalMs(double baseReadMs, double multiframeTotalMs, double projectMs) noexcept {
    return baseReadMs + multiframeTotalMs + projectMs;
}

// "Captured with Rawr\nMultiframe GPU merge\n" header.
std::string buildExifHeader();

// Header + PARAMETERS (three subgroups) + Tonemap/Film section. Shared by
// merged DNG and JPEG; timings are appended separately per file horizon.
std::string buildParametersBlock(const MultiframeTuningView& tuning, const MultiframeOutputInfo& output,
                                 const TonemapUiValues& tonemap, const std::string& filmDescription, bool filmRendered,
                                 std::uint32_t frameCount, std::uint32_t outWidth, std::uint32_t outHeight);

// Base Frame Selection section. Scores are max-normalized in print; raw
// values stay in RZSL. Empty scores (Middle mode) print the middle reference.
std::string buildSharpnessSection(MultiframeBaseFrameMode mode, const std::vector<float>& scores,
                                  std::uint32_t referenceIndex, std::uint32_t frameCount);

// "Multiframe: T ms" + four stage lines + "Multiframe timings breakdown:"
// eight detail lines. Shared string for DNG and (via context) JPEG.
std::string formatMultiframeTimings(const MultiframeStageTimings& stages, MultiframeBaseFrameMode mode);

// The two output-projection lines (DNG horizon).
std::string formatOutputProjection(double baseReadMs, double projectMs);

}  // namespace rawrcam::capture::multiframe
