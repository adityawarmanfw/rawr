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
    // the most highlight headroom the ZSL burst may be underexposed by
    // (legacy captures: the post-shutter dark frames' EV), and how many
    // post-shutter long frames to capture.
    float bracketEv = -2.0f;
    std::uint32_t bracketFrames = 2;
    // Appended (journal prefix-compatible), mergeAlgorithm 3 only. Long-frame
    // exposure over the ZSL reference in EV; 0 = the capture's headroom, i.e.
    // back to Camera2 AE (always at least +1 EV).
    float longFrameEv = 0.0f;
    // Set at the shutter, not from settings. 1 = underexposed ZSL burst plus
    // post-shutter long frames; 0 = older journals' dark post-shutter frames.
    std::uint32_t bracketCapture = 0;
    // Camera2 AE exposure the ZSL burst was underexposed from (exposure time
    // ns x ISO of the prepared reference, times 2^-headroom); 0 = unknown.
    // The merged DNGs lift their reference back to it.
    float aeLinearExposure = 0.0f;
    // Part of that lift the frozen adaptive curve carries (bracketToneSplit);
    // the frozen AE post-gain carries the rest. Recorded in the DNG recipe so
    // editor shadow-lift overrides keep it.
    float bracketCurveEv = 0.0f;
};

}  // namespace rawrcam::capture::multiframe
