#include "camera/CameraFocusControls.h"

#include <algorithm>
#include <cmath>

namespace rawrcam::camera {

void CameraFocusControls::reset(CameraControlState& state) {
    *this = CameraFocusControls{};
    state.tapAfActive = false;
}

bool CameraFocusControls::requestMode(CameraControlState& state, FocusControlMode mode, uint64_t requestId) {
    state.focusRequestId = std::max(state.focusRequestId, requestId);
    if (mode == FocusControlMode::Manual && !state.capabilities.manualFocusSupported) return false;
    if (mode == FocusControlMode::LockedAuto && !state.capabilities.tapAfSupported) return false;
    if (mode == FocusControlMode::Manual && state.focusMode != mode && state.appliedFocusDistance &&
        state.capabilities.minimumFocusDistance > 0 && std::isfinite(state.capabilities.minimumFocusDistance) &&
        std::isfinite(*state.appliedFocusDistance)) {
        state.requestedManualFocusNormalized =
            std::clamp(1.0f - *state.appliedFocusDistance / state.capabilities.minimumFocusDistance, 0.0f, 1.0f);
    }
    state.focusMode = mode;
    clearTap(state);
    return true;
}

bool CameraFocusControls::requestManualFocus(CameraControlState& state, float normalized, uint64_t requestId) {
    state.focusRequestId = std::max(state.focusRequestId, requestId);
    if (!std::isfinite(normalized) || !state.capabilities.manualFocusSupported) return false;
    state.requestedManualFocusNormalized = std::clamp(normalized, 0.0f, 1.0f);
    state.focusMode = FocusControlMode::Manual;
    clearTap(state);
    return true;
}

std::optional<CameraTapFocusPlan> CameraFocusControls::planTap(const CameraControlState& state,
                                                               const metadata::SensorGeometry& geometry,
                                                               SensorPoint point) const {
    if (!state.capabilities.tapAfSupported) return std::nullopt;
    const auto region = tapFocusRegion(geometry, point);
    if (!region) return std::nullopt;
    return CameraTapFocusPlan{*region, state.tapAfActive || trigger_ != CameraFocusTrigger::Idle};
}

void CameraFocusControls::acceptTap(CameraControlState& state, const CameraTapFocusPlan& plan, int64_t nowMs) {
    // A viewfinder tap is an AF request: leave MF (AF_MODE_OFF would ignore it)
    // for continuous AF steered to the tap, exactly like a tap from AF.
    if (state.focusMode == FocusControlMode::Manual) state.focusMode = FocusControlMode::Continuous;
    region_ = plan.region;
    state.tapAfActive = true;
    tapPolicy_.acceptedTap(state.focusMode == FocusControlMode::Continuous, nowMs);
    faceSteering_ = false;
    trigger_ = CameraFocusTrigger::Start;
}

void CameraFocusControls::clearTap(CameraControlState& state) {
    state.tapAfActive = false;
    tapPolicy_.clear();
    region_.reset();
    faceSteering_ = false;
    trigger_ = CameraFocusTrigger::Cancel;
}

bool CameraFocusControls::expireTap(CameraControlState& state, int64_t nowMs) {
    if (!tapPolicy_.shouldClear(nowMs)) return false;
    clearTap(state);
    return true;
}

bool CameraFocusControls::finishTrigger() {
    if (trigger_ == CameraFocusTrigger::Idle) return false;
    trigger_ = CameraFocusTrigger::Idle;
    return true;
}

FaceFocusChange CameraFocusControls::steerFaces(const CameraControlState& state,
                                                const metadata::SensorGeometry* geometry, bool requestReady) {
    if (state.focusMode != FocusControlMode::Continuous || state.tapAfActive ||
        !state.capabilities.faceDetectSupported || state.capabilities.maxAfRegions <= 0 || !geometry || !requestReady) {
        faceSteering_ = false;
        return FaceFocusChange::None;
    }
    if (state.faceDetections.empty()) {
        if (!faceSteering_) return FaceFocusChange::None;
        faceSteering_ = false;
        region_.reset();
        return FaceFocusChange::Cleared;
    }
    const auto& face = state.faceDetections.front();
    if (faceSteering_) {
        const float dx = std::abs(face.x + face.w * 0.5f - (lastFace_.x + lastFace_.w * 0.5f));
        const float dy = std::abs(face.y + face.h * 0.5f - (lastFace_.y + lastFace_.h * 0.5f));
        const float dw = lastFace_.w > 1e-6f ? std::abs(face.w - lastFace_.w) / lastFace_.w : 1.0f;
        const float dh = lastFace_.h > 1e-6f ? std::abs(face.h - lastFace_.h) / lastFace_.h : 1.0f;
        if (dx < 0.02f && dy < 0.02f && dw < 0.20f && dh < 0.20f) return FaceFocusChange::None;
    }
    const auto region = faceFocusRegion(*geometry, face);
    if (!region) return FaceFocusChange::None;
    region_ = region;
    faceSteering_ = true;
    lastFace_ = face;
    return FaceFocusChange::Updated;
}

}  // namespace rawrcam::camera
