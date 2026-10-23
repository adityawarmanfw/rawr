// Exposure/focus/spot-AE controls (Camera2-owned).
#include <iomanip>
#include <sstream>

#include "camera/NativeCameraController.h"
#include "diagnostics/logging/RuntimeTraceRecorder.h"
#include "session/SessionEngine.h"

namespace rawrcam::session {

void SessionEngine::setExposureMode(int mode) {
    if (!cameraControls_.controller()) return;
    // Camera2 S/I is capability-gated. Do not publish the semantic UI mode
    // until the native camera owner has actually accepted that mode. This
    // prevents an unsupported/rejected S or I tap from looking selected while
    // the repeating request remains ordinary Auto/Manual underneath.
    bool accepted = true;
    const auto cameraMode = mode == 1   ? rawrcam::camera::ExposureControlMode::Manual
                            : mode == 2 ? rawrcam::camera::ExposureControlMode::ShutterPriority
                            : mode == 3 ? rawrcam::camera::ExposureControlMode::IsoPriority
                                        : rawrcam::camera::ExposureControlMode::Auto;
    accepted = cameraControls_.controller()->setExposureMode(cameraMode);
    if (!accepted) return;

    {
        const auto tracedState = cameraControls_.controller()->controlState();
        rawrcam::diagnostics::RuntimeTraceRecorder::instance().record(
            rawrcam::diagnostics::RuntimeTraceStage::ExposureMode, 0, 0, -1, tracedState.requestedExposureTimeNs,
            tracedState.requestedSensitivity, static_cast<std::uint32_t>(mode),
            static_cast<std::int64_t>(tracedState.exposureMode), std::atoi(tracedState.capabilities.cameraId.c_str()));
    }
}
void SessionEngine::setManualExposureTimeNs(int64_t value) {
    if (!cameraControls_.controller()) return;
    const auto exposureMode = cameraControls_.controller()->controlState().exposureMode;
    if (exposureMode == rawrcam::camera::ExposureControlMode::ShutterPriority ||
        exposureMode == rawrcam::camera::ExposureControlMode::Manual) {
        cameraControls_.controller()->setManualExposureTimeNs(value);
    }
}
void SessionEngine::setManualSensitivity(int32_t value) {
    if (!cameraControls_.controller()) return;
    const auto exposureMode = cameraControls_.controller()->controlState().exposureMode;
    if (exposureMode == rawrcam::camera::ExposureControlMode::IsoPriority ||
        exposureMode == rawrcam::camera::ExposureControlMode::Manual) {
        cameraControls_.controller()->setManualSensitivity(value);
    }
}
void SessionEngine::setExposureCompensationSteps(int32_t value) {
    if (!cameraControls_.controller()) return;
    if (cameraControls_.controller()->controlState().exposureMode == rawrcam::camera::ExposureControlMode::Manual) {
        return;
    }
    cameraControls_.controller()->setExposureCompensationSteps(value);
}
void SessionEngine::setFocusMode(int mode, float x, float y, uint64_t requestId) {
    if (!cameraControls_.controller()) return;
    rawrcam::camera::FocusControlMode m = rawrcam::camera::FocusControlMode::Continuous;
    if (mode == 1)
        m = rawrcam::camera::FocusControlMode::LockedAuto;
    else if (mode == 2)
        m = rawrcam::camera::FocusControlMode::Manual;
    if (cameraControls_.controller()->setFocusMode(m, requestId) && m == rawrcam::camera::FocusControlMode::LockedAuto)
        focusAtDisplay(x, y, requestId);
}
void SessionEngine::setManualFocusNormalized(float value, uint64_t requestId) {
    if (cameraControls_.controller()) cameraControls_.controller()->setManualFocusNormalized(value, requestId);
}
namespace {
// A display point on a mirrored/undistorted viewfinder -> where it sits on the sensor.
std::optional<rawrcam::geometry::NormalizedPoint> throughLensWarp(
    rawrcam::geometry::NormalizedPoint point, uint32_t mirror,
    const std::optional<rawrcam::geometry::LensDistortionModel>& warp) {
    const auto m = rawrcam::geometry::mirroredUv(mirror, point.x, point.y);
    point = {float(m[0]), float(m[1])};
    if (!warp) return point;
    const auto s = rawrcam::geometry::sourceUv(*warp, point.x, point.y);
    if (s[0] < 0.0 || s[0] > 1.0 || s[1] < 0.0 || s[1] > 1.0) return std::nullopt;
    return rawrcam::geometry::NormalizedPoint{float(s[0]), float(s[1])};
}
}  // namespace
void SessionEngine::focusAtDisplay(float x, float y, uint64_t requestId) {
    if (!cameraControls_.controller()) return;
    std::optional<rawrcam::geometry::NormalizedPoint> point;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const int presentationRotation =
            (realtime_.sensorOrientationDegrees() - realtime_.displayRotationDegrees() + 360) % 360;
        point = rawrcam::geometry::displayToSource(
            x, y, realtime_.previewWidth(), realtime_.previewHeight(), swapchainRenderer_.extent().width,
            swapchainRenderer_.extent().height, presentationRotation, previewVideoCropRectLocked());
    }
    if (point) point = throughLensWarp(*point, swapchainRenderer_.presentedLensMirror(),
                            swapchainRenderer_.presentedLensWarp());
    if (point)
        cameraControls_.controller()->focusAtSensorNormalized(point->x, point->y, requestId);
    else
        cameraControls_.controller()->acknowledgeFocusRequest(requestId);
}
void SessionEngine::clearTapAf(uint64_t requestId) {
    if (!cameraControls_.controller()) return;
    cameraControls_.controller()->clearTapAf(requestId);
}
std::string SessionEngine::faceDetectionsDisplaySnapshot() {
    if (!cameraControls_.controller()) return "[]";
    const std::vector<rawrcam::camera::FaceDetection> faces =
        cameraControls_.controller()->controlState().faceDetections;
    if (faces.empty()) return "[]";
    std::lock_guard<std::mutex> lock(mu_);
    if (!realtime_.configured() || !swapchainRenderer_.ready()) return "[]";
    const int presentationRotation =
        (realtime_.sensorOrientationDegrees() - realtime_.displayRotationDegrees() + 360) % 360;
    const uint32_t srcW = realtime_.previewWidth();
    const uint32_t srcH = realtime_.previewHeight();
    const uint32_t dstW = swapchainRenderer_.extent().width;
    const uint32_t dstH = swapchainRenderer_.extent().height;
    std::ostringstream s;
    s << std::fixed << std::setprecision(4) << '[';
    bool first = true;
    const auto crop = previewVideoCropRectLocked();
    const auto warp = swapchainRenderer_.presentedLensWarp();
    const uint32_t mirror = swapchainRenderer_.presentedLensMirror();
    for (const auto& f : faces) {
        // Sensor corners -> the undistorted, mirrored viewfinder.
        auto corner = [&](float sx, float sy) {
            if (warp) {
                const auto c = rawrcam::geometry::correctedUv(*warp, sx, sy);
                sx = float(c[0]);
                sy = float(c[1]);
            }
            const auto m = rawrcam::geometry::mirroredUv(mirror, sx, sy);
            sx = float(m[0]);
            sy = float(m[1]);
            return rawrcam::geometry::sourceToDisplay(sx, sy, srcW, srcH, dstW, dstH, presentationRotation, crop);
        };
        const auto a = corner(f.x, f.y);
        const auto b = corner(f.x + f.w, f.y + f.h);
        if (!a || !b) continue;
        const float x = std::min(a->x, b->x);
        const float y = std::min(a->y, b->y);
        const float w = std::abs(b->x - a->x);
        const float h = std::abs(b->y - a->y);
        if (!first) s << ',';
        first = false;
        s << '[' << x << ',' << y << ',' << w << ',' << h << ',' << static_cast<int>(f.score) << ']';
    }
    s << ']';
    return s.str();
}
void SessionEngine::setSpotAeDisplay(bool active, float x, float y) {
    if (!active) {
        if (cameraControls_.controller()) {
            cameraControls_.controller()->setSpotMeteringTargetSensorNormalized(false, 0.5f, 0.5f);
        }
        return;
    }

    std::optional<rawrcam::geometry::NormalizedPoint> point;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!realtime_.configured() || !swapchainRenderer_.ready()) {
            return;
        }
        const int presentationRotation =
            (realtime_.sensorOrientationDegrees() - realtime_.displayRotationDegrees() + 360) % 360;
        point = rawrcam::geometry::displayToSource(
            x, y, realtime_.previewWidth(), realtime_.previewHeight(), swapchainRenderer_.extent().width,
            swapchainRenderer_.extent().height, presentationRotation, previewVideoCropRectLocked());
        if (!point) return;
    }
    point = throughLensWarp(*point, swapchainRenderer_.presentedLensMirror(),
                            swapchainRenderer_.presentedLensWarp());

    if (cameraControls_.controller()) {
        cameraControls_.controller()->setSpotMeteringTargetSensorNormalized(true, point->x, point->y);
    }
}

}  // namespace rawrcam::session
