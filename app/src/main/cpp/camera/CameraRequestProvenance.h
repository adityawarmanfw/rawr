#pragma once

#include "camera/CameraAePriority.h"
#include "camera/CameraControlTypes.h"
#include "camera/CameraRequestCadence.h"
#include "camera/ViewfinderExposure.h"

namespace rawrcam::camera {

// Frozen intent attached to the framework copy of the request that produced a
// result. The controller retains its storage until session retirement.
struct CameraRequestProvenance {
    uint64_t requestSerial = 0;
    ExposureControlMode exposureMode = ExposureControlMode::Auto;
    uint8_t expectedAePriority = camera2_priority::kOff;
    int64_t requestedExposureTimeNs = 0;
    int32_t requestedSensitivity = 0;
    int32_t targetFpsMin = 0;
    int32_t targetFpsMax = 0;
    uint64_t optimizedStillRequestId = 0;

    static CameraRequestProvenance from(const CameraControlState& state, uint64_t serial) {
        CameraRequestProvenance result;
        result.requestSerial = serial;
        result.exposureMode = state.exposureMode;
        result.requestedExposureTimeNs = state.requestedExposureTimeNs;
        result.requestedSensitivity = state.requestedSensitivity;
        if (state.exposureMode == ExposureControlMode::Manual) {
            const auto pair = viewfinderManualPair(state);
            result.requestedExposureTimeNs = pair.exposureTimeNs;
            result.requestedSensitivity = pair.sensitivity;
        }
        // rawr AE owns both axes like M (AE OFF): its frames calibrate the
        // sensitivity coordinate too.
        const bool rawrAe = state.exposureMode != ExposureControlMode::Manual && state.rawrAeActive &&
                            state.rawrAeExposureTimeNs > 0 && state.rawrAeSensitivity > 0;
        if (rawrAe) {
            result.exposureMode = ExposureControlMode::Manual;
            result.requestedExposureTimeNs = state.rawrAeExposureTimeNs;
            result.requestedSensitivity = state.rawrAeSensitivity;
            return result;
        }
        const bool priority = state.exposureMode == ExposureControlMode::ShutterPriority ||
                              state.exposureMode == ExposureControlMode::IsoPriority;
        if (const auto fps = cameraTargetFpsRange(state, priority)) {
            result.targetFpsMin = (*fps)[0];
            result.targetFpsMax = (*fps)[1];
        }
        if (state.exposureMode == ExposureControlMode::ShutterPriority) {
            result.expectedAePriority = camera2_priority::kSensorExposureTimePriority;
        } else if (state.exposureMode == ExposureControlMode::IsoPriority) {
            result.expectedAePriority = camera2_priority::kSensorSensitivityPriority;
        }
        return result;
    }
};

}  // namespace rawrcam::camera
