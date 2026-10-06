#include "metadata/CameraMetadataReader.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "metadata/MetadataValidation.h"
#include "geometry/CfaPattern.h"

namespace rawrcam::metadata {
namespace {

std::optional<ACameraMetadata_const_entry> entry(const ACameraMetadata* metadata, uint32_t tag) {
    if (!metadata) return std::nullopt;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(metadata, tag, &e) != ACAMERA_OK) return std::nullopt;
    return e;
}

Matrix3x3 rationalMatrix(const ACameraMetadata* metadata, uint32_t tag) {
    Matrix3x3 out{};
    const auto e = entry(metadata, tag);
    if (!e || !e->data.r || e->count < 9) return out;
    for (size_t i = 0; i < 9; ++i) {
        const auto& r = e->data.r[i];
        if (r.denominator == 0) return Matrix3x3{};
        out.rowMajor[i] = static_cast<float>(r.numerator) / static_cast<float>(r.denominator);
    }
    out.valid = true;
    return out;
}

RectI rect(const ACameraMetadata* metadata, uint32_t tag) {
    RectI out{};
    const auto e = entry(metadata, tag);
    if (!e || !e->data.i32 || e->count < 4) return out;
    out.left = e->data.i32[0];
    out.top = e->data.i32[1];
    out.right = e->data.i32[2];
    out.bottom = e->data.i32[3];
    out.valid = true;
    return out;
}

std::array<float, 4> mapBlackPatternToPhysicalRggb(const std::array<float, 4>& pattern, int32_t cfa) {
    // Camera2 COLOR_FILTER_ARRANGEMENT values coincide with CfaPattern codes
    // (RGGB=0, GRBG=1, GBRG=2, BGGR=3); out-of-range input keeps identity,
    // matching the legacy default arm.
    static_assert(ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB == 0 &&
                      ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GRBG == 1 &&
                      ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG == 2 &&
                      ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR == 3,
                  "Camera2 CFA values must match CfaPattern codes");
    return rawrcam::geometry::reorderRggbByCode(pattern, static_cast<std::uint32_t>(cfa));
}

std::optional<uint32_t> rawPreviewCfa(int32_t cfa) {
    const auto pattern = rawrcam::geometry::cfaPatternFromU32(static_cast<std::uint32_t>(cfa));
    if (!pattern) return std::nullopt;
    return static_cast<uint32_t>(*pattern);
}

int32_t firstI32(const ACameraMetadata* metadata, uint32_t tag, int32_t fallback) {
    const auto e = entry(metadata, tag);
    return e && e->data.i32 && e->count ? e->data.i32[0] : fallback;
}

int32_t firstU8(const ACameraMetadata* metadata, uint32_t tag, int32_t fallback) {
    const auto e = entry(metadata, tag);
    return e && e->data.u8 && e->count ? static_cast<int32_t>(e->data.u8[0]) : fallback;
}

int32_t illuminant(const ACameraMetadata* metadata, uint32_t tag) {
    const auto e = entry(metadata, tag);
    if (!e || e->count == 0) return -1;
    // Camera metadata represents these tags as byte on Android Camera2.
    return e->data.u8 ? static_cast<int32_t>(e->data.u8[0]) : -1;
}

}  // namespace

std::optional<CameraContextMetadata> readCameraContextMetadata(const ACameraMetadata* characteristics,
                                                               const std::string& cameraId, const std::string& lensId,
                                                               uint64_t cameraContextGeneration, uint32_t rawWidth,
                                                               uint32_t rawHeight, std::string* error) {
    if (!characteristics) {
        if (error) *error = "null characteristics";
        return std::nullopt;
    }
    const auto cfaEntry = entry(characteristics, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT);
    const auto blackEntry = entry(characteristics, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN);
    const auto whiteEntry = entry(characteristics, ACAMERA_SENSOR_INFO_WHITE_LEVEL);
    if (!cfaEntry || !cfaEntry->data.u8 || cfaEntry->count < 1 || !blackEntry || !blackEntry->data.i32 ||
        blackEntry->count < 4 || !whiteEntry || !whiteEntry->data.i32 || whiteEntry->count < 1) {
        if (error) *error = "required CFA/black/white metadata missing";
        return std::nullopt;
    }

    CameraContextMetadata out{};
    out.cameraContextGeneration = cameraContextGeneration;
    out.cameraId = cameraId;
    out.lensId = lensId;
    out.camera2Cfa = cfaEntry->data.u8[0];
    const auto mappedCfa = rawPreviewCfa(out.camera2Cfa);
    if (!mappedCfa) {
        if (error) *error = "unsupported CFA " + std::to_string(out.camera2Cfa);
        return std::nullopt;
    }
    out.rawPreviewCfa = *mappedCfa;
    out.lensFacing = firstU8(characteristics, ACAMERA_LENS_FACING, -1);
    out.sensorTimestampRealtime = firstU8(characteristics, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, 0) ==
                                  ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    const std::array<float, 4> blackPattern{
        static_cast<float>(blackEntry->data.i32[0]), static_cast<float>(blackEntry->data.i32[1]),
        static_cast<float>(blackEntry->data.i32[2]), static_cast<float>(blackEntry->data.i32[3])};
    out.baselineBlackLevelPhysicalRggb = mapBlackPatternToPhysicalRggb(blackPattern, out.camera2Cfa);
    out.baselineWhiteLevel = static_cast<float>(whiteEntry->data.i32[0]);

    out.geometry.rawBufferWidth = rawWidth;
    out.geometry.rawBufferHeight = rawHeight;
    if (const auto pixel = entry(characteristics, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE);
        pixel && pixel->data.i32 && pixel->count >= 2) {
        out.geometry.pixelArrayWidth = static_cast<uint32_t>(pixel->data.i32[0]);
        out.geometry.pixelArrayHeight = static_cast<uint32_t>(pixel->data.i32[1]);
    }
    out.geometry.preCorrectionActiveArray = rect(characteristics, ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE);
    out.geometry.activeArray = rect(characteristics, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE);
    out.geometry.sensorOrientationDegrees = firstI32(characteristics, ACAMERA_SENSOR_ORIENTATION, 0);
    if (const auto physical = entry(characteristics, ACAMERA_SENSOR_INFO_PHYSICAL_SIZE);
        physical && physical->data.f && physical->count >= 2) {
        out.sensorPhysicalSizeMm = {physical->data.f[0], physical->data.f[1]};
    }
    if (const auto focal = entry(characteristics, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS);
        focal && focal->data.f && focal->count) {
        out.availableFocalLengthsMm.assign(focal->data.f, focal->data.f + focal->count);
    }
    if (const auto apertures = entry(characteristics, ACAMERA_LENS_INFO_AVAILABLE_APERTURES);
        apertures && apertures->data.f && apertures->count) {
        out.availableApertures.assign(apertures->data.f, apertures->data.f + apertures->count);
    }
    if (const auto shadingSize = entry(characteristics, ACAMERA_LENS_INFO_SHADING_MAP_SIZE);
        shadingSize && shadingSize->data.i32 && shadingSize->count >= 2 && shadingSize->data.i32[0] > 0 &&
        shadingSize->data.i32[1] > 0) {
        out.lensShadingMapWidth = static_cast<uint32_t>(shadingSize->data.i32[0]);
        out.lensShadingMapHeight = static_cast<uint32_t>(shadingSize->data.i32[1]);
    }
    if (const auto boostRange = entry(characteristics, ACAMERA_CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE);
        boostRange && boostRange->data.i32 && boostRange->count >= 2) {
        out.maxPostRawSensitivityBoost = std::max(100, boostRange->data.i32[1]);
    }
    if (const auto shadingApplied = entry(characteristics, ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED);
        shadingApplied && shadingApplied->data.u8 && shadingApplied->count >= 1) {
        out.hasLensShadingApplied = true;
        out.lensShadingApplied = shadingApplied->data.u8[0] != 0;
    }
    if (const auto maxAnalog = entry(characteristics, ACAMERA_SENSOR_MAX_ANALOG_SENSITIVITY);
        maxAnalog && maxAnalog->data.i32 && maxAnalog->count >= 1 && maxAnalog->data.i32[0] > 0) {
        out.maxAnalogSensitivity = maxAnalog->data.i32[0];
    }
    if (const auto greenSplit = entry(characteristics, ACAMERA_SENSOR_GREEN_SPLIT);
        greenSplit && greenSplit->data.f && greenSplit->count >= 1 &&
        std::isfinite(greenSplit->data.f[0])) {
        out.hasGreenSplit = true;
        out.greenSplit = greenSplit->data.f[0];
    }
    if (const auto opticalBlack = entry(characteristics, ACAMERA_SENSOR_OPTICAL_BLACK_REGIONS);
        opticalBlack && opticalBlack->data.i32 && opticalBlack->count >= 4) {
        const size_t regions = opticalBlack->count / 4;
        for (size_t i = 0; i < regions && i < 8; ++i) {
            RectI r{};
            r.left = opticalBlack->data.i32[4 * i + 0];
            r.top = opticalBlack->data.i32[4 * i + 1];
            r.right = opticalBlack->data.i32[4 * i + 2];
            r.bottom = opticalBlack->data.i32[4 * i + 3];
            r.valid = r.right > r.left && r.bottom > r.top && r.left >= 0 && r.top >= 0;
            if (r.valid) out.opticalBlackRegions.push_back(r);
        }
    }
    if (const auto distortion = entry(characteristics, ACAMERA_LENS_DISTORTION);
        distortion && distortion->data.f && distortion->count >= 5) {
        bool finite = true;
        for (int i = 0; i < 5; ++i)
            if (!std::isfinite(distortion->data.f[i])) finite = false;
        if (finite) out.lensDistortion.assign(distortion->data.f, distortion->data.f + 5);
    }
    if (const auto intrinsic = entry(characteristics, ACAMERA_LENS_INTRINSIC_CALIBRATION);
        intrinsic && intrinsic->data.f && intrinsic->count >= 5) {
        bool finite = true;
        for (int i = 0; i < 5; ++i)
            if (!std::isfinite(intrinsic->data.f[i])) finite = false;
        if (finite && intrinsic->data.f[0] > 0.0f && intrinsic->data.f[1] > 0.0f)
            out.lensIntrinsicCalibration.assign(intrinsic->data.f, intrinsic->data.f + 5);
    }
    if (const auto flashAvailable = entry(characteristics, ACAMERA_FLASH_INFO_AVAILABLE);
        flashAvailable && flashAvailable->data.u8 && flashAvailable->count >= 1) {
        out.hasFlashInfoAvailable = true;
        out.flashInfoAvailable = flashAvailable->data.u8[0] != 0;
    }

    out.color.referenceIlluminant1 = illuminant(characteristics, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1);
    out.color.referenceIlluminant2 = illuminant(characteristics, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2);
    out.color.colorTransform1 = rationalMatrix(characteristics, ACAMERA_SENSOR_COLOR_TRANSFORM1);
    out.color.colorTransform2 = rationalMatrix(characteristics, ACAMERA_SENSOR_COLOR_TRANSFORM2);
    out.color.calibrationTransform1 = rationalMatrix(characteristics, ACAMERA_SENSOR_CALIBRATION_TRANSFORM1);
    out.color.calibrationTransform2 = rationalMatrix(characteristics, ACAMERA_SENSOR_CALIBRATION_TRANSFORM2);
    out.color.forwardMatrix1 = rationalMatrix(characteristics, ACAMERA_SENSOR_FORWARD_MATRIX1);
    out.color.forwardMatrix2 = rationalMatrix(characteristics, ACAMERA_SENSOR_FORWARD_MATRIX2);
    std::string validationError;
    if (!validate(out, &validationError)) {
        if (error) *error = validationError;
        return std::nullopt;
    }
    return out;
}

std::optional<FrameMetadataSnapshot> readFrameMetadataSnapshot(const ACameraMetadata* result,
                                                               CameraContextMetadataPtr cameraContext,
                                                               uint64_t frameOrdinal, std::string* error) {
    if (!result) {
        if (error) *error = "null capture result";
        return std::nullopt;
    }
    const auto timestampEntry = entry(result, ACAMERA_SENSOR_TIMESTAMP);
    if (!timestampEntry || !timestampEntry->data.i64 || timestampEntry->count < 1) {
        if (error) *error = "capture result missing SENSOR_TIMESTAMP";
        return std::nullopt;
    }

    if (!cameraContext) {
        if (error) *error = "null camera context";
        return std::nullopt;
    }

    FrameMetadataSnapshot out{};
    out.cameraContext = std::move(cameraContext);
    out.frameOrdinal = frameOrdinal;
    out.timestampNs = static_cast<uint64_t>(timestampEntry->data.i64[0]);
    out.blackLevelPhysicalRggb = out.cameraContext->baselineBlackLevelPhysicalRggb;
    out.staticWhiteLevel = out.cameraContext->baselineWhiteLevel;
    out.effectiveWhiteLevel = out.staticWhiteLevel;
    out.effectiveWhiteLevelSource = EffectiveWhiteLevelSource::StaticCharacteristics;

    if (const auto gains = entry(result, ACAMERA_COLOR_CORRECTION_GAINS); gains && gains->data.f && gains->count >= 4) {
        out.colorCorrectionGainsRggb = {gains->data.f[0], gains->data.f[1], gains->data.f[2], gains->data.f[3]};
    }
    if (const auto neutral = entry(result, ACAMERA_SENSOR_NEUTRAL_COLOR_POINT);
        neutral && neutral->data.r && neutral->count >= 3) {
        bool valid = true;
        for (size_t i = 0; i < 3; ++i) {
            const auto& r = neutral->data.r[i];
            if (r.denominator == 0) {
                valid = false;
                break;
            }
            out.neutralColorPoint[i] = static_cast<float>(r.numerator) / static_cast<float>(r.denominator);
        }
        out.hasNeutralColorPoint = valid;
    }
    out.colorCorrectionTransform = rationalMatrix(result, ACAMERA_COLOR_CORRECTION_TRANSFORM);
    out.colorCorrectionMode = firstU8(result, ACAMERA_COLOR_CORRECTION_MODE, -1);
    out.awbMode = firstU8(result, ACAMERA_CONTROL_AWB_MODE, -1);
    out.aeMode = firstU8(result, ACAMERA_CONTROL_AE_MODE, -1);
    out.aeState = firstU8(result, ACAMERA_CONTROL_AE_STATE, -1);
    out.awbState = firstU8(result, ACAMERA_CONTROL_AWB_STATE, -1);
    if (const auto boost = entry(result, ACAMERA_CONTROL_POST_RAW_SENSITIVITY_BOOST);
        boost && boost->data.i32 && boost->count >= 1) {
        out.postRawSensitivityBoost = std::max(100, boost->data.i32[0]);
    }

    if (const auto dynamicBlack = entry(result, ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL);
        dynamicBlack && dynamicBlack->data.f && dynamicBlack->count >= 4) {
        const std::array<float, 4> pattern{dynamicBlack->data.f[0], dynamicBlack->data.f[1], dynamicBlack->data.f[2],
                                           dynamicBlack->data.f[3]};
        out.blackLevelPhysicalRggb = mapBlackPatternToPhysicalRggb(pattern, out.cameraContext->camera2Cfa);
    }
    if (const auto dynamicWhite = entry(result, ACAMERA_SENSOR_DYNAMIC_WHITE_LEVEL);
        dynamicWhite && dynamicWhite->data.i32 && dynamicWhite->count >= 1) {
        out.reportedDynamicWhiteLevel = static_cast<float>(dynamicWhite->data.i32[0]);
        // Preserve the existing app behavior for this iteration so the new stats
        // path observes rather than retunes rendering. The source is explicit so
        // a future DCG-aware effective-white policy can replace this safely.
        out.effectiveWhiteLevel = *out.reportedDynamicWhiteLevel;
        out.effectiveWhiteLevelSource = EffectiveWhiteLevelSource::ReportedDynamic;
    }

    out.scalerCropRegion = rect(result, ACAMERA_SCALER_CROP_REGION);
#if defined(__ANDROID_API__) && __ANDROID_API__ >= 34
    out.rawCropRegion = rect(result, ACAMERA_SCALER_RAW_CROP_REGION);
#endif
    if (const auto exposure = entry(result, ACAMERA_SENSOR_EXPOSURE_TIME);
        exposure && exposure->data.i64 && exposure->count >= 1)
        out.exposureTimeNs = exposure->data.i64[0];
    if (const auto sensitivity = entry(result, ACAMERA_SENSOR_SENSITIVITY);
        sensitivity && sensitivity->data.i32 && sensitivity->count >= 1)
        out.sensitivity = sensitivity->data.i32[0];
    if (const auto aperture = entry(result, ACAMERA_LENS_APERTURE); aperture && aperture->data.f && aperture->count)
        out.aperture = aperture->data.f[0];
    if (const auto focal = entry(result, ACAMERA_LENS_FOCAL_LENGTH); focal && focal->data.f && focal->count)
        out.focalLengthMm = focal->data.f[0];
    if (const auto focus = entry(result, ACAMERA_LENS_FOCUS_DISTANCE); focus && focus->data.f && focus->count)
        out.focusDistanceDiopters = focus->data.f[0];
    out.flashState = firstU8(result, ACAMERA_FLASH_STATE, -1);
    if (const auto hotPixels = entry(result, ACAMERA_STATISTICS_HOT_PIXEL_MAP);
        hotPixels && hotPixels->data.i32 && hotPixels->count >= 2) {
        // int32[2*n] flat x,y pairs. Cap to bound memory on buggy HALs.
        constexpr size_t kMaxHotPixelInts = 4096;
        const size_t count = std::min<size_t>(hotPixels->count, kMaxHotPixelInts) & ~size_t{1};
        if (count >= 2) out.hotPixelMap.assign(hotPixels->data.i32, hotPixels->data.i32 + count);
    }
    if (const auto noise = entry(result, ACAMERA_SENSOR_NOISE_PROFILE); noise && noise->data.d && noise->count)
        out.sensorNoiseProfile.assign(noise->data.d, noise->data.d + noise->count);
    if (const auto shading = entry(result, ACAMERA_STATISTICS_LENS_SHADING_MAP);
        shading && shading->data.f && shading->count && out.cameraContext->lensShadingMapWidth > 0 &&
        out.cameraContext->lensShadingMapHeight > 0) {
        const uint64_t expected = static_cast<uint64_t>(out.cameraContext->lensShadingMapWidth) *
                                  out.cameraContext->lensShadingMapHeight * 4u;
        if (expected == shading->count) {
            out.lensShadingMapWidth = out.cameraContext->lensShadingMapWidth;
            out.lensShadingMapHeight = out.cameraContext->lensShadingMapHeight;
            out.lensShadingMap.assign(shading->data.f, shading->data.f + shading->count);
        }
    }
    std::string validationError;
    if (!validate(out, &validationError)) {
        if (error) *error = validationError;
        return std::nullopt;
    }
    return out;
}

}  // namespace rawrcam::metadata
