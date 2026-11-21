#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include <array>

#include "metadata/FrameMetadataSnapshot.h"
#include "rawr/raw_ingress/BayerBinNoise.h"
#include "rawr/raw_ingress/BayerBinSize.h"
namespace rawrcam::capture {
// Reduced photo output (LensRoute::photoReduced) on a full-size photo context: a 4x4 block mosaic
// (Hexadeca sums + quad remosaic, optionally resampled to 25 MP) or plain Bayer (linear 2x bin, or the
// direct 3x bin for the smaller size: 22 MP from 200 MP).
inline bool reducedRawRequested(const metadata::CameraContextMetadata& c) {
    return c.photoReduced && (c.cfaBlock == 4 || c.cfaBlock == 1) && bool(c.binnedFallbackContext);
}
struct ReducedRawSize {
    uint32_t width = 0, height = 0;
};
// Bayer frames for the smaller size go through the direct 3x bin; 4x4 frames are resampled after the
// remosaic.
[[nodiscard]] inline bool reducedRawDirect3x(const metadata::CameraContextMetadata& c) {
    return c.photoReduced25 && c.cfaBlock == 1;
}
// Stored size of a reduced frame: half per side (50 MP from 200 MP); for the smaller size a third per
// side for Bayer (22 MP), that half divided by sqrt(2) per side for 4x4 (25 MP); rounded down to a
// multiple of 8.
[[nodiscard]] inline ReducedRawSize reducedRawSize(const metadata::CameraContextMetadata& c, uint32_t width,
                                                   uint32_t height) {
    if (reducedRawDirect3x(c)) {
        const auto s = rawr::raw_ingress::bayerBinOutputSize(width, height, 3);
        return {s.width, s.height};
    }
    const uint32_t w = width / 2, h = height / 2;
    if (!c.photoReduced25) return {w, h};
    return {uint32_t(w / std::sqrt(2.0)) & ~7u, uint32_t(h / std::sqrt(2.0)) & ~7u};
}
// Camera-neutral white-balance gains (R, G, B; green 1) of a frame: from its neutral colour point,
// else its colour-correction gains, else none.
[[nodiscard]] inline std::array<float, 3> reducedRawWhiteBalance(const metadata::FrameMetadataSnapshot& m) {
    const auto& n = m.neutralColorPoint;
    if (m.hasNeutralColorPoint && n[0] > 0 && n[1] > 0 && n[2] > 0) return {n[1] / n[0], 1.0f, n[1] / n[2]};
    const auto& g = m.colorCorrectionGainsRggb;
    if (g[0] > 0 && g[1] > 0 && g[3] > 0) return {g[0] / g[1], 1.0f, g[3] / g[1]};
    return {1.0f, 1.0f, 1.0f};
}
// Noise variance of a reduced frame's CFA site (buffer phase 0..3) relative to the exact sum of four
// samples the level scaling assumes (see applyReducedRawMetadata). Bayer: the linear bin's flat-area
// kernel, which mixes colours through the white balance (green keeps about 0.8 of a four-sample sum,
// red and blue 0.2-0.5). Hexadeca: exact sums. The 25 MP resample keeps about 0.40 of its input.
[[nodiscard]] inline double reducedRawNoiseScale(const metadata::CameraContextMetadata& c,
                                                 const std::array<float, 3>& whiteBalance, uint32_t phase) {
    static constexpr uint8_t kColours[4][4] = {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}};
    if (c.cfaBlock == 1)
        return rawr::raw_ingress::bayerBinNoiseVarianceGain(kColours[std::min<uint32_t>(c.rawPreviewCfa, 3)][phase & 3u],
                                                            whiteBalance, reducedRawDirect3x(c) ? 3 : 2) / 0.25;
    return c.photoReduced25 ? 0.40 : 1.0;  // 4x4: the 25 MP resample's reduction
}
// Largest source code the reducer takes: its 2x2 sums (x4) live in R16 images; the phase fit's
// 16-sample block sums do too, which limits it to 12-bit data.
constexpr float kReducedRawMaxCode = 16383.0f;
constexpr float kReducedRawMaxFitCode = 4095.0f;
[[nodiscard]] inline bool reducedRawFitFits(float whiteLevel) {
    return std::isfinite(whiteLevel) && whiteLevel > 0.0f && whiteLevel <= kReducedRawMaxFitCode;
}
// Why a lens that asks for reduced output cannot get it (nullptr: it can). A white level of 0 is
// unknown (dynamic levels): each frame is checked against its own level later.
[[nodiscard]] inline const char* reducedRawUnsupportedReason(const metadata::CameraContextMetadata& c, uint32_t width,
                                                             uint32_t height) {
    if (!reducedRawRequested(c)) return "not_requested";
    if (!width || !height || width % 8 || height % 8) return "geometry";
    if (c.baselineWhiteLevel < 0.0f || !std::isfinite(c.baselineWhiteLevel) || c.baselineWhiteLevel > kReducedRawMaxCode)
        return "white_level";
    return nullptr;
}
// Sensor-coordinate metadata stays in the advertised Camera2 coordinate system.
// Only stored-buffer dimensions change; existing geometry mapping handles the scale.
inline void applyReducedRawMetadata(metadata::FrameMetadataSnapshot& m) {
    if (!m.cameraContext || !reducedRawRequested(*m.cameraContext))
        throw std::runtime_error("reduced_raw_invalid_context");
    auto c = std::make_shared<metadata::CameraContextMetadata>(*m.cameraContext);
    if (c->geometry.rawBufferWidth % 8 || c->geometry.rawBufferHeight % 8)
        throw std::runtime_error("reduced_raw_invalid_geometry");
    const auto size = reducedRawSize(*c, c->geometry.rawBufferWidth, c->geometry.rawBufferHeight);
    const auto whiteBalance = reducedRawWhiteBalance(m);
    std::array<double, 4> noise{};
    for (uint32_t p = 0; p < 4; ++p) noise[p] = reducedRawNoiseScale(*c, whiteBalance, p);
    c->geometry.rawBufferWidth = size.width;
    c->geometry.rawBufferHeight = size.height;
    c->cfaBlock = 1;
    c->photoCfaBlock = 1;
    c->photoReduced = false;
    c->photoReduced25 = false;
    c->photoPhaseFit = false;
    c->binnedFallbackContext.reset();
    for (auto& v : c->baselineBlackLevelPhysicalRggb) v *= 4;
    c->baselineWhiteLevel *= 4;
    for (auto& v : m.blackLevelPhysicalRggb) v *= 4;
    m.staticWhiteLevel *= 4;
    m.effectiveWhiteLevel *= 4;
    if (m.reportedDynamicWhiteLevel) *m.reportedDynamicWhiteLevel *= 4;
    // Defective-pixel locations do not survive the reduction. The noise profile stays: the denoise
    // and merge consumers map it with a = S / range, b = (S * black + O) / range^2, and the levels
    // above are x4, so a drops by 4 exactly as the variance of a sum of four samples does (b alike,
    // O being negligible next to S * black). Clearing it would turn profiled denoise off. The Bayer bin and
    // the 25 MP resample average differently from four samples: their factor scales S and O per CFA site
    // (the profile is [S, O] per site in buffer order; any other layout takes the sites' mean).
    if (m.sensorNoiseProfile.size() == 8) {
        for (uint32_t p = 0; p < 4; ++p) {
            m.sensorNoiseProfile[2 * p] *= noise[p];
            m.sensorNoiseProfile[2 * p + 1] *= noise[p];
        }
    } else {
        const double mean = (noise[0] + noise[1] + noise[2] + noise[3]) / 4;
        for (auto& v : m.sensorNoiseProfile) v *= mean;
    }
    m.hotPixelMap.clear();
    m.cameraContext = std::move(c);
}
}  // namespace rawrcam::capture
