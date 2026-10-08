#pragma once
#include <cstdint>
#include <string>

#include "tonemap/ColorRenderProfile.h"
namespace rawrcam::develop {
enum class DemosaicAlgorithm : std::uint8_t { Rcd = 0, Vng4 = 2, DualRcdVng4 = 3 };
struct DevelopSettings {
    bool pipelineDiagnosticsEnabled = false;
    rawrcam::tonemap_integration::ColorRenderProfile colorRenderProfile =
        rawrcam::tonemap_integration::ColorRenderProfile::RawrBase;
    std::string importedLutProfileId;
    DemosaicAlgorithm demosaicAlgorithm = DemosaicAlgorithm::Rcd;
    bool dualAutoContrast = true;
    float dualContrastPercent = 20.0f;
    std::uint32_t fccSteps = 2;
    // Absolute chroma median + uniform reconstruction. Avoid chroma clipping
    // before the scene-linear highlight/tone stages.
    float fccEdgeSigma = 0.0f;
    float fccChromaBound = 0.0f;
    // Lab chroma repair after highlight reconstruction/FCC; strength 0 bypasses.
    // Legacy edge/luma fields remain for recipe compatibility only.
    float defringeStrength = 1.0f;
    float defringeEdgeThreshold = 0.02f;
    float defringeLumaFloor = 0.08f;
    // Profiled wavelet denoise (linear RGB, pre-WB). 0 disables
    // (legacy bit-identical); raw strength multiplier (validated 1..2, to 8).
    float denoiseStrength = 0.0f;
    // Shadow-preservation fulcrum (higher preserves more texture).
    float denoiseDetail = 1.0f;
    // Luma-band force (Y0 threshold scale, 0..1; shipped tuning 0.25) and
    // wavelet band count (1..7, shipped 7). Clamped at record time.
    float denoiseForceY = 0.25f;
    int denoiseMaxScale = 7;
    // GALOSH-RAW Bayer denoise (pre-demosaic, P1a). 0 = off (default,
    // legacy bit-identical); 1 = full luma+chroma; 2 = chroma-only
    // (luma lane bypassed, detail preserved). Mirrors
    // galosh::GaloshRawMode; mapped from JNI/Settings in P1c.
    int galoshRawMode = 0;
    float galoshStrength = 1.0f;
    float galoshLuma = 1.0f;
    float galoshChroma = 1.0f;
    // GALOSH-YUV denoise (post-tonemap linear RGB, P1b). 0 = off
    // (default, legacy bit-identical); 1 = full; 2 = chroma-only.
    // Mirrors galosh::GaloshYuvMode; mapped from JNI/Settings in P1c.
    int galoshYuvMode = 0;
    // Multiframe only: chroma-only profiled wavelet on the merge, noise from
    // the burst fit / frame count (independent of the single-frame denoise).
    bool multiframeChromaDenoise = false;
    float galoshYuvStrengthY = 1.0f;
    float galoshYuvStrengthC = 1.0f;
    bool lensShadingCorrectionEnabled = false;
    bool highlightReconstructionEnabled = true;
    std::uint32_t highlightReconstructionMethod = 0;  // 0 propagation, 1 RawTherapee Coloropp
    float highlightThreshold = 1.0f;
    float highlightCompression = 100.0f;
    // Geometric undistort in the still WB pass (Camera2 LENS_DISTORTION model).
    // Default off; gated on full-pixel-array geometry + calibration upstream.
    bool distortionCorrectionEnabled = false;
};
}  // namespace rawrcam::develop
