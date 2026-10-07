// Camera controller ownership, native session-policy execution, and controls.
#include "camera/CameraControlSurface.h"

#include <chrono>

#include "camera/CameraControlSerialization.h"
#include "camera/CameraProfileJson.h"
#include "camera/NativeCameraController.h"
#include "diagnostics/logging/NativeLog.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"

namespace rawrcam::camera {
namespace {
int64_t monotonicMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

void CameraControlSurface::attach(std::unique_ptr<rawrcam::camera::NativeCameraController> controller) {
    cameraController_ = std::move(controller);
}
void CameraControlSurface::shutdown() {
    if (cameraController_) {
        cameraController_->shutdown();
        std::lock_guard<std::mutex> lock(rawFrameMutex_);
        cameraController_.reset();
    }
}
rawrcam::camera::NativeCameraController* CameraControlSurface::controller() noexcept { return cameraController_.get(); }
const rawrcam::camera::NativeCameraController* CameraControlSurface::controller() const noexcept {
    return cameraController_.get();
}
void CameraControlSurface::setCameraActive(bool active) {
    // Explicit activation is reserved for teardown/offline replay. Live
    // lifecycle callers submit foreground/surface/configuration facts instead.
    sessionPolicy_.forceStopped();
    foregroundDesired_.store(false, std::memory_order_relaxed);
    stillLeases_.store(0, std::memory_order_relaxed);
    applyCameraActive(active);
}
void CameraControlSurface::applyCameraActive(bool active) {
    if (cameraController_) cameraController_->setActive(active);
    if (!active && rawrcam::diagnostics::RuntimeTraceRecorder::instance().enabled()) {
        (void)rawrcam::diagnostics::RuntimeTraceRecorder::instance().dumpToFile();
    }
}
void CameraControlSurface::setForeground(bool foreground) {
    // Main-thread safe: publish intent without waiting for HAL retirement.
    foregroundDesired_.store(foreground, std::memory_order_relaxed);
}
void CameraControlSurface::setSurfaceReady(bool ready) {
    surfaceReady_.store(ready, std::memory_order_relaxed);
    sessionPolicy_.setSurfaceReady(ready);
    tickSession();
}
void CameraControlSurface::setInitialConfigReady() {
    sessionPolicy_.setConfigReady();
    tickSession();
}
void CameraControlSurface::setStillCaptureInFlight(bool begin) {
    // Acquired before IO capture dispatch, visible even to an already queued
    // clock tick. Unmatched completions cannot underflow the lease counter.
    if (begin)
        stillLeases_.fetch_add(1, std::memory_order_relaxed);
    else {
        auto count = stillLeases_.load(std::memory_order_relaxed);
        while (count > 0 && !stillLeases_.compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {
        }
    }
}
bool CameraControlSurface::canRecoverStills() const {
    return foregroundDesired_.load(std::memory_order_relaxed) && surfaceReady_.load(std::memory_order_relaxed);
}
void CameraControlSurface::noteFrameResult() noexcept {
    lastFrameTime_.store(monotonicMillis(), std::memory_order_relaxed);
}
void CameraControlSurface::tickSession() {
    if (!cameraController_) return;
    cameraController_->tickControls(monotonicMillis());
    sessionPolicy_.setForeground(foregroundDesired_.load(std::memory_order_relaxed), monotonicMillis());
    sessionPolicy_.setStillLeases(stillLeases_.load(std::memory_order_relaxed));
    const auto action = sessionPolicy_.advance(monotonicMillis(), lastFrameTime_.load(std::memory_order_relaxed));
    using Action = rawrcam::camera::CameraSessionPolicy::Action;
    if (action == Action::None) return;
    LOGI("CAMERA_SESSION_POLICY action=%d", static_cast<int>(action));
    if (action == Action::Stop || action == Action::Restart) applyCameraActive(false);
    if (action == Action::Start || action == Action::Restart) {
        // A stop can arrive while retirement blocks the operation lane.
        if (!foregroundDesired_.load(std::memory_order_relaxed)) {
            sessionPolicy_.setForeground(false, monotonicMillis());
            (void)sessionPolicy_.advance(monotonicMillis(), 0);
            return;
        }
        lastFrameTime_.store(0, std::memory_order_relaxed);
        applyCameraActive(true);
        sessionPolicy_.activationCompleted(monotonicMillis());
    }
}
void CameraControlSurface::setZeroCopy(bool enabled) {
    if (cameraController_) cameraController_->setExperimentalZeroCopyEnabled(enabled);
    sessionPolicy_.setZeroCopy(enabled, monotonicMillis());
}
void CameraControlSurface::rawFrameArrived(uint64_t generation) {
    // Ingress outlives the controller during shutdown.
    std::lock_guard<std::mutex> lock(rawFrameMutex_);
    if (cameraController_) cameraController_->rawFrameArrived(generation);
}
void CameraControlSurface::setInternalTraceCaptureEnabled(bool enabled) {
    rawrcam::diagnostics::RuntimeTraceRecorder::instance().setEnabled(enabled);
    LOGI("INTERNAL_TRACE_CAPTURE enabled=%s", enabled ? "true" : "false");
}
void CameraControlSurface::setOisEnabled(bool enabled) {
    if (cameraController_) cameraController_->setOisEnabled(enabled);
}
void CameraControlSurface::setAntibandingMode(uint8_t mode) {
    if (cameraController_) cameraController_->setAntibandingMode(mode);
}
void CameraControlSurface::setAutoMinFps(int fps) {
    if (cameraController_) cameraController_->setAutoMinFps(fps);
}
void CameraControlSurface::setVideoMode(bool video, int fps) {
    if (cameraController_) cameraController_->setVideoMode(video, fps);
}
bool CameraControlSurface::setRecordingFps(int fps) {
    return cameraController_ && cameraController_->setRecordingFps(fps);
}
void CameraControlSurface::setShutterAngleDegrees(double degrees) {
    if (cameraController_) cameraController_->setShutterAngleDegrees(degrees);
}
void CameraControlSurface::selectLens(const std::string& lensId) {
    if (cameraController_) cameraController_->setLensId(lensId);
}
bool CameraControlSurface::setCameraProfile(const std::string& json) {
    if (!cameraController_) return false;
    auto profile = rawrcam::camera::parseCameraProfile(json);
    return profile && cameraController_->setProfile(std::move(*profile));
}
std::string CameraControlSurface::cameraControlSnapshot() const {
    if (!cameraController_) return "{}";
    const auto cameraState = cameraController_->controlState();
    auto json = rawrcam::camera::serializeCameraControlState(cameraState);
    if (!json.empty() && json.back() == '}') {
        json.pop_back();
        const int semanticMode = static_cast<int>(cameraState.exposureMode);
        json += ",\"semanticExposureMode\":" + std::to_string(semanticMode) + "}";
    }
    return json;
}
int CameraControlSurface::videoRotationDegrees(int deviceRotationDegrees) const {
    return cameraController_ ? cameraController_->videoRotationDegrees(deviceRotationDegrees) : -1;
}
void CameraControlSurface::setPreferredCameraId(const std::string& cameraId) {
    if (cameraController_) cameraController_->setPreferredCameraId(cameraId);
}
void CameraControlSurface::setCameraAccessRoute(const std::string& route) {
    if (cameraController_) cameraController_->setAccessRoute(route);
}

}  // namespace rawrcam::camera
