#include "capture/single/SingleFrameCaptureContextBuilder.h"

#include <algorithm>
#include <cmath>

#include "color/ColorMath.h"
#include "color/FilmExposure.h"
#include "color/WhiteBalance.h"
#include "develop/DevelopContextBuilder.h"
#include "develop/render/DenoiseProfile.h"
#include "geometry/OrientationTransform.h"

namespace rawrcam::capture {
const char* singleFrameDemosaicName(develop::DemosaicAlgorithm algorithm) noexcept {
    switch (algorithm) {
        case develop::DemosaicAlgorithm::Rcd:
            return "RCD";
        case develop::DemosaicAlgorithm::Vng4:
            return "VNG4";
        case develop::DemosaicAlgorithm::DualRcdVng4:
            return "RCD+VNG4";
        case develop::DemosaicAlgorithm::Amaze:
            return "AMaZE";
        case develop::DemosaicAlgorithm::DualAmazeVng4:
            return "AMaZE+VNG4";
    }
    return "UNKNOWN";
}
develop::rendered::RenderedStillContext makeSingleFrameRenderContext(
    const rawrcam::imaging::RawSnapshot& captured, const tonemap::TonemapParams& tonemapParams, float aePostGain,
    uint32_t presentationQuarterTurns, bool filmEnabled, const spektrafilm_native::FilmLook& filmLook,
    const JpegCaptureRequest* jpeg, const CaptureDiagnostic& emit) {
    const auto& colorState = captured.colorState;
    const rawrcam::color::math::Vec3 wb = rawrcam::color::collapseRggbToRgb(colorState.baselineWbRggb);

    develop::rendered::RenderedStillContext rendered{};
    rendered.requestId = captured.requestId;
    rendered.timestampNs = captured.timestampNs;
    rendered.width = captured.width;
    rendered.height = captured.height;
    rendered.presentationQuarterTurns = presentationQuarterTurns;
    rendered.whiteBalanceRgb = {wb[0], wb[1], wb[2]};
    // Still demosaicers emit pre-WB camera RGB. The rendered-still stage now
    // applies WB explicitly, then configurable FCC, so Tonemap receives the existing
    // post-WB camera-to-working transform without re-composing WB here.
    rendered.cameraToWorkingColumnMajor =
        rawrcam::color::math::cameraToWorkingColumnMajor(colorState.cameraToLinearSrgbRowMajor);
    rendered.tonemapParams = tonemapParams;
    rendered.tonemapParams.aePostGain = aePostGain;
    if (jpeg) {
        rendered.ultraHdrEnabled = jpeg->output.ultraHdr.enabled;
        rendered.gainmapParams = jpeg->output.ultraHdr.toGainmapParams();
        // HDR gainmap input is camera RGB: same calibrated matrix the
        // tonemap/film path uses (never the AP1->sRGB default).
        rendered.gainmapCstRowMajor = colorState.cameraToLinearSrgbRowMajor;
        rendered.hasGainmapCst = true;
        // Scene-exposure match: the SDR base contains the capture exposure
        // gain but the HDR tap is pre-exposure sensor linear. Without this
        // the map goes flat (~1x) in clipped highlights. Tonemap path
        // mirrors renderExposureEV = log2(aePostGain) + exposureEV; film
        // path mirrors its folded EV exactly like the film record.
        if (filmEnabled) {
            rendered.gainmapParams.hdrExposure = std::exp2(rawrcam::color::filmExposureEv(filmLook, aePostGain));
        } else {
            rendered.gainmapParams.hdrExposure = std::max(aePostGain, 1.0e-6f) * std::exp2(tonemapParams.exposureEV);
        }
    }
    // Film still state, snapshotted with the frame: enabled flag, full
    // look, and the sensor->linear-sRGB matrix the film input needs.
    rendered.filmEnabled = filmEnabled;
    rendered.filmTiled =
        uint64_t(rendered.width) * rendered.height >= 12000000u && (!filmLook.grainEnabled || filmLook.grainModel != 2);
    rendered.filmLook = filmLook;
    rendered.sensorToLinearSrgb = colorState.cameraToLinearSrgbRowMajor;
    if (jpeg) {
        develop::applyDevelopSettings(rendered, jpeg->develop);
        // Profiled denoise model: green S,O from the frame noise profile
        // (CFA-aware channel pick) + the demosaic normalization. Missing
        // or invalid profiles force strength 0 (never hallucinate).
        rendered.denoiseStrength = jpeg->develop.denoiseStrength;
        rendered.denoiseDetail = jpeg->develop.denoiseDetail;
        rendered.denoiseForceY = jpeg->develop.denoiseForceY;
        rendered.denoiseMaxScale = jpeg->develop.denoiseMaxScale;
        rendered.denoiseNoiseA = 0.0f;
        rendered.denoiseNoiseB = 0.0f;
        if (rendered.denoiseStrength > 0.0f) {
            if (develop::rendered::resolveDenoiseNoise(captured.metadata, rendered.denoiseNoiseA,
                                                       rendered.denoiseNoiseB)) {
                emit("DENOISE_PROFILE requestId=" + std::to_string(captured.requestId) + " strength=" +
                     std::to_string(rendered.denoiseStrength) + " detail=" + std::to_string(rendered.denoiseDetail) +
                     " a=" + std::to_string(rendered.denoiseNoiseA) + " b=" + std::to_string(rendered.denoiseNoiseB));
            } else {
                rendered.denoiseStrength = 0.0f;
            }
        }
        rendered.diagnosticsEnabled = jpeg->develop.pipelineDiagnosticsEnabled;
        rendered.cfaPattern = captured.metadata.cameraContext->rawPreviewCfa;
        rendered.lensShading = develop::highlight::LensShadingMapSnapshot::fromMetadata(
            captured.metadata, jpeg->develop.lensShadingCorrectionEnabled);
        // Undistort calibration is defined on the full pixel array. Apply only
        // when the still frame covers it; merged/reconstructed grids skip.
        const auto& ctx = *captured.metadata.cameraContext;
        const bool geometryMatches =
            captured.width == ctx.geometry.pixelArrayWidth && captured.height == ctx.geometry.pixelArrayHeight;
        const bool calibrationPresent = ctx.lensDistortion.size() == 5 && ctx.lensIntrinsicCalibration.size() == 5;
        rendered.distortionCorrectionEnabled =
            jpeg->develop.distortionCorrectionEnabled && geometryMatches && calibrationPresent;
        if (calibrationPresent) {
            std::copy_n(ctx.lensIntrinsicCalibration.begin(), 5, rendered.lensIntrinsic.begin());
            std::copy_n(ctx.lensDistortion.begin(), 5, rendered.lensDistortion.begin());
            rendered.hasLensCalibration = true;
        }
        if (jpeg->develop.distortionCorrectionEnabled && !rendered.distortionCorrectionEnabled) {
            emit("DISTORTION_CORRECTION_SKIP requestId=" + std::to_string(captured.requestId) +
                 " geometryMatches=" + (geometryMatches ? "true" : "false") +
                 " calibrationPresent=" + (calibrationPresent ? "true" : "false"));
        }
        if (rendered.distortionCorrectionEnabled) {
            emit("DISTORTION_CORRECTION_APPLY requestId=" + std::to_string(captured.requestId) + " k1=" +
                 std::to_string(rendered.lensDistortion[0]) + " k2=" + std::to_string(rendered.lensDistortion[1]));
        }
    }
    return rendered;
}
void applySingleFrameCaptureMetadata(encoding::jpeg::JpegCaptureContext& output, const imaging::RawSnapshot& captured) {
    const auto& camera = *captured.metadata.cameraContext;
    const auto turns =
        geometry::presentationQuarterTurns(camera.geometry.sensorOrientationDegrees, output.deviceRotationDegrees);
    output.exifOrientation = turns == 1u ? 6u : (turns == 2u ? 3u : (turns == 3u ? 8u : 1u));
    output.exposureTimeNs = captured.metadata.exposureTimeNs;
    output.sensitivity = captured.metadata.sensitivity;
    output.aperture = captured.metadata.aperture;
    output.focalLengthMm = captured.metadata.focalLengthMm;
}

}  // namespace rawrcam::capture
