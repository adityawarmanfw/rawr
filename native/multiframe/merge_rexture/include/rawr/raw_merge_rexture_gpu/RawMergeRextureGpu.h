#pragma once
#include <array>
#include <cmath>
#include <cstdint>

// Rexture: kernel-regression Bayer burst merge for equal-exposure bursts at output scale 1. Every algorithm constant is
// compiled into the shaders; the only user parameter (maximum number of frames) is applied by the caller when it
// selects the burst. What is configurable here is what the sensor and the frame metadata provide.
namespace rawr::raw_merge_rexture_gpu {

enum class CfaPattern : std::uint32_t { RGGB = 0, GRBG = 1, GBRG = 2, BGGR = 3 };

struct Config {
    CfaPattern cfa = CfaPattern::RGGB;
    // Scalar noise model in normalised units: variance = noiseAlpha * x + noiseBeta (x = (raw - black) / (white - black)).
    float noiseAlpha = 1.80710882e-4f, noiseBeta = 3.1937599182128e-6f;
    // White balance gains (R, G, B) used by the low-confidence fallback demosaic only; the output is not balanced.
    std::array<float, 3> whiteBalance{{1.0f, 1.0f, 1.0f}};
    // Frames in the burst (base included). Only the grain pass reads it: with 8 or more frames the areas it still evens out
    // are ones the robustness test rejected frames from (motion), where it keeps more of the texture than with few frames.
    float burstFrames = 1.0f;
    // Detail fit, "Rexturation" in the app (rx_fit.comp; off by default): one forward-model step on green against every frame's raw samples (sums kept while the
    // companions accumulate, no second visit), its detail added as grey after the grain pass. Restores the fine detail the
    // merge's kernel and the pixel aperture soften. Two full-resolution RG16F images.
    bool detailRestore = false;
    // Moving-texture restore (rx_moving.comp; on by default): where most covering companions were rejected for motion over a
    // broad area (a sea, waves), the merge blended a few misplaced copies of the texture (soft, painted, glints dimmed); the base
    // frame's own detail finer than ~0.05 c/px is put back there. Static areas and small or patchy movers are left as merged.
    bool movingRestore = true;
};

[[nodiscard]] inline bool valid(const Config& c) noexcept {
    return static_cast<std::uint32_t>(c.cfa) <= 3u && std::isfinite(c.noiseAlpha) && std::isfinite(c.noiseBeta) &&
           c.noiseAlpha > 0.0f && c.noiseBeta >= 0.0f && std::isfinite(c.whiteBalance[0]) &&
           std::isfinite(c.whiteBalance[1]) && std::isfinite(c.whiteBalance[2]) && c.whiteBalance[0] > 0.0f &&
           c.whiteBalance[1] > 0.0f && c.whiteBalance[2] > 0.0f;
}

}  // namespace rawr::raw_merge_rexture_gpu
