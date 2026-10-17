#pragma once

#include <algorithm>
#include <cmath>

namespace rawrcam::color {

// AE post-gain cap with exponential soft knee.
//
// rawBoost100 is the Camera2 CONTROL_POST_RAW_SENSITIVITY_BOOST result
// (100 == 1x). maxGain is the user-configured linear cap (1, 2, or 4,
// default 4x). kneeWidthEv is the HighlightProtection-driven knee width
// (Low=2, Normal=1, High=0.5; default 1), matching the print-scan
// exponential shoulder idiom already in-tree.
//
// - raw <= cap: identity (linear, C1 at hinge).
// - raw > cap and cap > 1: capEV + width*(1-exp(-excess/width)).
// - cap <= 1: hard cap (no knee) so "1x" truly means no lift.
// Result is clamped to [1, 16], within TonemapEngine's ceiling
// (TonemapMath.h kMaxAePostGain), which leaves room for HDR+ headroom lifts.
inline float effectiveAePostGain(int32_t rawBoost100, float maxGain, float kneeWidthEv = 1.0f) noexcept {
    float rawGain = std::max(1.0f, static_cast<float>(rawBoost100) / 100.0f);
    float cap = std::clamp(maxGain, 1.0f, 16.0f);
    if (!(cap > 1.0f)) {
        return std::clamp(std::min(rawGain, 1.0f), 1.0f, 16.0f);
    }
    if (rawGain <= cap) {
        return std::clamp(rawGain, 1.0f, 16.0f);
    }
    const float width = std::clamp(kneeWidthEv, 0.25f, 4.0f);
    const float rawEv = std::log2(rawGain);
    const float capEv = std::log2(cap);
    const float excess = rawEv - capEv;
    const float effEv = capEv + width * (1.0f - std::exp(-excess / width));
    return std::clamp(std::exp2(effEv), 1.0f, 16.0f);
}

}  // namespace rawrcam::color
