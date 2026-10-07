#include "camera/CameraControlCapabilities.h"

#include <camera/NdkCameraMetadataTags.h>

#include <algorithm>
#include <optional>
#include <sstream>

#include "camera/Camera2PriorityCompat.h"
#include "camera/CameraAePriority.h"

namespace rawrcam::camera {
namespace {

std::optional<ACameraMetadata_const_entry> entry(const ACameraMetadata* metadata, uint32_t tag) {
    if (!metadata) return std::nullopt;
    ACameraMetadata_const_entry value{};
    if (ACameraMetadata_getConstEntry(metadata, tag, &value) != ACAMERA_OK) return std::nullopt;
    return value;
}

bool containsU8(const std::optional<ACameraMetadata_const_entry>& e, uint8_t value) {
    if (!e || !e->data.u8) return false;
    for (uint32_t i = 0; i < e->count; ++i) {
        if (e->data.u8[i] == value) return true;
    }
    return false;
}

void readWbMatrix(const ACameraMetadata* characteristics, uint32_t tag,
                  rawrcam::metadata::Matrix3x3& out) {
    const auto e = entry(characteristics, tag);
    if (!e || !e->data.r || e->count < 9) return;
    for (size_t i = 0; i < 9; ++i) {
        const auto& r = e->data.r[i];
        if (r.denominator == 0) return;
        out.rowMajor[i] = static_cast<float>(r.numerator) / static_cast<float>(r.denominator);
    }
    out.valid = true;
}

int32_t readIlluminant(const ACameraMetadata* characteristics, uint32_t tag) {
    const auto e = entry(characteristics, tag);
    if (!e || e->count == 0) return -1;
    return e->data.u8 ? static_cast<int32_t>(e->data.u8[0]) : -1;
}

}  // namespace

CameraControlState readInitialCameraControlState(const ACameraMetadata* characteristics, const LensRoute& route,
                                                 uint64_t generation) {
    CameraControlState state{};
    state.capabilities.generation = generation;
    state.capabilities.cameraId = route.cameraId;
    state.capabilities.lensId = route.lensId;
    state.capabilities.rawWidth = static_cast<int32_t>(route.stream.width);
    state.capabilities.rawHeight = static_cast<int32_t>(route.stream.height);

    if (const auto e = entry(characteristics, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE);
        e && e->data.i32 && e->count >= 2) {
        state.capabilities.sensitivityMin = e->data.i32[0];
        state.capabilities.sensitivityMax = e->data.i32[1];
    }
    if (const auto e = entry(characteristics, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE);
        e && e->data.i64 && e->count >= 2) {
        state.capabilities.exposureTimeMinNs = e->data.i64[0];
        state.capabilities.exposureTimeMaxNs = e->data.i64[1];
    }
    if (const auto e = entry(characteristics, ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION);
        e && e->data.i64 && e->count >= 1) {
        state.capabilities.maxFrameDurationNs = e->data.i64[0];
    }

    // Pick the broadest advertised normal-AE FPS range for Camera2 priority
    // modes. TEMPLATE_PREVIEW commonly defaults to 30/30; that silently caps
    // shutter-priority exposure near 1/30 even though the new priority mode
    // promises application ownership of SENSOR_EXPOSURE_TIME. Prefer a range
    // whose upper bound is <=30 fps (normal preview) and whose lower bound is
    // as small as possible, preserving headroom for long priority exposures.
    if (const auto e = entry(characteristics, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        e && e->data.i32 && e->count >= 2) {
        bool havePreferred = false;
        int32_t bestMin = 0;
        int32_t bestMax = 0;
        for (uint32_t i = 0; i + 1 < e->count; i += 2) {
            const int32_t lo = e->data.i32[i];
            const int32_t hi = e->data.i32[i + 1];
            if (lo <= 0 || hi < lo) continue;
            state.capabilities.aeTargetFpsRanges.push_back({lo, hi});
            const bool preferred = hi <= 30;
            if (bestMin == 0 || (preferred && !havePreferred) ||
                (preferred == havePreferred && (lo < bestMin || (lo == bestMin && hi > bestMax)))) {
                havePreferred = preferred;
                bestMin = lo;
                bestMax = hi;
            }
        }
        state.capabilities.priorityAeTargetFpsMin = bestMin;
        state.capabilities.priorityAeTargetFpsMax = bestMax;
    }
    if (const auto e = entry(characteristics, ACAMERA_CONTROL_AE_COMPENSATION_RANGE);
        e && e->data.i32 && e->count >= 2) {
        state.capabilities.evMinSteps = e->data.i32[0];
        state.capabilities.evMaxSteps = e->data.i32[1];
    }
    if (const auto e = entry(characteristics, ACAMERA_CONTROL_AE_COMPENSATION_STEP); e && e->data.r && e->count >= 1) {
        state.capabilities.evStepNumerator = e->data.r[0].numerator;
        state.capabilities.evStepDenominator = std::max(1, e->data.r[0].denominator);
    }

    // AE antibanding modes. Missing tag implies the full set (OFF/50/60/AUTO);
    // per the Camera2 docs AUTO is default when available, else 50/60.
    if (const auto e = entry(characteristics, ACAMERA_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES);
        e && e->data.u8 && e->count >= 1) {
        for (uint32_t i = 0; i < e->count; ++i) {
            const uint8_t mode = e->data.u8[i];
            if (mode <= 3 && std::find(state.capabilities.supportedAntibandingModes.begin(),
                                       state.capabilities.supportedAntibandingModes.end(),
                                       mode) == state.capabilities.supportedAntibandingModes.end()) {
                state.capabilities.supportedAntibandingModes.push_back(mode);
            }
        }
    }
    if (state.capabilities.supportedAntibandingModes.empty()) {
        state.capabilities.supportedAntibandingModes = {0, 1, 2, 3};
    }

    const auto requestCaps = entry(characteristics, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES);
    state.capabilities.manualExposureSupported =
        containsU8(requestCaps, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);

    const auto aePriorityModes = entry(characteristics, camera2_priority::kAeAvailablePriorityModesTag);
    state.capabilities.shutterPrioritySupported =
        containsU8(aePriorityModes, camera2_priority::kSensorExposureTimePriority);
    state.capabilities.isoPrioritySupported = containsU8(aePriorityModes, camera2_priority::kSensorSensitivityPriority);
    state.capabilities.softwarePrioritySupported =
        state.capabilities.manualExposureSupported &&
        !(state.capabilities.shutterPrioritySupported && state.capabilities.isoPrioritySupported);

    // White balance: gate presets on AWB_AVAILABLE_MODES and manual gains on
    // COLOR_CORRECTION TRANSFORM_MATRIX. Missing tags fall back to Auto-only /
    // manual-supported (a RAW-capable device without the tag is still expected
    // to honor TRANSFORM_MATRIX; the request echo + result validation confirm).
    if (const auto e = entry(characteristics, ACAMERA_CONTROL_AWB_AVAILABLE_MODES);
        e && e->data.u8 && e->count >= 1) {
        for (uint32_t i = 0; i < e->count; ++i) {
            const uint8_t mode = e->data.u8[i];
            if ((mode >= 1 && mode <= 8) &&
                std::find(state.capabilities.supportedAwbModes.begin(),
                          state.capabilities.supportedAwbModes.end(),
                          mode) == state.capabilities.supportedAwbModes.end()) {
                state.capabilities.supportedAwbModes.push_back(mode);
            }
        }
    }
    if (state.capabilities.supportedAwbModes.empty()) {
        state.capabilities.supportedAwbModes.push_back(ACAMERA_CONTROL_AWB_MODE_AUTO);
    }
    // The NDK exposes no COLOR_CORRECTION available-modes tag; TRANSFORM_MATRIX
    // (FULL devices always include it) is implied by the MANUAL_SENSOR
    // capability this RAW pipeline already requires for manual exposure. The
    // request echo and result gains confirm at runtime.
    state.capabilities.manualGainsSupported = state.capabilities.manualExposureSupported;

    if (const auto e = entry(characteristics, ACAMERA_CONTROL_MAX_REGIONS); e && e->data.i32 && e->count >= 3) {
        state.capabilities.maxAeRegions = e->data.i32[0];
        state.capabilities.maxAfRegions = e->data.i32[2];
    }

    const auto afModes = entry(characteristics, ACAMERA_CONTROL_AF_AVAILABLE_MODES);
    const bool hasAutoAf = containsU8(afModes, ACAMERA_CONTROL_AF_MODE_AUTO) ||
                           containsU8(afModes, ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE);
    state.capabilities.tapAfSupported = state.capabilities.maxAfRegions > 0 && hasAutoAf;

    // Face detection for face-priority AF. FULL carries scores; SIMPLE only
    // rectangles. Either is usable (SIMPLE faces steer with equal weight).
    const auto faceModes = entry(characteristics, ACAMERA_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES);
    state.capabilities.faceDetectFullMode = containsU8(faceModes, ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL);
    const bool faceSimple = containsU8(faceModes, ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE);
    state.capabilities.faceDetectSupported = state.capabilities.faceDetectFullMode || faceSimple;

    float minimumFocusDistance = 0.0f;
    if (const auto e = entry(characteristics, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE);
        e && e->data.f && e->count >= 1) {
        minimumFocusDistance = e->data.f[0];
    }
    state.capabilities.minimumFocusDistance = std::max(0.0f, minimumFocusDistance);
    state.capabilities.manualFocusSupported =
        minimumFocusDistance > 0.0f && containsU8(afModes, ACAMERA_CONTROL_AF_MODE_OFF);

    if (const auto e = entry(characteristics, ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION);
        e && e->data.u8 && e->count >= 1) {
        state.capabilities.focusDistanceReadoutTrustworthy =
            e->data.u8[0] != ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED;
    }

    state.capabilities.oisSupported =
        containsU8(entry(characteristics, ACAMERA_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION),
                   ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON);

    // Static color calibration for the calibrated AUTO WB display readout
    // (neutral -> CCT through the device matrices). Absent tags leave the
    // calibration invalid and the UI falls back to the gains inverse.
    auto& wbCal = state.capabilities.wbDisplayCalibration;
    wbCal.referenceIlluminant1 = readIlluminant(characteristics, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1);
    wbCal.referenceIlluminant2 = readIlluminant(characteristics, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2);
    readWbMatrix(characteristics, ACAMERA_SENSOR_COLOR_TRANSFORM1, wbCal.colorTransform1);
    readWbMatrix(characteristics, ACAMERA_SENSOR_COLOR_TRANSFORM2, wbCal.colorTransform2);
    readWbMatrix(characteristics, ACAMERA_SENSOR_CALIBRATION_TRANSFORM1, wbCal.calibrationTransform1);
    readWbMatrix(characteristics, ACAMERA_SENSOR_CALIBRATION_TRANSFORM2, wbCal.calibrationTransform2);
    readWbMatrix(characteristics, ACAMERA_SENSOR_FORWARD_MATRIX1, wbCal.forwardMatrix1);
    readWbMatrix(characteristics, ACAMERA_SENSOR_FORWARD_MATRIX2, wbCal.forwardMatrix2);

    state.exposureMode = ExposureControlMode::Auto;
    state.focusMode = FocusControlMode::Continuous;
    state.requestedSensitivity =
        std::clamp(100, state.capabilities.sensitivityMin,
                   std::max(state.capabilities.sensitivityMin, state.capabilities.sensitivityMax));
    constexpr int64_t kTenMillisecondsNs = 10000000LL;
    state.requestedExposureTimeNs =
        std::clamp(kTenMillisecondsNs, state.capabilities.exposureTimeMinNs,
                   std::max(state.capabilities.exposureTimeMinNs, state.capabilities.exposureTimeMaxNs));
    state.requestedEvSteps = std::clamp(0, state.capabilities.evMinSteps, state.capabilities.evMaxSteps);
    state.requestedManualFocusNormalized = 1.0f;
    state.autoMinFps = 15;
    return state;
}

std::string describeCameraControlCapabilities(const CameraControlState& state) {
    const auto& c = state.capabilities;
    std::ostringstream out;
    out << "CAMERA_CONTROL_CAPS cameraId=" << c.cameraId << " generation=" << c.generation
        << " isoRange=" << c.sensitivityMin << ".." << c.sensitivityMax << " exposureNsRange=" << c.exposureTimeMinNs
        << ".." << c.exposureTimeMaxNs << " priorityAeFps=" << c.priorityAeTargetFpsMin << ".."
        << c.priorityAeTargetFpsMax << " evSteps=" << c.evMinSteps << ".." << c.evMaxSteps
        << " evStep=" << c.evStepNumerator << '/' << c.evStepDenominator
         << " manualExposure=" << c.manualExposureSupported << " shutterPriority=" << c.shutterPrioritySupported
         << " isoPriority=" << c.isoPrioritySupported << " softPriority=" << c.softwarePrioritySupported << " tapAf=" << c.tapAfSupported
         << " faceDetect=" << c.faceDetectSupported << (c.faceDetectFullMode ? "(full)" : "(simple)")
          << " manualFocus=" << c.manualFocusSupported << " focusCalibrated=" << c.focusDistanceReadoutTrustworthy
         << " ois=" << c.oisSupported << " awbModes=[";
    for (size_t i = 0; i < c.supportedAwbModes.size(); ++i) {
        if (i) out << ',';
        out << static_cast<int>(c.supportedAwbModes[i]);
    }
    out << "] manualGains=" << c.manualGainsSupported;
    return out.str();
}

}  // namespace rawrcam::camera
