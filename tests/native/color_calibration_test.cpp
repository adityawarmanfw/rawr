#include "color/ColorCalibration.h"
#include "color/ColorMath.h"

#include <array>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>

namespace {
using rawrcam::metadata::Matrix3x3;

Matrix3x3 matrix(std::array<float,9> values) {
    Matrix3x3 out{};
    out.rowMajor = values;
    out.valid = true;
    return out;
}

bool near(float a, float b, float tolerance) { return std::abs(a-b) <= tolerance; }

rawrcam::metadata::FrameMetadataSnapshot fixture(
    const char* id,
    const char* lens,
    uint32_t width,
    uint32_t height,
    const std::array<float,9>& color1,
    const std::array<float,9>& color2,
    const std::array<float,9>& calibration1,
    const std::array<float,9>& calibration2,
    const std::array<float,9>& forward1,
    const std::array<float,9>& forward2,
    const std::array<float,4>& gains,
    const std::array<float,3>& neutral) {
    auto c = std::make_shared<rawrcam::metadata::CameraContextMetadata>();
    c->cameraContextGeneration = 1;
    c->cameraId = id;
    c->lensId = lens;
    c->geometry.rawBufferWidth = width;
    c->geometry.rawBufferHeight = height;
    c->rawPreviewCfa = 0;
    c->baselineBlackLevelPhysicalRggb = {64,64,64,64};
    c->baselineWhiteLevel = 1023;
    c->color.referenceIlluminant1 = 21; // D65
    c->color.referenceIlluminant2 = 17; // Standard A
    c->color.colorTransform1 = matrix(color1);
    c->color.colorTransform2 = matrix(color2);
    c->color.calibrationTransform1 = matrix(calibration1);
    c->color.calibrationTransform2 = matrix(calibration2);
    c->color.forwardMatrix1 = matrix(forward1);
    c->color.forwardMatrix2 = matrix(forward2);

    rawrcam::metadata::FrameMetadataSnapshot f{};
    f.cameraContext = c;
    f.frameOrdinal = 1;
    f.timestampNs = 1;
    f.blackLevelPhysicalRggb = {64,64,64,64};
    f.staticWhiteLevel = 1023;
    f.effectiveWhiteLevel = 1023;
    f.colorCorrectionGainsRggb = gains;
    f.neutralColorPoint = neutral;
    f.hasNeutralColorPoint = true;
    return f;
}

rawrcam::metadata::FrameMetadataSnapshot mainFixture() {
    return fixture(
        "3", "main", 4080, 3064,
        {0.921875f,-0.34375f,-0.109375f,-0.421875f,1.25781f,0.132812f,-0.132812f,0.28125f,0.398438f},
        {1.35156f,-0.703125f,-0.242188f,-0.664062f,1.75781f,-0.0703125f,-0.117188f,0.242188f,0.578125f},
        {1.03906f,0,0,0,1,0,0,0,0.976562f},
        {1.02344f,0,0,0,1,0,0,0,0.992188f},
        {0.539062f,0.351562f,0.078125f,0.179688f,0.960938f,-0.140625f,0.0390625f,-0.421875f,1.21094f},
        {0.554688f,0.15625f,0.257812f,0.203125f,0.6875f,0.117188f,0.078125f,-0.578125f,1.32031f},
        {2.06348f,1,1,1.68262f}, {0.484375f,1,0.594727f});
}

rawrcam::metadata::FrameMetadataSnapshot wideFixture() {
    return fixture(
        "4", "wide", 4096, 3072,
        {0.945312f,-0.351562f,-0.132812f,-0.476562f,1.32812f,0.117188f,-0.132812f,0.289062f,0.4375f},
        {1.53125f,-0.835938f,-0.351562f,-0.632812f,1.71875f,-0.0625f,-0.0625f,0.179688f,0.648438f},
        {0.796875f,0,0,0,1,0,0,0,0.976562f},
        {1.13281f,0,0,0,1,0,0,0,0.671875f},
        {0.523438f,0.3125f,0.125f,0.1875f,0.898438f,-0.0859375f,0.0234375f,-0.375f,1.17188f},
        {0.5f,0.195312f,0.273438f,0.171875f,0.710938f,0.117188f,0,-0.429688f,1.25781f},
        {2.41406f,1,1,1.55176f}, {0.414062f,1,0.644531f});
}

rawrcam::metadata::FrameMetadataSnapshot teleFixture() {
    return fixture(
        "5", "tele", 4080, 3072,
        {1.14062f,-0.46875f,-0.179688f,-0.3125f,1.30469f,-0.0078125f,-0.046875f,0.234375f,0.398438f},
        {1.60156f,-0.898438f,-0.367188f,-0.5625f,1.67188f,-0.15625f,-0.0234375f,0.148438f,0.625f},
        {0.875f,0,0,0,1,0,0,0,0.96875f},
        {1.19531f,0,0,0,1,0,0,0,0.65625f},
        {0.429688f,0.320312f,0.210938f,0.109375f,0.835938f,0.0546875f,-0.015625f,-0.335938f,1.17969f},
        {0.460938f,0.210938f,0.289062f,0.140625f,0.71875f,0.148438f,-0.0390625f,-0.40625f,1.27344f},
        {2.06738f,1,1,1.57812f}, {0.483398f,1,0.633789f});
}

void assertNeutralInvariant(const rawrcam::metadata::FrameMetadataSnapshot& frame,
                            const rawrcam::color::FrameColorTransform& state) {
    const std::array<float,3> wbNeutral{
        frame.colorCorrectionGainsRggb[0] * frame.neutralColorPoint[0],
        0.5f * (frame.colorCorrectionGainsRggb[1] + frame.colorCorrectionGainsRggb[2]) * frame.neutralColorPoint[1],
        frame.colorCorrectionGainsRggb[3] * frame.neutralColorPoint[2]};
    const auto rgb = rawrcam::color::math::multiply(state.cameraToLinearSrgbRowMajor, wbNeutral);
    const float mean = (rgb[0]+rgb[1]+rgb[2])/3.0f;
    assert(std::abs(rgb[0]-mean) < 0.035f);
    assert(std::abs(rgb[1]-mean) < 0.035f);
    assert(std::abs(rgb[2]-mean) < 0.035f);
}

} // namespace

int main() {
    using namespace rawrcam::color;

    assert(near(math::reciprocalTemperatureWeight1(6504,6504,2856), 1.0f, 1e-6f));
    assert(near(math::reciprocalTemperatureWeight1(2856,6504,2856), 0.0f, 1e-6f));
    assert(near(math::correlatedColorTemperatureKelvin({0.3127f,0.3290f,true}), 6504.0f, 250.0f));
    assert(near(math::correlatedColorTemperatureKelvin({0.44757f,0.40745f,true}), 2856.0f, 180.0f));

    const auto main = mainFixture();
    const auto wide = wideFixture();
    const auto tele = teleFixture();
    const auto mainState = deriveFrameColorTransform(main, PreviewColorMode::Auto);
    const auto wideState = deriveFrameColorTransform(wide, PreviewColorMode::Auto);
    const auto teleState = deriveFrameColorTransform(tele, PreviewColorMode::Auto);

    for (const auto* state : {&mainState, &wideState, &teleState}) {
        assert(state->source == "DUAL_ILLUMINANT_FORWARD_POST_WB");
        assert(state->hasEstimatedWhite);
        assert(state->estimatedCctKelvin > 2500.0f && state->estimatedCctKelvin < 8000.0f);
        assert(state->calibrationWeight1 >= 0.0f && state->calibrationWeight1 <= 1.0f);
    }

    // Real audit fixtures: main ~5900K; wide/tele ~5550K. Wide/tele must not
    // collapse to D65 endpoint 1 as r2.13 did.
    assert(mainState.estimatedCctKelvin > 5400.0f && mainState.estimatedCctKelvin < 6504.0f);
    assert(wideState.estimatedCctKelvin > 5000.0f && wideState.estimatedCctKelvin < 6200.0f);
    assert(teleState.estimatedCctKelvin > 5000.0f && teleState.estimatedCctKelvin < 6200.0f);
    assert(wideState.calibrationWeight1 < 0.95f);
    assert(teleState.calibrationWeight1 < 0.95f);

    assertNeutralInvariant(main, mainState);
    assertNeutralInvariant(wide, wideState);
    assertNeutralInvariant(tele, teleState);

    // Manual WB (AWB off): the requested gains are the white. The HAL still
    // reports its own neutral point, which must not be used: a raw that the
    // manual gains make neutral has to render neutral.
    {
        auto manual = wide;
        manual.awbMode = 0;
        manual.colorCorrectionGainsRggb = {2.0f, 1, 1, 1.85f};  // differs from 1/neutral
        const auto manualState = deriveFrameColorTransform(manual, PreviewColorMode::Auto);
        const auto rgb = math::multiply(manualState.cameraToLinearSrgbRowMajor, std::array<float,3>{1, 1, 1});
        const float mean = (rgb[0]+rgb[1]+rgb[2])/3.0f;
        assert(std::abs(rgb[0]-mean) < 0.035f);
        assert(std::abs(rgb[1]-mean) < 0.035f);
        assert(std::abs(rgb[2]-mean) < 0.035f);
    }

    const auto f1 = deriveFrameColorTransform(wide, PreviewColorMode::Forward1);
    const auto f2 = deriveFrameColorTransform(wide, PreviewColorMode::Forward2);
    assert(f1.source == "FORWARD_MATRIX1_POST_WB_DIAGNOSTIC");
    assert(f2.source == "FORWARD_MATRIX2_POST_WB_DIAGNOSTIC");
    return 0;
}
