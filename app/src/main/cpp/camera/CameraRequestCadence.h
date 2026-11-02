#pragma once

#include <algorithm>
#include <array>
#include <optional>

#include "camera/CameraControlTypes.h"

namespace rawrcam::camera {

// Bracket requests keep the recipe's cadence too; legacy lenses retain 30 fps.
inline int64_t cameraBracketFrameDuration(const CameraControlState& state, int64_t exposureNs) {
    const int64_t period = state.capabilities.frameRate > 0
                              ? 1'000'000'000LL / state.capabilities.frameRate : 33'333'333LL;
    const int64_t duration = std::max(period, exposureNs);
    return state.capabilities.maxFrameDurationNs > 0
               ? std::min(duration, state.capabilities.maxFrameDurationNs) : duration;
}

// Shared by session parameters, repeating requests and frozen provenance.
inline std::optional<std::array<int32_t, 2>> cameraTargetFpsRange(const CameraControlState& state,
                                                                  bool priorityRequested) {
    // A vendor sensor mode that runs at one rate (Xiaomi 75 mm 50 MP: 24 fps) keeps it on every request.
    if (state.capabilities.frameRate > 0)
        return std::array<int32_t, 2>{state.capabilities.frameRate, state.capabilities.frameRate};
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
