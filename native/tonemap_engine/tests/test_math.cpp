#include <algorithm>
#include <cmath>
#include <iostream>

#include "tonemap/TonemapMath.h"
#include "tonemap/color/ColorSpace.h"

static float eval(float x, const tonemap::CurveCoefficients& c) {
    if (x <= c.x[0]) return 0.0f;
    if (x >= c.x[4]) return 1.0f;
    const int i = (x < c.x[1]) ? 0 : (x < c.x[2]) ? 1 : (x < c.x[3]) ? 2 : 3;
    const float t = (x - c.x[i]) / (c.x[i + 1] - c.x[i]);
    const auto q = c.cubic[i];
    return ((q[0] * t + q[1]) * t + q[2]) * t + q[3];
}
// AgX: presets, darktable invariants and the Blender matrices.
static int testAgx() {
    using namespace tonemap;
    const auto gray = [](const AgxParams& p, float y) { return agxRenderReference({y, y, y}, p); };
    for (uint32_t i = 0; i < kAgxPresetCount; ++i) {
        const AgxParams p = agxPreset(AgxPreset(i));
        if (!validAgx(p)) {
            std::cerr << "agx preset " << i << " invalid\n";
            return 20;
        }
        // Neutral stays neutral and the gray ramp is monotonic over the range.
        float prev = -1.0f;
        for (int k = 0; k <= 4000; ++k) {
            const float ev = -14.0f + 24.0f * k / 4000.0f;
            const auto o = gray(p, 0.18f * std::exp2(ev));
            if (!std::isfinite(o[0]) || std::abs(o[0] - o[1]) > 2e-5f || std::abs(o[1] - o[2]) > 2e-5f ||
                o[1] + 1e-6f < prev) {
                std::cerr << "agx gray ramp preset " << i << " ev " << ev << "\n";
                return 21;
            }
            prev = o[1];
        }
        if (prev < 0.99f || gray(p, 0.0f)[1] > 1e-6f) {
            std::cerr << "agx endpoints preset " << i << "\n";
            return 22;
        }
    }
    // Default pivot: middle gray renders to the 0.18 pivot target.
    if (std::abs(gray(AgxParams{}, 0.18f)[1] - 0.18f) > 1e-4f) {
        std::cerr << "agx pivot drift\n";
        return 23;
    }
    // Controls land on the intended fields (primaries are interleaved R/G/B).
    {
        float v[kAgxControlCount];
        std::copy(kAgxPresetControls[0], kAgxPresetControls[0] + kAgxControlCount, v);
        const auto at = [&](AgxControl c) -> float& { return v[static_cast<uint32_t>(c)]; };
        at(AgxControl::BlackEv) = -12.0f;
        at(AgxControl::Gamma) = 2.6f;
        at(AgxControl::LookLift) = 0.1f;
        at(AgxControl::GreenRotation) = 0.2f;
        at(AgxControl::BlueInset) = 0.3f;
        at(AgxControl::MasterUnrotation) = 0.5f;
        at(AgxControl::RedOutset) = 0.4f;
        at(AgxControl::BlueUnrotation) = -0.25f;
        const AgxParams p = agxFromControls(v);
        if (p.blackEv != -12.0f || p.gamma != 2.6f || p.lookLift != 0.1f || p.primaries.rotation[1] != 0.2f ||
            p.primaries.inset[2] != 0.3f || p.primaries.masterUnrotation != 0.5f || p.primaries.outset[0] != 0.4f ||
            p.primaries.unrotation[2] != -0.25f || !validAgx(p)) {
            std::cerr << "agx control mapping\n";
            return 24;
        }
    }
    // darktable's Blender-like primaries reproduce Blender's AgX inset and
    // outset (EaryChow AgX_LUT_Gen, Rec.2020) to its D50 approximation.
    {
        const AgxMatrices m = compileAgxMatrices(agxBlenderPrimaries());
        const color::Mat3 blenderInset{0.856627156f, 0.095121245f, 0.048251598f, 0.137318972f, 0.761241987f,
                                       0.101439041f, 0.111898208f, 0.076799415f, 0.811302377f};
        const color::Mat3 blenderOutset{1.12710057f,   -0.110606639f, -0.016493931f, -0.141329754f, 1.157823685f,
                                        -0.016493931f, -0.141329754f, -0.110606639f, 1.251936393f};
        const color::Mat3 toSrgb = color::linearGamutMatrix(color::Gamut::Rec2020, color::Gamut::SRgbRec709);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                float outset = 0.0f;
                for (int k = 0; k < 3; ++k) outset += toSrgb[r * 3 + k] * blenderOutset[k * 3 + c];
                if (std::abs(m.baseToRendering[r * 3 + c] - blenderInset[r * 3 + c]) > 3e-3f ||
                    std::abs(m.renderingToSrgb[r * 3 + c] - outset) > 4e-3f) {
                    std::cerr << "agx blender matrices\n";
                    return 25;
                }
            }
    }
    // Saturated, out-of-gamut and extreme input stays finite.
    for (const color::Vec3 c : {color::Vec3{-0.5f, 0.2f, 3.0f}, color::Vec3{1e7f, 0.0f, 0.0f},
                                color::Vec3{0.0f, 0.0f, 0.0f}, color::Vec3{50.0f, 0.001f, 20.0f}}) {
        for (uint32_t i = 0; i < kAgxPresetCount; ++i) {
            const auto o = agxRenderReference(c, agxPreset(AgxPreset(i)));
            if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2])) {
                std::cerr << "agx nonfinite\n";
                return 26;
            }
        }
    }
    std::cout << "CPP_AGX_TEST_PASS\n";
    return 0;
}

int main() {
    for (float black : {-12.f, -10.f, -8.f})
        for (float s : {-2.f, 0.f, 1.f})
            for (float ct : {.6f, 1.f, 1.5f})
                for (float shoulder : {1.f, 2.f, 2.3f})
                    for (float hb : {-1.f, 0.f, .25f}) {
                        auto p = tonemap::TonemapPresets::NeutralBaseline().params;
                        p.blackPointEV = black;
                        p.shadowLiftEV = s;
                        p.contrast = ct;
                        p.shoulderStartEV = shoulder;
                        p.highlightBiasEV = hb;
                        p.whitePointEV = 6.f;
                        auto c = tonemap::compileCurve(p);
                        float prev = eval(c.x[0] - 1, c);
                        for (int i = 1; i <= 200000; i++) {
                            float x = c.x[0] - 1 + (c.x[4] - c.x[0] + 2) * i / 200000.f;
                            float y = eval(x, c);
                            if (!std::isfinite(y) || y + 2e-6f < prev) {
                                std::cerr << "non-monotone/nonfinite\n";
                                return 1;
                            }
                            prev = y;
                        }
                        // Middle gray remains exactly anchored.
                        const auto cfg = tonemap::TonemapPresets::NeutralBaseline().config;
                        if (std::abs(eval(0.f, c) - cfg.middleGray) > 2e-6f) {
                            std::cerr << "middle gray drift\n";
                            return 2;
                        }
                    }
    {
        const auto preset = tonemap::TonemapPresets::NeutralBaseline();
        if (preset.params.exposureEV != 0.0f || preset.params.saturation != 0.0f || preset.params.vibrance != 0.0f ||
            preset.params.aePostGain != 1.0f || tonemap::kMaxAePostGain != 16.0f) {
            std::cerr << "baseline params drift\n";
            return 3;
        }
        auto custom = preset.config;
        custom.middleGray = 0.20f;
        custom.shadowAnchorEV = -2.5f;
        custom.shoulderOutputCap = 0.88f;
        auto c = tonemap::compileCurve(preset.params, custom);
        if (std::abs(eval(0.f, c) - 0.20f) > 2e-6f || std::abs(c.x[1] + 2.5f) > 1e-6f) {
            std::cerr << "config not applied\n";
            return 4;
        }
    }
    std::cout << "CPP_CONFIG_PRESET_TEST_PASS\n";
    std::cout << "CPP_CURVE_TEST_PASS\n";
    return testAgx();
}
