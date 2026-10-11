#pragma once
#include <array>
#include <cmath>
#include <stdexcept>

#include "tonemap/AdaptiveTone.h"
#include "tonemap/Agx.h"
#include "tonemap/color/ColorSpace.h"

namespace tonemap {

// Realtime photographic controls plus externally supplied AE post gain.
// aePostGain is capture-controller output, not an aesthetic default or sensor gain.
// Supported external AE post-gain range is 0 < gain <= 16x.
// 16x linear gain is +4 EV (log2(16) = 4). Applications integrating an
// exposure controller must configure its maximum post gain at or below this
// value; TonemapEngine rejects larger values and never silently clamps them.
inline constexpr float kMaxAePostGain = 16.0f;

// AgX writes outputSpace.transfer (sRGB or Rec.709) from a parametric curve
// instead of the RAWR NTRL LUT; see Agx.h.
enum class RenderTransform : unsigned { Existing = 0, SRgb = 1, Rec709 = 2, Log = 3, AgX = 4 };

struct TonemapParams {
    float exposureEV;
    // Photographic tone controls are conventional -100..+100 values.
    // Historical field names are retained to avoid a broad API/JNI rename.
    float blackPointEV;     // Blacks UI
    float shadowLiftEV;     // Shadows UI
    float midtoneLiftEV;    // Midtones UI
    float contrast;         // Contrast UI
    float shoulderStartEV;  // renderer-internal fixed base-curve anchor
    float whitePointEV;     // Whites UI
    // Independent tone-shaping control. It never enables, tunes, or re-runs RAW
    // channel reconstruction; negative values compress scene-linear highlights.
    float highlightBiasEV;  // Highlights UI
    float saturation;       // Saturation UI, conventional -100..+100
    float vibrance;         // Vibrance UI, conventional -100..+100

    // Externally supplied downstream linear gain from the exposure controller.
    // Identity is 1.0. Supported range: 0 < aePostGain <= kMaxAePostGain.
    // This is NOT sensor gain and does not imply SNR improvement. TonemapEngine
    // rejects out-of-range values; it never silently clamps AE decisions.
    // Kept separate from exposureEV even though both shift pre-tone scene exposure.
    float aePostGain = 1.0f;
    RenderTransform renderTransform = RenderTransform::Existing;
    color::ColorSpace outputSpace{};
    AdaptiveToneParams adaptiveTone{};
    // Read only by RenderTransform::AgX.
    AgxParams agx{};
};

// Renderer configuration. These values describe the current validated rendering
// model rather than user slider state. They can be replaced without changing
// the public control semantics or recompiling shaders.
struct TonemapConfig {
    float middleGray;
    float shadowAnchorEV;
    float shoulderOutputCap;
    float gamutCompressionStart;
    float vibranceStrength;
    float vibranceShadowStart;
    float vibranceShadowEnd;
};

struct TonemapPreset {
    TonemapParams params;
    TonemapConfig config;
};

namespace TonemapPresets {
// Synthetic/hardware-validated V1.3 baseline. Deliberately not named Natural:
// real-camera calibration has not happened yet.
constexpr TonemapPreset NeutralBaseline() noexcept {
    return {
        {        /* params */
         0.0f,   // exposureEV
         0.0f,   // Blacks UI
         0.0f,   // Shadows UI
         0.0f,   // Midtones UI
         0.0f,   // Contrast UI
         2.0f,   // shoulderStartEV (fixed base-render anchor)
         0.0f,   // Whites UI
         0.0f,   // Highlights UI
         0.0f,   // saturation UI
         0.0f,   // vibrance UI
         1.0f},  // aePostGain
        {        /* config */
         0.18f,  // middleGray
         -3.0f,  // shadowAnchorEV
         0.90f,  // shoulderOutputCap
         0.85f,  // gamutCompressionStart
         0.70f,  // vibranceStrength (+100 weak-color boost)
         0.02f,  // vibranceShadowStart
         0.10f}  // vibranceShadowEnd
    };
}
}  // namespace TonemapPresets

struct CurveCoefficients {
    std::array<float, 5> x{};
    std::array<float, 5> y{};
    std::array<std::array<float, 4>, 4> cubic{};  // a,b,c,d in normalized t
};

inline CurveCoefficients compileCurve(const TonemapParams& p, const TonemapConfig& cfg) {
    (void)p;
    const double middleGray = cfg.middleGray;
    // Iter24 photographic controls are post-render operators. The base render
    // spline keeps fixed neutral anchors and is never reshaped by Blacks,
    // Contrast, Highlights, or Whites.
    const double x0 = -10.0, x1 = cfg.shadowAnchorEV, x2 = 0.0, x3 = 2.0, x4 = 6.0;
    if (!(middleGray > 0.0 && middleGray < 1.0)) throw std::invalid_argument("illegal middleGray");
    if (!(cfg.shoulderOutputCap > middleGray && cfg.shoulderOutputCap < 1.0f))
        throw std::invalid_argument("illegal shoulderOutputCap");
    if (!(x0 < x1 && x1 < x2 && x2 < x3 && x3 < x4)) throw std::invalid_argument("illegal tone anchor ordering");

    std::array<double, 5> x{x0, x1, x2, x3, x4};
    const double tiny = 1e-6;
    double y1 = middleGray * std::exp2(x1);
    // V2 keeps the base shoulder neutral. Highlight shaping is a monotonic
    // post-curve transform that preserves both middle gray and display white.
    double y3 = middleGray * std::exp2(x3);
    y1 = std::min(std::max(y1, tiny), middleGray - tiny);
    y3 = std::min(std::max(y3, middleGray + tiny), double(cfg.shoulderOutputCap));
    std::array<double, 5> y{0.0, y1, middleGray, y3, 1.0};

    std::array<double, 4> h{}, d{};
    for (int i = 0; i < 4; i++) {
        h[i] = x[i + 1] - x[i];
        d[i] = (y[i + 1] - y[i]) / h[i];
    }
    std::array<double, 5> m{};
    m[0] = 0.0;
    m[4] = 0.0;
    auto harmonic = [&](int i) -> double {
        const double dp = d[i - 1], dn = d[i];
        if (dp <= 0.0 || dn <= 0.0) return 0.0;
        const double hp = h[i - 1], hn = h[i], w1 = 2.0 * hn + hp, w2 = hn + 2.0 * hp;
        return (w1 + w2) / (w1 / dp + w2 / dn);
    };
    m[1] = harmonic(1);
    m[3] = harmonic(3);
    // V2 base curve has a fixed neutral middle-gray slope. Contrast is applied
    // afterward as a monotonic pivot-preserving transform, avoiding limiter collapse.
    m[2] = middleGray * std::log(2.0);

    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < 4; i++) {
            if (d[i] <= 0.0) {
                m[i] = m[i + 1] = 0.0;
                continue;
            }
            const double aa = m[i] / d[i], bb = m[i + 1] / d[i], ss = aa * aa + bb * bb;
            if (ss > 9.0) {
                const double tau = 3.0 / std::sqrt(ss);
                m[i] = tau * aa * d[i];
                m[i + 1] = tau * bb * d[i];
            }
        }
    }

    CurveCoefficients out;
    for (int i = 0; i < 5; i++) {
        out.x[i] = float(x[i]);
        out.y[i] = float(y[i]);
    }
    for (int i = 0; i < 4; i++) {
        const double hi = h[i], yi = y[i], yj = y[i + 1], mi = m[i], mj = m[i + 1];
        out.cubic[i] = {float(2 * yi - 2 * yj + hi * (mi + mj)), float(-3 * yi + 3 * yj - hi * (2 * mi + mj)),
                        float(hi * mi), float(yi)};
    }
    return out;
}

// Convenience overload for tools/tests that explicitly want the validated
// baseline. Production engine code passes its TonemapConfig explicitly.
inline CurveCoefficients compileCurve(const TonemapParams& p) {
    return compileCurve(p, TonemapPresets::NeutralBaseline().config);
}

}  // namespace tonemap
