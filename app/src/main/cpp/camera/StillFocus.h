#pragma once
#include <algorithm>
#include <cmath>

#include "camera/CameraControlTypes.h"

namespace rawrcam::camera {
inline bool focusDistanceMatches(float observed, float wanted) {
    return std::isfinite(observed) && std::isfinite(wanted) && observed >= 0 && wanted >= 0 &&
           std::abs(observed - wanted) <= std::max(0.01f, std::abs(wanted) * 0.02f);
}
inline float requestedFocusDiopters(const CameraControlState& state) {
    return (1.0f - std::clamp(state.requestedManualFocusNormalized, 0.0f, 1.0f)) *
           state.capabilities.minimumFocusDistance;
}
inline void invalidateFocus(CameraControlState& state) {
    ++state.focusEpoch;
    state.focusObservedAtMs = 0;
    state.focusTriggerTimestampNs = 0;
    state.afState.reset();
}
inline bool stillFocusReady(const CameraControlState& state, int64_t nowMs) {
    if (!state.capabilities.manualFocusSupported) return state.capabilities.minimumFocusDistance == 0;  // fixed lens
    if (state.focusObservedEpoch != state.focusEpoch || state.focusObservedAtMs <= 0 ||
        nowMs - state.focusObservedAtMs > 1500 || nowMs < state.focusObservedAtMs || !state.appliedFocusDistance ||
        !std::isfinite(*state.appliedFocusDistance) || *state.appliedFocusDistance < 0 || state.lensState != 0)
        return false;
    if (state.focusMode == FocusControlMode::Manual) {
        const float wanted = requestedFocusDiopters(state);
        return state.appliedAfMode == 0 && focusDistanceMatches(*state.appliedFocusDistance, wanted);
    }
    return state.afState == 2 || state.afState == 4;
}
}  // namespace rawrcam::camera
