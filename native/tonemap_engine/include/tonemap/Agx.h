#pragma once
// AgX render transform, ported from darktable src/iop/agx.c (GPL-3.0-or-later,
// Copyright (C) 2025-2026 darktable developers). The host compiles the user
// parameters into curve coefficients and three matrices; agx.glsl evaluates
// them per pixel and agxRenderReference() below is its CPU reference.
//
// Base primaries are always linear Rec2020 (darktable's default working
// profile), so darktable's "working profile" base used by the smooth presets
// is the same space here. The primaries are inset/rotated in darktable's
// D50-adapted xy, which reproduces the Blender AgX matrices.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "tonemap/color/ColorSpace.h"

namespace tonemap {

struct AgxPrimaries {
    std::array<float, 3> inset{0.29462451f, 0.25861925f, 0.14641371f};
    std::array<float, 3> rotation{0.03540329f, -0.02108586f, -0.06305724f};
    float masterOutset = 1.0f;
    float masterUnrotation = 0.0f;
    std::array<float, 3> outset{0.290776401758f, 0.263155400753f, 0.045810721815f};
    std::array<float, 3> unrotation{0.03540329f, -0.02108586f, -0.06305724f};
};

// User parameters in darktable's units. Defaults are darktable's
// "scene-referred default" preset. The pivot is stored as EV relative to
// middle gray (darktable stores it normalized to the log range), so moving
// the white or black point keeps the pivot on the same scene exposure.
struct AgxParams {
    float lookLift = 0.0f;
    float lookSlope = 1.0f;
    float lookBrightness = 1.0f;
    float lookSaturation = 1.0f;
    float preserveHue = 0.6f;

    float blackEv = -10.0f;
    float whiteEv = 6.5f;
    float pivotEv = 0.0f;
    float pivotOutput = 0.18f;
    float contrast = 3.0f;
    float toeLinear = 0.0f;
    float shoulderLinear = 0.0f;
    float toePower = 1.5f;
    float shoulderPower = 3.3f;
    float gamma = 2.2f;
    bool autoGamma = false;
    float targetBlack = 0.0f;
    float targetWhite = 1.0f;

    AgxPrimaries primaries{};
};

enum class AgxPreset : uint32_t { Default = 0, BlenderBase = 1, BlenderPunchy = 2, Sigmoid = 3, Smooth = 4 };
inline constexpr uint32_t kAgxPresetCount = 5;

// Negative Highlights softens the AgX shoulder instead of compressing scene
// linear luminance before the render: -100 scales shoulder power by this.
// (Widening the white point barely moves highlights, because the contrast
// is range-adjusted: +3 EV lowers +4 EV gray by only ~0.006 encoded.)
inline constexpr float kAgxHighlightShoulderScale = 0.45f;

namespace agx_detail {
inline constexpr double kEpsilon = 1e-6;
inline constexpr double kDefaultGamma = 2.2;
using M3 = std::array<std::array<double, 3>, 3>;
using V2 = std::array<double, 2>;

inline M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) r[i][j] += a[i][k] * b[k][j];
    return r;
}

inline M3 inverse(const M3& m) {
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0],
                 h = m[2][1], i = m[2][2];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    return {{{A / det, -(b * i - c * h) / det, (b * f - c * e) / det},
             {B / det, (a * i - c * g) / det, -(a * f - c * d) / det},
             {C / det, -(a * h - b * g) / det, (a * e - b * d) / det}}};
}

inline std::array<double, 3> xyToXyz(const V2& xy) { return {xy[0] / xy[1], 1.0, (1.0 - xy[0] - xy[1]) / xy[1]}; }

// RGB->XYZ from chromaticities (Bruce Lindbloom); columns are the primaries.
inline M3 rgbToXyz(const std::array<V2, 3>& primaries, const std::array<double, 3>& white) {
    M3 p{};
    for (int c = 0; c < 3; c++) {
        const auto v = xyToXyz(primaries[c]);
        for (int r = 0; r < 3; r++) p[r][c] = v[r];
    }
    const M3 inv = inverse(p);
    for (int c = 0; c < 3; c++) {
        const double s = inv[c][0] * white[0] + inv[c][1] * white[1] + inv[c][2] * white[2];
        for (int r = 0; r < 3; r++) p[r][c] *= s;
    }
    return p;
}

// darktable's linear Rec2020 profile as lcms stores it: D50 media white and
// Bradford-adapted colorants.
struct BaseProfile {
    std::array<V2, 3> primaries;
    V2 white;
    M3 toXyz;
};

inline BaseProfile rec2020Profile() {
    const std::array<double, 3> d65{0.95045471, 1.0, 1.08905029}, d50{0.9642, 1.0, 0.8249};
    const M3 brad{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
    const M3 bradInv = inverse(brad);
    std::array<double, 3> src{}, dst{};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            src[r] += brad[r][k] * d65[k];
            dst[r] += brad[r][k] * d50[k];
        }
    const M3 scale{{{dst[0] / src[0], 0, 0}, {0, dst[1] / src[1], 0}, {0, 0, dst[2] / src[2]}}};
    const M3 adapt = mul(bradInv, mul(scale, brad));
    const M3 toXyz = mul(adapt, rgbToXyz({V2{0.708, 0.292}, V2{0.170, 0.797}, V2{0.131, 0.046}}, d65));
    BaseProfile p{};
    for (int c = 0; c < 3; c++) {
        const double sum = toXyz[0][c] + toXyz[1][c] + toXyz[2][c];
        p.primaries[c] = {toXyz[0][c] / sum, toXyz[1][c] / sum};
    }
    const double wsum = d50[0] + d50[1] + d50[2];
    p.white = {d50[0] / wsum, d50[1] / wsum};
    p.toXyz = toXyz;
    return p;
}

// darktable custom_primaries.c: move a primary along a rotated hue line from
// the white point, scaled relative to the gamut edge in that direction.
inline V2 rotateAndScalePrimary(const BaseProfile& p, double scaling, double rotation, int index) {
    const double angle =
        std::atan2(p.primaries[index][1] - p.white[1], p.primaries[index][0] - p.white[0]) + rotation;
    const double c = std::cos(angle), s = std::sin(angle);
    double distance = 1e30;
    for (int i = 0; i < 3; i++) {
        const V2 a = p.primaries[i], b = p.primaries[(i + 1) % 3];
        const double x1 = p.white[0], y1 = p.white[1], x2 = x1 + c, y2 = y1 + s;
        const double den = (x1 - x2) * (a[1] - b[1]) - (a[0] - b[0]) * (y1 - y2);
        if (den == 0.0) continue;
        const double t = ((x1 - a[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (y1 - a[1])) / den;
        if (t >= 0.0) distance = std::min(distance, t);
    }
    return {p.white[0] + scaling * distance * c, p.white[1] + scaling * distance * s};
}

inline double clampd(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

inline double pivotYAtGamma(const AgxParams& p, double gamma) {
    return std::pow(clampd(p.pivotOutput, p.targetBlack, p.targetWhite), 1.0 / gamma);
}

inline double sigmoidScale(double limitX, double limitY, double transitionX, double transitionY, double slope,
                           double power) {
    const double projected = slope * std::max(kEpsilon, limitX - transitionX);
    const double actual = std::max(kEpsilon, limitY - transitionY);
    const double base = std::max(kEpsilon, std::pow(actual, -power) - std::pow(projected, -power));
    return std::min(1e9, std::pow(base, -1.0 / power));
}

inline double slopeGammaCompensation(const AgxParams& p, double gamma, double pivotY) {
    const double atCurrent = gamma * std::pow(std::max(kEpsilon, pivotY), gamma - 1.0);
    const double atDefault =
        kDefaultGamma * std::pow(std::max(kEpsilon, pivotYAtGamma(p, kDefaultGamma)), kDefaultGamma - 1.0);
    return atCurrent / atDefault;
}
}  // namespace agx_detail

inline AgxPrimaries agxBlenderPrimaries() { return {}; }

inline AgxPrimaries agxSmoothPrimaries() {
    constexpr float kDeg = 3.14159265358979f / 180.0f;
    AgxPrimaries p{};
    p.inset = {0.10f, 0.10f, 0.15f};
    p.rotation = {2.0f * kDeg, -1.0f * kDeg, -3.0f * kDeg};
    // Sigmoid smooth does not restore purity, but the master control can.
    p.masterOutset = 0.0f;
    p.masterUnrotation = 1.0f;
    p.outset = p.inset;
    p.unrotation = p.rotation;
    return p;
}

// Per-preset defaults of the five app-facing controls, in AgxPreset order:
// white EV, shoulder power, toe power, purity (master outset), preserve hue.
// The app mirrors this table in AgxSettings.kt; AgxPresetSyncTest reads it
// from this file, so keep it a plain table of float literals.
// AGX_PRESET_CONTROLS_BEGIN
inline constexpr float kAgxPresetControls[kAgxPresetCount][5] = {
    {6.5f, 3.30f, 1.50f, 1.0f, 0.6f},  // Default
    {6.5f, 1.50f, 1.50f, 1.0f, 0.6f},  // BlenderBase
    {6.5f, 1.50f, 1.50f, 1.0f, 0.6f},  // BlenderPunchy
    {6.5f, 3.71f, 1.40f, 1.0f, 1.0f},  // Sigmoid
    {6.5f, 3.02f, 1.61f, 0.0f, 0.0f},  // Smooth
};
// AGX_PRESET_CONTROLS_END

// darktable's built-in presets.
inline AgxParams agxPreset(AgxPreset preset) {
    AgxParams p{};
    switch (preset) {
        case AgxPreset::Default:
            break;
        case AgxPreset::Sigmoid:
            p.contrast = 3.07f;
            break;
        case AgxPreset::BlenderBase:
        case AgxPreset::BlenderPunchy: {
            p.gamma = 2.4f;
            // Blender's 2.4 contrast at gamma 2.4, expressed relative to the
            // 2.2-gamma slope compensation applied when compiling.
            const double pivotY = std::pow(0.18, 1.0 / 2.4);
            p.contrast = float(2.4 * agx_detail::slopeGammaCompensation(p, 2.4, pivotY));
            if (preset == AgxPreset::BlenderPunchy) {
                p.lookBrightness = 1.0f / (1.35f * 1.35f);
                p.lookSaturation = 1.4f;
            }
            break;
        }
        case AgxPreset::Smooth:
            p.primaries = agxSmoothPrimaries();
            p.contrast = 2.98f;
            break;
    }
    const float* c = kAgxPresetControls[static_cast<uint32_t>(preset)];
    p.whiteEv = c[0];
    p.shoulderPower = c[1];
    p.toePower = c[2];
    p.primaries.masterOutset = c[3];
    p.preserveHue = c[4];
    return p;
}

// Ranges are darktable's slider limits.
inline bool validAgx(const AgxParams& p) noexcept {
    const auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    bool ok = in(p.lookLift, -1.0f, 1.0f) && in(p.lookSlope, 0.0f, 10.0f) && in(p.lookBrightness, 0.0f, 100.0f) &&
              in(p.lookSaturation, 0.0f, 10.0f) && in(p.preserveHue, 0.0f, 1.0f) && in(p.blackEv, -20.0f, -0.1f) &&
              in(p.whiteEv, 0.1f, 20.0f) && in(p.pivotEv, p.blackEv, p.whiteEv) && in(p.pivotOutput, 0.0f, 1.0f) &&
              in(p.contrast, 0.1f, 10.0f) && in(p.toeLinear, 0.0f, 1.0f) && in(p.shoulderLinear, 0.0f, 1.0f) &&
              in(p.toePower, 0.0f, 10.0f) && in(p.shoulderPower, 0.0f, 10.0f) && in(p.gamma, 0.01f, 100.0f) &&
              in(p.targetBlack, 0.0f, 0.15f) && in(p.targetWhite, 0.2f, 1.0f) &&
              in(p.primaries.masterOutset, 0.0f, 2.0f) && in(p.primaries.masterUnrotation, 0.0f, 2.0f);
    for (int i = 0; i < 3; i++)
        ok = ok && in(p.primaries.inset[i], 0.0f, 0.99f) && in(p.primaries.outset[i], 0.0f, 0.99f) &&
             in(p.primaries.rotation[i], -0.5236f, 0.5236f) && in(p.primaries.unrotation[i], -0.5236f, 0.5236f);
    return ok;
}

// The engine's negative Highlights control maps onto the AgX shoulder.
inline AgxParams agxWithHighlights(AgxParams p, float highlightsUI) noexcept {
    const float recovery = std::clamp(-highlightsUI * 0.01f, 0.0f, 1.0f);
    p.shoulderPower *= 1.0f - recovery * (1.0f - kAgxHighlightShoulderScale);
    return p;
}

struct AgxCurve {
    float blackEv, rangeEv, gamma;
    float slope, intercept;
    float targetBlack, targetWhite;
    float toePower, toeTransitionX, toeTransitionY, toeScale;
    bool convexToe;
    float toeFallbackCoefficient, toeFallbackPower;
    float shoulderPower, shoulderTransitionX, shoulderTransitionY, shoulderScale;
    bool concaveShoulder;
    float shoulderFallbackCoefficient, shoulderFallbackPower;
    float lookLift, lookSlope, lookPower, lookSaturation;
    bool lookTuned;
    float preserveHue;
};

// darktable _calculate_tone_mapping_params().
inline AgxCurve compileAgxCurve(const AgxParams& p) {
    using namespace agx_detail;
    AgxCurve t{};
    const double range = double(p.whiteEv) - p.blackEv;
    t.blackEv = p.blackEv;
    t.rangeEv = float(range);
    const double pivotX = clampd((double(p.pivotEv) - p.blackEv) / range, kEpsilon, 1.0 - kEpsilon);
    double gamma = p.gamma;
    if (p.autoGamma && pivotX > 0.0 && p.pivotOutput > 0.0f) gamma = std::log2(double(p.pivotOutput)) / std::log2(pivotX);
    t.gamma = float(gamma);
    const double pivotY = pivotYAtGamma(p, gamma);
    // 16.5 EV is darktable's default range: contrast keeps its per-EV meaning.
    const double slope = p.contrast * (range / 16.5) / slopeGammaCompensation(p, gamma, pivotY);
    t.slope = float(slope);

    const double targetBlack = std::pow(double(p.targetBlack), 1.0 / gamma);
    t.targetBlack = float(targetBlack);
    const double toePower = std::max(0.01, double(p.toePower));
    t.toePower = float(toePower);
    double dx = (pivotY - targetBlack) * p.toeLinear / slope;
    const double toeX = std::max(kEpsilon, pivotX - dx);
    const double toeY = pivotY - slope * (pivotX - toeX);
    t.toeTransitionX = float(toeX);
    t.toeTransitionY = float(toeY);
    t.toeScale = float(-sigmoidScale(1.0, 1.0 - targetBlack, 1.0 - toeX, 1.0 - toeY, slope, toePower));
    const double toeDy = std::max(kEpsilon, toeY - targetBlack);
    t.convexToe = toeDy / toeX > slope;
    const double toeFallbackPower = slope * toeX / toeDy;
    t.toeFallbackPower = float(toeFallbackPower);
    t.toeFallbackCoefficient = float(toeDy / std::pow(toeX, toeFallbackPower));
    t.intercept = float(toeY - slope * toeX);

    const double targetWhite = std::pow(double(p.targetWhite), 1.0 / gamma);
    t.targetWhite = float(targetWhite);
    dx = (targetWhite - pivotY) * p.shoulderLinear / slope;
    const double shoulderX = std::min(1.0 - kEpsilon, pivotX + dx);
    const double shoulderY = pivotY + slope * (shoulderX - pivotX);
    t.shoulderTransitionX = float(shoulderX);
    t.shoulderTransitionY = float(shoulderY);
    const double shoulderPower = std::max(0.01, double(p.shoulderPower));
    t.shoulderPower = float(shoulderPower);
    t.shoulderScale = float(sigmoidScale(1.0, targetWhite, shoulderX, shoulderY, slope, shoulderPower));
    const double shoulderLength = 1.0 - shoulderX;
    const double shoulderDy = std::max(kEpsilon, targetWhite - shoulderY);
    t.concaveShoulder = shoulderDy / shoulderLength > slope;
    const double shoulderFallbackPower = slope * shoulderLength / shoulderDy;
    t.shoulderFallbackPower = float(shoulderFallbackPower);
    t.shoulderFallbackCoefficient = float(shoulderDy / std::pow(shoulderLength, shoulderFallbackPower));

    const double brightness = p.lookBrightness;
    t.lookPower = float(brightness < 1.0 ? 1.0 / std::sqrt(std::max(brightness, kEpsilon)) : 1.0 / brightness);
    t.lookLift = p.lookLift;
    t.lookSlope = p.lookSlope;
    t.lookSaturation = p.lookSaturation;
    t.lookTuned = p.lookSlope != 1.0f || p.lookBrightness != 1.0f || p.lookLift != 0.0f || p.lookSaturation != 1.0f;
    t.preserveHue = p.preserveHue;
    return t;
}

// Row-major matrices around the per-channel curve.
struct AgxMatrices {
    color::Mat3 ap1ToBase;        // linear AP1 -> linear Rec2020 (gamut compression runs here)
    color::Mat3 baseToRendering;  // inset and rotation
    color::Mat3 renderingToSrgb;  // outset/unrotation -> Rec2020 -> linear sRGB/Rec.709
    color::Vec3 renderingLuma;    // Y row of the inset space, for the look saturation
};

// darktable _create_matrices() with base = pipe = linear Rec2020.
inline AgxMatrices compileAgxMatrices(const AgxPrimaries& q) {
    using namespace agx_detail;
    const BaseProfile base = rec2020Profile();
    const M3 xyzToBase = inverse(base.toXyz);
    const std::array<double, 3> white = xyToXyz(base.white);
    std::array<V2, 3> inset{}, outset{};
    for (int i = 0; i < 3; i++) {
        inset[i] = rotateAndScalePrimary(base, 1.0 - q.inset[i], q.rotation[i], i);
        outset[i] = rotateAndScalePrimary(base, 1.0 - double(q.masterOutset) * q.outset[i],
                                          double(q.masterUnrotation) * q.unrotation[i], i);
    }
    const M3 renderingToXyz = rgbToXyz(inset, white);
    const M3 baseToRendering = mul(xyzToBase, renderingToXyz);
    const M3 renderingToBase = inverse(mul(xyzToBase, rgbToXyz(outset, white)));
    const auto toM3 = [](const color::Mat3& m) {
        M3 r{};
        for (int i = 0; i < 9; i++) r[i / 3][i % 3] = m[i];
        return r;
    };
    const auto toMat3 = [](const M3& m) {
        color::Mat3 r{};
        for (int i = 0; i < 9; i++) r[i] = float(m[i / 3][i % 3]);
        return r;
    };
    AgxMatrices out{};
    out.ap1ToBase = color::linearGamutMatrix(color::Gamut::AcesCgAp1, color::Gamut::Rec2020);
    out.baseToRendering = toMat3(baseToRendering);
    out.renderingToSrgb =
        toMat3(mul(toM3(color::linearGamutMatrix(color::Gamut::Rec2020, color::Gamut::SRgbRec709)), renderingToBase));
    out.renderingLuma = {float(renderingToXyz[1][0]), float(renderingToXyz[1][1]), float(renderingToXyz[1][2])};
    return out;
}

namespace agx_detail {
inline float sigmoid(float x, float power) { return x / std::pow(1.0f + std::pow(x, power), 1.0f / power); }

inline float curve(float x, const AgxCurve& t) {
    float r;
    if (x < t.toeTransitionX) {
        r = t.convexToe ? (x < 0.0f ? t.targetBlack
                                    : t.targetBlack + std::max(0.0f, t.toeFallbackCoefficient *
                                                                         std::pow(x, t.toeFallbackPower)))
                        : t.toeScale * sigmoid(t.slope * (x - t.toeTransitionX) / t.toeScale, t.toePower) +
                              t.toeTransitionY;
    } else if (x <= t.shoulderTransitionX) {
        r = t.slope * x + t.intercept;
    } else {
        r = t.concaveShoulder
                ? (x >= 1.0f ? t.targetWhite
                             : t.targetWhite - std::max(0.0f, t.shoulderFallbackCoefficient *
                                                                  std::pow(1.0f - x, t.shoulderFallbackPower)))
                : t.shoulderScale * sigmoid(t.slope * (x - t.shoulderTransitionX) / t.shoulderScale, t.shoulderPower) +
                      t.shoulderTransitionY;
    }
    return std::clamp(r, t.targetBlack, t.targetWhite);
}

// Blender luminance_compenstation_bt2020.py, via darktable.
inline color::Vec3 compressIntoGamut(color::Vec3 c) {
    const float k[3] = {0.2658180370250449f, 0.59846986045365f, 0.1357121025213052f};
    const auto luma = [&](const color::Vec3& v) { return v[0] * k[0] + v[1] * k[1] + v[2] * k[2]; };
    const auto opponentLuma = [&](const color::Vec3& v) {
        const float m = std::max({v[0], v[1], v[2]});
        const color::Vec3 o{m - v[0], m - v[1], m - v[2]};
        return std::max({o[0], o[1], o[2]}) - luma(o);
    };
    const float target = opponentLuma(c) + luma(c);
    const float offset = std::max(-std::min({c[0], c[1], c[2]}), 0.0f);
    for (float& v : c) v += offset;
    const float compensated = opponentLuma(c) + luma(c);
    const float ratio = compensated > target && compensated > float(kEpsilon) ? target / compensated : 1.0f;
    for (float& v : c) v *= ratio;
    return c;
}

// Hue in [0,1), saturation and value, as darktable dt_RGB_2_HSV.
inline color::Vec3 rgbToHsv(const color::Vec3& c) {
    const float mn = std::min({c[0], c[1], c[2]}), mx = std::max({c[0], c[1], c[2]}), d = mx - mn;
    if (!(std::abs(mx) > 1e-6f && std::abs(d) > 1e-6f)) return {0.0f, 0.0f, mx};
    float h = c[0] == mx ? (c[1] - c[2]) / d : c[1] == mx ? 2.0f + (c[2] - c[0]) / d : 4.0f + (c[0] - c[1]) / d;
    h /= 6.0f;
    return {h - std::floor(h), d / mx, mx};
}

inline color::Vec3 hsvToRgb(const color::Vec3& hsv) {
    const float C = hsv[1] * hsv[2], m = hsv[2] - C, h = hsv[0] * 6.0f, i = std::floor(h), fc = (h - i) * C;
    const float top = C + m, inc = fc + m, dec = top - fc;
    switch (int(i)) {
        case 0: return {top, inc, m};
        case 1: return {dec, top, m};
        case 2: return {m, top, inc};
        case 3: return {m, dec, top};
        case 4: return {inc, m, top};
        default: return {top, m, dec};
    }
}
}  // namespace agx_detail

// Linear AP1 (after exposure) -> display-linear sRGB/Rec.709, unclamped.
inline color::Vec3 agxRender(color::Vec3 ap1, const AgxCurve& t, const AgxMatrices& m) {
    using namespace agx_detail;
    for (float& v : ap1) v = std::isnan(v) ? 0.0f : std::clamp(v, -1e6f, 1e6f);
    const color::Vec3 rgb = color::applyMatrix(m.baseToRendering, compressIntoGamut(color::applyMatrix(m.ap1ToBase, ap1)));
    const bool restoreHue = t.preserveHue != 0.0f;
    const float hueBefore = restoreHue ? rgbToHsv(rgb)[0] : 0.0f;
    color::Vec3 out{};
    for (int k = 0; k < 3; k++) {
        const float relative = std::max(float(kEpsilon), rgb[k] / 0.18f);
        out[k] = curve(std::clamp((std::log2(relative) - t.blackEv) / t.rangeEv, 0.0f, 1.0f), t);
    }
    if (t.lookTuned) {
        const float lm = t.lookSlope / (1.0f + t.lookLift), lb = t.lookLift * lm;
        for (float& v : out) {
            const float s = lm * v + lb;
            v = s > 0.0f ? std::pow(s, t.lookPower) : s;
        }
        const float y = out[0] * m.renderingLuma[0] + out[1] * m.renderingLuma[1] + out[2] * m.renderingLuma[2];
        for (float& v : out) v = y + t.lookSaturation * (v - y);
    }
    for (float& v : out) v = std::pow(std::max(0.0f, v), t.gamma);
    if (restoreHue) {
        color::Vec3 hsv = rgbToHsv(out);
        const float h = hueBefore + (1.0f - t.preserveHue) * std::remainder(hsv[0] - hueBefore, 1.0f);
        hsv[0] = h - std::floor(h);
        out = hsvToRgb(hsv);
    }
    return color::applyMatrix(m.renderingToSrgb, out);
}

inline color::Vec3 agxRenderReference(const color::Vec3& ap1, const AgxParams& p) {
    return agxRender(ap1, compileAgxCurve(p), compileAgxMatrices(p.primaries));
}

}  // namespace tonemap
