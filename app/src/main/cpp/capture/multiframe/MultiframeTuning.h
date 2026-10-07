#pragma once

#include <cstdint>

namespace rawrcam::capture::multiframe {

// Immutable shutter-time settings. The app only transports these values;
// raw_gpu_pipeline validates them and owns their algorithmic interpretation.
struct MultiframeTuning {
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
    // Support-aware cleanup of fallback (motion/rejected) areas at finalize.
    float fallbackChromaGain = 0.0f;
    float fallbackLumaGain = 0.0f;
    // Appended (journal prefix-compatible): 0 = Wronski super-resolution
    // merge, 1 = HDR+ spatial, 2 = HDR+ frequency, 3 = HDR+ bracketed
    // (frequency merge plus post-shutter dark frames) (HDR+: 1x only; uses
    // only the hdrplus* and bracket* fields).
    std::uint32_t mergeAlgorithm = 0;
    float hdrplusStrength = 13.0f;
    std::uint32_t hdrplusTileSize = 32;  // 16 tracks small motion better, 32 steadier in heavy noise
    // Appended (journal prefix-compatible), used by mergeAlgorithm 3 only:
    // exposure of the post-shutter dark frames relative to the burst, and
    // how many of them to capture.
    float bracketEv = -2.0f;
    std::uint32_t bracketFrames = 2;
};

}  // namespace rawrcam::capture::multiframe
