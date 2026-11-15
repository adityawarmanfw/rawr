#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include "metadata/FrameMetadataSnapshot.h"
namespace rawrcam::capture {
inline bool reducedRawRequested(const metadata::CameraContextMetadata& c) {
    return c.photoReduced && c.cfaBlock == 4 && bool(c.binnedFallbackContext);
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
    c->geometry.rawBufferWidth /= 2;
    c->geometry.rawBufferHeight /= 2;
    c->cfaBlock = 1;
    c->photoCfaBlock = 1;
    c->photoReduced = false;
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
    // O being negligible next to S * black). Clearing it would turn profiled denoise off.
    m.hotPixelMap.clear();
    m.cameraContext = std::move(c);
}
}  // namespace rawrcam::capture
