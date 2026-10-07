#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "camera/CameraSessionPolicy.h"

namespace rawrcam::camera {
class NativeCameraController;
}  // namespace rawrcam::camera

namespace rawrcam::camera {

// Sole owner of the Camera2 NDK controller and the pure camera-domain
// controls (activation, selection, preferences, OIS/DCG, snapshot).
// Cross-domain flows (spot-AE dual routing, still arming/submission,
// shutdown sequencing) reach the controller through controller(); all other
// access stays inside this class. Null-safe exactly like the previous
// inline unique_ptr checks: controller() returns nullptr until attach().
class CameraControlSurface final {
   public:
    CameraControlSurface() = default;

    // Takes ownership of a fully constructed controller (PreviewCallbacks
    // are built by SessionEngine, which owns the callback targets).
    void attach(std::unique_ptr<rawrcam::camera::NativeCameraController> controller);
    void shutdown();

    [[nodiscard]] rawrcam::camera::NativeCameraController* controller() noexcept;
    [[nodiscard]] const rawrcam::camera::NativeCameraController* controller() const noexcept;

    void setCameraActive(bool active);
    // Atomic inputs/readouts, safe on Main while the operation lane is blocked.
    void setForeground(bool foreground);
    void setStillCaptureInFlight(bool begin);
    bool canRecoverStills() const;
    // Capture callback input: timestamp publication only.
    void noteFrameResult() noexcept;
    // All remaining policy and resource operations require the serial JNI lane.
    void setSurfaceReady(bool ready);
    void setInitialConfigReady();
    void tickSession();
    void setZeroCopy(bool enabled);
    void rawFrameArrived(uint64_t generation);
    void setInternalTraceCaptureEnabled(bool enabled);
    void setOisEnabled(bool enabled);
    void setAntibandingMode(uint8_t mode);
    void setAutoMinFps(int fps);
    void setAePriorityDisabled(bool disabled);
    void setVideoMode(bool video, int fps);
    bool setRecordingFps(int fps);
    void setShutterAngleDegrees(double degrees);
    void selectLens(const std::string& lensId);
    // Lens profile JSON (CameraProfileJson); false when it is malformed or
    // a debug setprop pins a built-in profile.
    bool setCameraProfile(const std::string& json);
    std::string cameraControlSnapshot() const;
    int videoRotationDegrees(int deviceRotationDegrees) const;
    void setPreferredCameraId(const std::string& cameraId);
    void setCameraAccessRoute(const std::string& route);

   private:
    void applyCameraActive(bool active);
    std::unique_ptr<rawrcam::camera::NativeCameraController> cameraController_;
    std::mutex rawFrameMutex_;
    // Policy/configuration actions share the adapter's serial lane. Immediate
    // Android intent and capture callback time cross threads through atomics.
    rawrcam::camera::CameraSessionPolicy sessionPolicy_;
    std::atomic<int64_t> lastFrameTime_{0};
    std::atomic<bool> foregroundDesired_{false};
    std::atomic<bool> surfaceReady_{false};
    std::atomic<unsigned> stillLeases_{0};
};

}  // namespace rawrcam::camera
