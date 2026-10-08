#pragma once

#include <cstdint>

namespace rawrcam::video {

// Per-mode image intent. Kotlin supplies values; native owns all frame work.
// Resource-sized controls (FCC passes and defringe pipeline) are latched when
// recording starts. Other fields may change at a frame boundary.
struct VideoProcessingConfig {
    bool lensShadingEnabled = false;
    bool highlightEnabled = true;
    uint32_t highlightMethod = 0;
    float highlightThreshold = 1.0f;
    float highlightCompression = 100.0f;
    uint32_t fccSteps = 0;  // 0 bypasses FCC; enabled video FCC is one step.
    float defringeStrength = 0.0f;
    float waveletDenoiseStrength = 0.0f;
    float waveletDenoiseDetail = 1.0f;
    // Luma-band force (Y0 threshold scale, 0..1; shipped tuning 0.25) and
    // wavelet band count (1..7, shipped 7). Clamped at record time.
    float waveletDenoiseForceY = 0.25f;
    int waveletDenoiseScales = 7;
};

}  // namespace rawrcam::video
