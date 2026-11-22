#include "color/ColorCalibration.h"

#include <cmath>
#include <optional>
#include <sstream>

#include "color/ColorMath.h"

namespace rawrcam::color {
namespace {
using math::Matrix3;
using math::Vec3;

bool nearlyIdentity(const Matrix3& m) {
    const Matrix3 id = math::identity3();
    for (size_t i = 0; i < 9; ++i)
        if (std::abs(m[i] - id[i]) > 1.0e-4f) return false;
    return true;
}

Vec3 frameCameraNeutral(const metadata::FrameMetadataSnapshot& f) {
    // With AWB off (Manual WB) the render's white is the requested gains. The
    // HAL's neutral point there is its own AWB guess, not our white; using it
    // left a colour cast that ignored the Manual WB. Same rule as AsShotNeutral
    // in DngMetadataAdapter, so the JPEG matches the DNG.
    const bool manualWb = f.awbMode == 0;
    if (f.hasNeutralColorPoint && !manualWb) {
        return {f.neutralColorPoint[0], f.neutralColorPoint[1], f.neutralColorPoint[2]};
    }
    const float greenGain = 0.5f * (f.colorCorrectionGainsRggb[1] + f.colorCorrectionGainsRggb[2]);
    const Vec3 gains{f.colorCorrectionGainsRggb[0], greenGain, f.colorCorrectionGainsRggb[3]};
    return {gains[0] > 1.0e-8f ? 1.0f / gains[0] : 1.0f, gains[1] > 1.0e-8f ? 1.0f / gains[1] : 1.0f,
            gains[2] > 1.0e-8f ? 1.0f / gains[2] : 1.0f};
}

Vec3 rawPreviewRgbGains(const std::array<float, 4>& wb) { return {wb[0], 0.5f * (wb[1] + wb[2]), wb[3]}; }

struct DualIlluminantSolution {
    bool valid = false;
    math::Xy whiteXy{};
    float cctKelvin = 0.0f;
    float weight1 = 1.0f;
    Matrix3 cameraToLinearSrgbAfterWb = math::identity3();
};

std::optional<DualIlluminantSolution> solveDualIlluminant(const metadata::FrameMetadataSnapshot& f,
                                                          const std::array<float, 4>& wb) {
    if (!f.cameraContext) return std::nullopt;
    const auto& c = f.cameraContext->color;
    if (!c.colorTransform1.valid || !c.colorTransform2.valid || !c.calibrationTransform1.valid ||
        !c.calibrationTransform2.valid || !c.forwardMatrix1.valid || !c.forwardMatrix2.valid)
        return std::nullopt;

    const auto t1 = math::referenceIlluminantCctKelvin(c.referenceIlluminant1);
    const auto t2 = math::referenceIlluminantCctKelvin(c.referenceIlluminant2);
    if (!t1 || !t2) return std::nullopt;

    const Vec3 neutral = frameCameraNeutral(f);
    math::Xy white{0.34567f, 0.35850f, true};  // D50 starting point, matching DNG practice.
    float weight1 = 1.0f;

    for (int pass = 0; pass < 30; ++pass) {
        const float temperature = math::correlatedColorTemperatureKelvin(white);
        if (!(temperature > 0.0f)) return std::nullopt;
        weight1 = math::reciprocalTemperatureWeight1(temperature, *t1, *t2);

        const Matrix3 colorMatrix = math::interpolate(c.colorTransform1.rowMajor, c.colorTransform2.rowMajor, weight1);
        const Matrix3 calibration =
            math::interpolate(c.calibrationTransform1.rowMajor, c.calibrationTransform2.rowMajor, weight1);
        // Camera2 ColorTransformN is XYZ -> reference camera space;
        // CalibrationTransformN is reference camera -> individual camera.
        const Matrix3 xyzToIndividualCamera = math::multiply(calibration, colorMatrix);
        const auto individualToXyz = math::inverse(xyzToIndividualCamera);
        if (!individualToXyz) return std::nullopt;
        const math::Xy next = math::xyzToXy(math::multiply(*individualToXyz, neutral));
        if (!next.valid) return std::nullopt;
        if (std::abs(next.x - white.x) + std::abs(next.y - white.y) < 1.0e-7f) {
            white = next;
            break;
        }
        if (pass == 29) {
            white.x = 0.5f * (white.x + next.x);
            white.y = 0.5f * (white.y + next.y);
        } else {
            white = next;
        }
    }

    const float cct = math::correlatedColorTemperatureKelvin(white);
    if (!(cct > 0.0f)) return std::nullopt;
    weight1 = math::reciprocalTemperatureWeight1(cct, *t1, *t2);

    const Matrix3 forward = math::interpolate(c.forwardMatrix1.rowMajor, c.forwardMatrix2.rowMajor, weight1);
    const Matrix3 calibration =
        math::interpolate(c.calibrationTransform1.rowMajor, c.calibrationTransform2.rowMajor, weight1);
    const auto individualToReference = math::inverse(calibration);
    if (!individualToReference) return std::nullopt;

    const Vec3 referenceNeutral = math::multiply(*individualToReference, neutral);
    const auto inverseReferenceNeutral = math::inverseDiagonal(referenceNeutral);
    const Vec3 appliedRgbGains = rawPreviewRgbGains(wb);
    const auto inverseAppliedWb = math::inverseDiagonal(appliedRgbGains);
    if (!inverseReferenceNeutral || !inverseAppliedWb) return std::nullopt;

    // DNG/Camera2 forward-matrix path for an individual camera:
    //   unbalanced individual camera RGB -> D50 XYZ
    //     FM * inv(diag(referenceNeutral)) * inv(CameraCalibration)
    // raw_preview has already multiplied the individual camera RGB by the
    // supplied WB gains, so explicitly compose the inverse of those gains.
    // This prevents CameraCalibration/WB from being applied a second time.
    const Matrix3 cameraToD50AfterWb = math::multiply(
        forward, math::multiply(*inverseReferenceNeutral, math::multiply(*individualToReference, *inverseAppliedWb)));

    const Matrix3 d50ToLinearSrgb = math::multiply(math::kXyzD65ToLinearSrgb, math::kD50ToD65Bradford);
    DualIlluminantSolution out{};
    out.valid = true;
    out.whiteXy = white;
    out.cctKelvin = cct;
    out.weight1 = weight1;
    out.cameraToLinearSrgbAfterWb = math::multiply(d50ToLinearSrgb, cameraToD50AfterWb);
    return out;
}

Matrix3 endpointForwardToLinearSrgbAfterWb(const metadata::FrameMetadataSnapshot& f, const std::array<float, 4>& wb,
                                           bool second) {
    if (!f.cameraContext) return math::identity3();
    const auto& c = f.cameraContext->color;
    const auto& forward = second ? c.forwardMatrix2 : c.forwardMatrix1;
    const auto& calibration = second ? c.calibrationTransform2 : c.calibrationTransform1;
    if (!forward.valid || !calibration.valid) return math::identity3();
    const auto individualToReference = math::inverse(calibration.rowMajor);
    if (!individualToReference) return math::identity3();

    const Vec3 neutral = frameCameraNeutral(f);
    const Vec3 referenceNeutral = math::multiply(*individualToReference, neutral);
    const auto invReferenceNeutral = math::inverseDiagonal(referenceNeutral);
    const auto invAppliedWb = math::inverseDiagonal(rawPreviewRgbGains(wb));
    if (!invReferenceNeutral || !invAppliedWb) return math::identity3();

    const Matrix3 cameraToD50AfterWb = math::multiply(
        forward.rowMajor, math::multiply(*invReferenceNeutral, math::multiply(*individualToReference, *invAppliedWb)));
    return math::multiply(math::multiply(math::kXyzD65ToLinearSrgb, math::kD50ToD65Bradford), cameraToD50AfterWb);
}

}  // namespace

PreviewColorMode parsePreviewColorMode(const std::string& mode) {
    if (mode == "result") return PreviewColorMode::Result;
    if (mode == "forward1") return PreviewColorMode::Forward1;
    if (mode == "forward2") return PreviewColorMode::Forward2;
    if (mode == "identity") return PreviewColorMode::Identity;
    if (mode == "neutral_wb") return PreviewColorMode::NeutralWb;
    return PreviewColorMode::Auto;
}

FrameColorTransform deriveFrameColorTransform(const metadata::FrameMetadataSnapshot& f, PreviewColorMode mode) {
    FrameColorTransform out{};
    if (!f.cameraContext) {
        out.source = "MISSING_CAMERA_CONTEXT";
        return out;
    }

    out.baselineWbRggb =
        mode == PreviewColorMode::NeutralWb ? std::array<float, 4>{1, 1, 1, 1} : f.colorCorrectionGainsRggb;

    if (mode == PreviewColorMode::Identity) {
        out.cameraToLinearSrgbRowMajor = math::identity3();
        out.source = "IDENTITY_DIAGNOSTIC";
        return out;
    }

    if (mode == PreviewColorMode::Result && f.colorCorrectionTransform.valid &&
        !nearlyIdentity(f.colorCorrectionTransform.rowMajor)) {
        out.cameraToLinearSrgbRowMajor = f.colorCorrectionTransform.rowMajor;
        out.source = "COLOR_CORRECTION_TRANSFORM_RESULT_DIAGNOSTIC";
        return out;
    }

    if (mode == PreviewColorMode::Forward1 || mode == PreviewColorMode::NeutralWb) {
        out.cameraToLinearSrgbRowMajor = endpointForwardToLinearSrgbAfterWb(f, out.baselineWbRggb, false);
        out.source = "FORWARD_MATRIX1_POST_WB_DIAGNOSTIC";
        return out;
    }
    if (mode == PreviewColorMode::Forward2) {
        out.cameraToLinearSrgbRowMajor = endpointForwardToLinearSrgbAfterWb(f, out.baselineWbRggb, true);
        out.source = "FORWARD_MATRIX2_POST_WB_DIAGNOSTIC";
        return out;
    }

    if (const auto solution = solveDualIlluminant(f, out.baselineWbRggb); solution && solution->valid) {
        out.cameraToLinearSrgbRowMajor = solution->cameraToLinearSrgbAfterWb;
        out.source = "DUAL_ILLUMINANT_FORWARD_POST_WB";
        out.hasEstimatedWhite = true;
        out.estimatedWhiteX = solution->whiteXy.x;
        out.estimatedWhiteY = solution->whiteXy.y;
        out.estimatedCctKelvin = solution->cctKelvin;
        out.calibrationWeight1 = solution->weight1;
        return out;
    }

    // Conservative fallback: endpoint 1 using the same post-WB semantics.
    out.cameraToLinearSrgbRowMajor = endpointForwardToLinearSrgbAfterWb(f, out.baselineWbRggb, false);
    out.source = "FORWARD_MATRIX1_POST_WB_FALLBACK";
    return out;
}

std::string describe(const FrameColorTransform& state, const metadata::FrameMetadataSnapshot& frameMetadata) {
    std::ostringstream s;
    s << "PREVIEW_COLOR_STATE generation="
      << (frameMetadata.cameraContext ? frameMetadata.cameraContext->cameraContextGeneration : 0)
      << " frameOrdinal=" << frameMetadata.frameOrdinal << " timestampNs=" << frameMetadata.timestampNs
      << " cameraId=" << (frameMetadata.cameraContext ? frameMetadata.cameraContext->cameraId : std::string("<null>"))
      << " source=" << state.source << " wbAppliedByRawPreview=" << state.baselineWbAppliedByRawPreview << " wbRggb=["
      << state.baselineWbRggb[0] << ',' << state.baselineWbRggb[1] << ',' << state.baselineWbRggb[2] << ','
      << state.baselineWbRggb[3] << ']';
    if (state.hasEstimatedWhite) {
        s << " estimatedWhiteXY=[" << state.estimatedWhiteX << ',' << state.estimatedWhiteY << ']'
          << " estimatedCctK=" << state.estimatedCctKelvin << " calibrationWeight1=" << state.calibrationWeight1;
    }
    s << " sensorToLinearSrgbRowMajor=[";
    for (size_t i = 0; i < state.cameraToLinearSrgbRowMajor.size(); ++i) {
        if (i) s << ',';
        s << state.cameraToLinearSrgbRowMajor[i];
    }
    s << ']';
    return s.str();
}

}  // namespace rawrcam::color
