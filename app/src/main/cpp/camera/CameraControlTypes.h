#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "camera/CameraCadencePolicy.h"
#include "metadata/CameraContextMetadata.h"

namespace rawrcam::camera {

enum class ExposureControlMode : uint8_t {
    Auto = 0,
    Manual = 1,
    ShutterPriority = 2,
    IsoPriority = 3,
};
enum class FocusControlMode : uint8_t { Continuous = 0, LockedAuto = 1, Manual = 2 };

// Face detection from STATISTICS_FACE_RECTANGLES/SCORES, normalized 0..1 in
// the standard active-array space (SENSOR_INFO_ACTIVE_ARRAY_SIZE — the
// coordinate system face rects are reported in). Sorted by score, strongest
// first; capped at kMaxFaceDetections.
struct FaceDetection {
    float x = 0.0f;  // left
    float y = 0.0f;  // top
    float w = 0.0f;
    float h = 0.0f;
    uint8_t score = 0;  // 1..100, higher is more confident
};
inline constexpr size_t kMaxFaceDetections = 4;

// White-balance request mode. Values 0..8 mirror ACAMERA_CONTROL_AWB_MODE so
// the request writer can pass presets straight through to the HAL.
// ManualTempTint is a Rawr-side sentinel (no NDK counterpart in the pinned
// headers): AWB OFF + COLOR_CORRECTION TRANSFORM_MATRIX with in-app
// Kelvin/tint-derived gains and the last HAL color transform as seed.
enum class WhiteBalanceControlMode : uint8_t {
    Auto = 1,
    Incandescent = 2,
    Fluorescent = 3,
    WarmFluorescent = 4,
    Daylight = 5,
    CloudyDaylight = 6,
    Twilight = 7,
    Shade = 8,
    ManualTempTint = 9,
};

struct CameraControlCapabilities {
    uint64_t generation = 0;
    std::string cameraId;
    std::string lensId;
    int32_t rawWidth = 0;
    int32_t rawHeight = 0;
    // Active camera profile and its UI lenses as (lensId, label).
    std::string cameraProfileId;
    std::vector<std::pair<std::string, std::string>> profileLenses;

    int32_t sensitivityMin = 0;
    int32_t sensitivityMax = 0;
    int64_t exposureTimeMinNs = 0;
    int64_t exposureTimeMaxNs = 0;
    int64_t maxFrameDurationNs = 0;
    int32_t priorityAeTargetFpsMin = 0;
    int32_t priorityAeTargetFpsMax = 0;
    std::vector<std::array<int32_t, 2>> aeTargetFpsRanges;

    int32_t evMinSteps = 0;
    int32_t evMaxSteps = 0;
    int32_t evStepNumerator = 0;
    int32_t evStepDenominator = 1;

    bool manualExposureSupported = false;
    bool shutterPrioritySupported = false;
    bool isoPrioritySupported = false;
    // True when the HAL has no Camera2 AE priority for an axis but does have manual sensor control. S/I are then
    // emulated in software (camera/SoftwareAe.h): AE-off requests whose free axis the app drives from the
    // rendered frame. Hardware priority always wins when the HAL offers it.
    bool softwarePrioritySupported = false;
    // Raw NDK antibanding modes from ACAMERA_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES.
    // Values mirror ACAMERA_CONTROL_AE_ANTIBANDING_MODE_* (OFF=0, 50HZ=1, 60HZ=2, AUTO=3).
    // All four when the tag is missing (most HALs accept them).
    std::vector<uint8_t> supportedAntibandingModes;
    // Raw NDK AWB mode values from ACAMERA_CONTROL_AWB_AVAILABLE_MODES.
    // Empty when the tag is missing (assume Auto only).
    std::vector<uint8_t> supportedAwbModes;
    // True when manual temp/tint gains may be used (implied by the
    // MANUAL_SENSOR capability; FULL devices always include TRANSFORM_MATRIX).
    bool manualGainsSupported = false;
    // Static device color calibration for the AUTO/preset WB display readout.
    // The generic slider gains model is not calibrated to any sensor (its
    // cool-end red gain peaks near 1.26 while this HAL reports ~2.5 for an
    // ~8K scene), so the gains inverse pins at the 10K corner and stops
    // tracking. The snapshot display estimate instead solves CCT from the
    // HAL neutral through this calibration, matching dedicated camera apps.
    // Empty/invalid when the tags are missing: the display falls back to
    // the uncalibrated gains inverse.
    metadata::StaticColorCalibration wbDisplayCalibration{};
    bool tapAfSupported = false;
    bool manualFocusSupported = false;
    // True when the HAL advertises STATISTICS face detection (FULL preferred,
    // SIMPLE accepted). Powers the face boxes + face-steered continuous AF.
    bool faceDetectSupported = false;
    bool faceDetectFullMode = false;
    bool focusDistanceReadoutTrustworthy = false;
    bool oisSupported = false;
    float minimumFocusDistance = 0.0f;
    int32_t maxAfRegions = 0;
    int32_t maxAeRegions = 0;
};

struct CameraControlState {
    CameraControlCapabilities capabilities;
    ExposureControlMode exposureMode = ExposureControlMode::Auto;
    // Free-axis exposure written by SoftwareAe while a software priority mode is active (0 = not seeded yet).
    int64_t softExposureTimeNs = 0;
    int32_t softSensitivity = 0;
    FocusControlMode focusMode = FocusControlMode::Continuous;
    bool tapAfActive = false;
    uint64_t focusRequestId = 0;
    bool spotAeActive = false;
    // Snapshot projection of native cadence/angle intent, copied under the
    // controller mutex alongside requested/applied camera state.
    bool videoMode = false;
    int videoPreviewFps = 30;
    std::optional<double> requestedShutterAngleDegrees;
    std::vector<ShutterAngleChoice> shutterAngleChoices;
    WhiteBalanceControlMode whiteBalanceMode = WhiteBalanceControlMode::Auto;
    // ManualTempTint request coordinates. Kelvin in [2000, 10000], tint in
    // [-50, 50] (green-magenta axis, 0 neutral). Gains are derived at request
    // time; the RAW bytes are never touched (DNG carries AsShotNeutral only).
    int32_t requestedWhiteBalanceTemperatureK = 5200;
    int32_t requestedWhiteBalanceTint = 0;
    // Last HAL COLOR_CORRECTION_TRANSFORM result, seeding the manual-gains
    // request so switching AUTO->MANUAL keeps the current color rendering and
    // only moves the white point. Updated from repeating-request results while
    // not in manual (frozen for the manual session). Per the Camera2 docs,
    // switching to TRANSFORM_MATRIX with the HAL-provided gains + transform
    // reproduces the previous white point; slider moves then apply relative
    // ratios on top (see manualWhiteBalanceEntryGains).
    std::array<float, 9> manualWhiteBalanceTransformSeed{1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool hasManualWhiteBalanceTransformSeed = false;
    // Manual-gains entry baseline for relative white-point shifts. Captured
    // once on AUTO/preset->MANUAL from the live HAL AWB gains (raw RGGB),
    // with manualWhiteBalanceEntryTempK/Tint holding the neutral temp/tint
    // estimate they correspond to. Slider rendering is per-channel
    // entry x forward(requested)/forward(entry), so the first manual frame
    // matches the preset/auto it came from exactly and moving sliders back
    // restores it. Frozen for the manual session; refreshed on next entry.
    std::array<float, 4> manualWhiteBalanceEntryGains{1, 1, 1, 1};
    bool hasManualWhiteBalanceEntryGains = false;
    int32_t manualWhiteBalanceEntryTempK = 5200;
    int32_t manualWhiteBalanceEntryTint = 0;
    // Last HAL AWB gains (RGGB) observed while not in manual-gains mode.
    // Copied per CaptureResult (4-float copy, negligible); used only to seed
    // AUTO->MANUAL temp/tint so sliders begin at the current neutral state.
    // No per-frame estimation runs here — inversion happens once on manual
    // entry and in the 10 Hz snapshot serialization at most.
    std::array<float, 4> lastHalAwbGains{1, 1, 1, 1};
    bool hasLastHalAwbGains = false;
    // Last HAL neutral color point (sensor RGB, 1/gains) observed while not
    // in manual-gains mode. Feeds the calibrated display estimate
    // (neutral + wbDisplayCalibration -> CCT/tint); frozen in manual like
    // the gains above so our own echoed rendering can't overwrite the seed.
    std::array<float, 3> lastHalNeutral{1, 1, 1};
    bool hasLastHalNeutral = false;
    // Native display/entry estimate: calibrated when possible, gains inverse otherwise.
    // Published at UI snapshot cadence, never calculated on the per-frame callback.
    int32_t autoWbTemperatureK = 5200;
    int32_t autoWbTint = 0;
    bool hasAutoWbEstimate = false;
    bool autoWbEstimateCalibrated = false;
    // Acknowledges accepted and rejected UI commands, including slider gestures.
    int64_t whiteBalanceRequestId = 0;

    int64_t requestedExposureTimeNs = 0;
    int32_t requestedSensitivity = 0;
    int32_t requestedEvSteps = 0;
    // Requested AE antibanding mode (NDK values OFF=0, 50HZ=1, 60HZ=2, AUTO=3).
    uint8_t requestedAntibandingMode = 3;
    float requestedManualFocusNormalized = 1.0f;
    bool oisEnabled = true;
    // Auto-AE FPS floor for all Auto modes (5..30, default 15).
    // 30 == no override (template default). <30 writes
    // AE_TARGET_FPS_RANGE [floor, max] so the HAL can prefer longer
    // exposure over post-raw digital boost in low light.
    int32_t autoMinFps = 15;
    // A fixed recording cadence overrides the ordinary Auto exposure floor.
    // Zero restores the still/viewfinder request policy.
    int32_t recordingFps = 0;

    std::optional<int64_t> appliedExposureTimeNs;
    std::optional<int32_t> appliedSensitivity;
    std::optional<int32_t> appliedEvSteps;
    std::optional<uint8_t> afState;
    std::optional<float> appliedFocusDistance;
    // Camera2 CONTROL_POST_RAW_SENSITIVITY_BOOST result (100 == 1x).
    // Copied per repeating CaptureResult alongside appliedSensitivity so the
    // monitor can show the boost multiplier; null before the first result.
    std::optional<int32_t> appliedPostRawSensitivityBoost;
    // Sensor-reported frame rate: 1e9 / SENSOR_FRAME_DURATION result when the
    // HAL mirrors it, else the AE_TARGET_FPS_RANGE result upper bound.
    std::optional<double> appliedRawFps;
    // Measured viewfinder rate: EMA of SENSOR_TIMESTAMP deltas at full frame
    // rate, sampled by the 10 Hz UI snapshot. Null until enough frames arrive.
    std::optional<double> measuredViewfinderFps;
    // Latest face detections (strongest first). Updated from every repeating
    // CaptureResult while face detection is enabled; empty when unsupported,
    // disabled, or no faces in frame. Sensor-normalized; SessionEngine converts
    // to display coordinates for the UI snapshot.
    std::vector<FaceDetection> faceDetections;
};

// True when the current S/I mode must be emulated in software: the HAL lacks that axis's priority mode, manual
// sensor control exists, and this is plain photo preview (video cadence owns exposure through shutter angle).
inline bool usesSoftwarePriority(const CameraControlState& s) {
    if (!s.capabilities.softwarePrioritySupported || s.videoMode || s.recordingFps > 0) return false;
    return (s.exposureMode == ExposureControlMode::ShutterPriority && !s.capabilities.shutterPrioritySupported) ||
           (s.exposureMode == ExposureControlMode::IsoPriority && !s.capabilities.isoPrioritySupported);
}

// The state a Camera2 request is built from. A software priority mode is sent as ordinary Manual with the
// app-driven free axis substituted, so request building, provenance and the hardware priority audit need no
// knowledge of it.
inline CameraControlState requestStateFor(const CameraControlState& s) {
    if (!usesSoftwarePriority(s)) return s;
    CameraControlState out = s;
    if (s.exposureMode == ExposureControlMode::ShutterPriority) {
        if (s.softSensitivity > 0) out.requestedSensitivity = s.softSensitivity;
    } else if (s.softExposureTimeNs > 0) {
        out.requestedExposureTimeNs = s.softExposureTimeNs;
    }
    out.exposureMode = ExposureControlMode::Manual;
    return out;
}

}  // namespace rawrcam::camera
