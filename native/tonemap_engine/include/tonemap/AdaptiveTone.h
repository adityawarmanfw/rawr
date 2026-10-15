#pragma once
#include <algorithm>
#include <cmath>

namespace tonemap {


// Peak lift sits below the scene key so shadows get the most and the key
// only part of it. The toe fades to identity kAdaptiveToeEv below the peak.
inline constexpr float kAdaptivePeakBelowKeyEv = 1.5f;
inline constexpr float kAdaptiveToeEv = 5.0f;
// Lift per EV of shoulder width. The shoulder slope in log luminance is
// >= 1 - 1.5 * kAdaptiveMaxLiftPerEv = 0.625, so peak-to-highlight contrast
// keeps at least ~5/8 of its separation instead of collapsing toward 1/4.
inline constexpr float kAdaptiveMaxLiftPerEv = 0.25f;
// Per-capture override range (HDR+ Bracketed lifts its clean merged
// shadows further). 0.4 keeps a shoulder slope >= 0.4, still monotonic.
inline constexpr float kAdaptiveMinLiftPerEv = 0.1f;
inline constexpr float kAdaptiveMaxLiftPerEvLimit = 0.4f;

// Coordinates are log2 luminance after HAL gain, before creative exposure.
// Identity defaults preserve old capture jobs and disabled rendering exactly.
struct AdaptiveToneParams {
    float liftEv = 0.0f;
    float keyEv = -4.0f;
    float highlightEv = 0.0f;
    // Contrast limit: lift per EV of shoulder width.
    float maxLiftPerEv = kAdaptiveMaxLiftPerEv;
};

// Contrast limit actually used (invalid values fall back to the default).
inline float adaptiveToneLiftPerEv(const AdaptiveToneParams& p) noexcept {
    return std::isfinite(p.maxLiftPerEv) && p.maxLiftPerEv >= kAdaptiveMinLiftPerEv
               ? std::min(p.maxLiftPerEv, kAdaptiveMaxLiftPerEvLimit)
               : kAdaptiveMaxLiftPerEv;
}

inline float adaptiveTonePeakEv(const AdaptiveToneParams& p) noexcept { return p.keyEv - kAdaptivePeakBelowKeyEv; }

// Largest lift the curve applies for these anchors; 0 if they are degenerate.
inline float adaptiveToneMaxLift(const AdaptiveToneParams& p) noexcept {
    return adaptiveToneLiftPerEv(p) * std::max(p.highlightEv - adaptiveTonePeakEv(p), 0.0f);
}

inline bool validAdaptiveTone(const AdaptiveToneParams& p) noexcept {
    return std::isfinite(p.liftEv) && p.liftEv >= 0.0f && p.liftEv <= 3.0f && std::isfinite(p.keyEv) &&
           std::isfinite(p.highlightEv) && std::isfinite(p.maxLiftPerEv) && p.maxLiftPerEv >= kAdaptiveMinLiftPerEv &&
           p.maxLiftPerEv <= kAdaptiveMaxLiftPerEvLimit;
}

// CPU reference for shader_common/shaders/adaptive_tone.glsl.
// Both joins have unit slope; near black and above highlightEv the curve is
// identity. Re-limit every evaluation so interpolated/smoothed parameters
// cannot invert tones or over-flatten the shoulder.
inline float adaptiveToneGain(float y, const AdaptiveToneParams& p) noexcept {
    const float peak = adaptiveTonePeakEv(p);
    const float span = p.highlightEv - peak;
    if (!(p.liftEv > 0.0f) || !(y > 0.0f) || !(span > 0.0f)) return 1.0f;
    const float ev = std::log2(y);
    if (ev >= p.highlightEv) return 1.0f;
    const float lift = std::min(p.liftEv, adaptiveToneLiftPerEv(p) * span);
    const auto smooth = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float toe = smooth((ev - (peak - kAdaptiveToeEv)) / kAdaptiveToeEv);
    const float shoulder = 1.0f - smooth((ev - peak) / span);
    return std::exp2(lift * toe * shoulder);
}
}  // namespace tonemap
