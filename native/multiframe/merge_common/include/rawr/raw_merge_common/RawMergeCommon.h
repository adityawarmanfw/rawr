#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
// Noise model and sensor facts shared by the multiframe merges (Rexture, HDR+ Quality).
namespace rawr::raw_merge_common {
enum class CfaPattern : std::uint32_t { RGGB = 0, GRBG = 1, GBRG = 2, BGGR = 3 };
// Camera2 SENSOR_NOISE_PROFILE pairs in row-major 2x2 CFA-site order.
// Variance in normalized RAW space is slope * signal + offset.
struct CfaNoiseProfile {
    std::array<float, 4> slopeBySite{};
    std::array<float, 4> offsetBySite{};
};
struct Config {
    CfaPattern cfa = CfaPattern::RGGB;
    float noiseAlpha = 1.80710882e-4f, noiseBeta = 3.1937599182128e-6f;
    std::optional<CfaNoiseProfile> sensorNoiseProfile{};
    std::optional<CfaNoiseProfile> highSignalNoiseProfile{};
    std::array<float, 4> noiseKneeBySite{};  // 0 selects sensorNoiseProfile over the full range.
    // Replace the configured noise profile, per run, with one fitted from the
    // burst itself (reference vs neighbouring frame, flat static blocks).
    // Falls back to the configured profile when the fit is not usable.
    bool estimateNoiseFromBurst = false;
    // Sensor-reported hot/defective pixels, flat [x0, y0, x1, y1, ...] in RAW
    // pixels (Camera2 STATISTICS_HOT_PIXEL_MAP). Concealed in every frame
    // right after normalization; empty disables the pass.
    std::vector<std::int32_t> hotPixels{};
};
inline bool valid(const CfaNoiseProfile& n) {
    for (std::size_t i = 0; i < 4; ++i)
        if (!(std::isfinite(n.slopeBySite[i]) && n.slopeBySite[i] > 0.0f && std::isfinite(n.offsetBySite[i]) &&
              n.offsetBySite[i] >= 0.0f))
            return false;
    return true;
}
inline bool valid(const Config& c) {
    if (!(c.noiseAlpha > 0 && (!c.sensorNoiseProfile || valid(*c.sensorNoiseProfile)) &&
          (!c.highSignalNoiseProfile || c.sensorNoiseProfile) &&
          (!c.highSignalNoiseProfile || valid(*c.highSignalNoiseProfile))))
        return false;
    for (float knee : c.noiseKneeBySite)
        if (!(std::isfinite(knee) && knee >= 0.0f && knee < 1.0f)) return false;
    return true;
}
}  // namespace rawr::raw_merge_common
