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
};

[[nodiscard]] inline bool valid(const Config& c) noexcept {
    return static_cast<std::uint32_t>(c.cfa) <= 3u && std::isfinite(c.noiseAlpha) && std::isfinite(c.noiseBeta) &&
           c.noiseAlpha > 0.0f && c.noiseBeta >= 0.0f && std::isfinite(c.whiteBalance[0]) &&
           std::isfinite(c.whiteBalance[1]) && std::isfinite(c.whiteBalance[2]) && c.whiteBalance[0] > 0.0f &&
           c.whiteBalance[1] > 0.0f && c.whiteBalance[2] > 0.0f;
}

}  // namespace rawr::raw_merge_rexture_gpu
