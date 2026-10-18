#pragma once

#include <algorithm>
#include <array>
#include <optional>

#include "camera/CameraControlTypes.h"

namespace rawrcam::camera {

// Shared by session parameters, repeating requests and frozen provenance.
inline std::optional<std::array<int32_t, 2>> cameraTargetFpsRange(const CameraControlState& state,
                                                                  bool priorityRequested) {
    if (state.recordingFps > 0) return std::array<int32_t, 2>{state.recordingFps, state.recordingFps};
    const auto& caps = state.capabilities;
    if (priorityRequested && caps.priorityAeTargetFpsMin > 0 &&
        caps.priorityAeTargetFpsMax >= caps.priorityAeTargetFpsMin) {
        int32_t floor = caps.priorityAeTargetFpsMin;
        if (state.autoMinFps >= 5 && state.autoMinFps < 30) floor = std::max(floor, state.autoMinFps);
        return std::array<int32_t, 2>{std::min(floor, caps.priorityAeTargetFpsMax), caps.priorityAeTargetFpsMax};
    }
    if ((state.exposureMode == ExposureControlMode::Auto || caps.targetFpsRangeRequired) && state.autoMinFps >= 5 &&
        state.autoMinFps < 30) {
        const auto maximum = caps.priorityAeTargetFpsMax > 0 ? caps.priorityAeTargetFpsMax : 30;
        return std::array<int32_t, 2>{std::min(state.autoMinFps, maximum), maximum};
    }
    if (caps.targetFpsRangeRequired) {
        const auto maximum = caps.priorityAeTargetFpsMax > 0 ? caps.priorityAeTargetFpsMax : 30;
        return std::array<int32_t, 2>{maximum, maximum};
    }
    return std::nullopt;
}

}  // namespace rawrcam::camera
