#pragma once

#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>

#include <cstdint>
#include <string>
#include <vector>

#include "capture/multiframe/MultiframeBaseFrame.h"
#include "encoding/ProcessingParameters.h"
#include "encoding/jpeg/JpegTimingsFormat.h"

namespace rawrcam::capture::multiframe {

// Shutter-time tuning bank, transported for EXIF reporting.
struct MultiframeTuningView {
    float outputScale = 1.0f;
    std::uint32_t mergeAlgorithm = 4;  // 2 = HDR+ Quality (frequency merge), 4 = Rexture
    float hdrplusStrength = 13.0f;
    std::uint32_t hdrplusTileSize = 32;
    MultiframeBaseFrameMode baseFrameMode = MultiframeBaseFrameMode::Sharpest;
};

// General-pipeline (non-multiframe) capture settings for the shared
// PARAMETERS section, plus the multiframe noise-profile input. Filled in
// run() from the JPEG context; no native deps.
struct MultiframeOutputInfo : encoding::OutputParameters {
    MultiframeOutputInfo() { chromaDenoiseLabel = "off"; }
    std::string noiseProfile = "Default";
    std::string jpegInput = "merged CFA";
};

// Tonemap UI values, mapped from tonemap::TonemapParams in run() so this
// module stays free of engine headers.
using TonemapUiValues = encoding::ToneParameters;

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
    // Press-time base selection (Rexture): scoring time and how many frames were scored. 0 = not run.
    double baseSelectMs = 0.0;
    std::uint32_t baseSelectFrames = 0;
    // Wall time of the merge run against its GPU-timestamped stages: the stage times below are GPU intervals, the merge
    // also waits on the queue and the GPU idles between its submissions. 0 = not measured.
    double mergeWallMs = 0.0, mergeGpuMs = 0.0, mergeWaitMs = 0.0;
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

// "Captured with Rawr v<version>\nMultiframe GPU merge\n" header.
std::string buildExifHeader();

// Header + PARAMETERS (three subgroups) + Tonemap/Film section. Shared by
// merged DNG and JPEG; timings are appended separately per file horizon.
std::string buildParametersBlock(const MultiframeTuningView& tuning, const MultiframeOutputInfo& output,
                                 const TonemapUiValues& tonemap, const std::string& filmDescription, bool filmRendered,
                                 std::uint32_t frameCount, std::uint32_t outWidth, std::uint32_t outHeight);

// Base Frame Selection section: the base frame and the mean sharpness
// relative to it (per-frame raw scores stay in RZSL). Empty scores (Middle
// mode) print the middle reference. pressSelected: the base was chosen at the
// shutter press (Rexture), so the per-burst sharpness list does not apply.
std::string buildSharpnessSection(MultiframeBaseFrameMode mode, const std::vector<float>& scores,
                                  std::uint32_t referenceIndex, std::uint32_t frameCount, bool pressSelected = false);

// "Multiframe: T ms" + four stage lines + "Multiframe timings breakdown:"
// eight detail lines. Shared string for DNG and (via context) JPEG.
std::string formatMultiframeTimings(const MultiframeStageTimings& stages, MultiframeBaseFrameMode mode);

// The output-projection section lives in encoding/jpeg/JpegTimingsFormat.h
// (shared with the JPEG TIMINGS).
using encoding::jpeg::formatOutputProjection;

}  // namespace rawrcam::capture::multiframe
