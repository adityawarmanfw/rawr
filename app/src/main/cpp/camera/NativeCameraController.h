#pragma once

#include <android/native_window.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "camera/CameraControlTypes.h"
#include "camera/CameraRouting.h"
#include "metadata/CameraContextMetadata.h"
#include "metadata/FrameMetadataSnapshot.h"

namespace rawrcam::camera {

struct PreviewCallbacks {
    std::function<bool(uint64_t, const metadata::CameraContextMetadata&, const CameraControlCapabilities&)>
        configurePreview;
    std::function<ANativeWindow*(uint64_t, uint32_t, uint32_t, geometry::RawPixelFormat)> createRawWindow;
    std::function<void(uint64_t)> destroyRawWindow;
    std::function<bool(const metadata::FrameMetadataSnapshot&)> submitMetadata;
    std::function<void(const std::string&)> diagnostic;
};

// Owns Camera2 NDK lifecycle and request/result acquisition only.
// It does not interpret color calibration and it does not own Vulkan/UI state.
class NativeCameraController {
   public:
    explicit NativeCameraController(PreviewCallbacks callbacks);
    ~NativeCameraController();

    NativeCameraController(const NativeCameraController&) = delete;
    NativeCameraController& operator=(const NativeCameraController&) = delete;

    void setActive(bool active);
    void setLensId(const std::string& lensId);
    // Replaces the lens profile (user configuration). Returns false when a
    // debug setprop pins a built-in profile.
    bool setProfile(CameraProfile profile);
    void setPreferredCameraId(const std::string& cameraId);
    void setAccessRoute(const std::string& route);

    CameraControlState controlState() const;
    // -1 while no selected camera context is available.
    int videoRotationDegrees(int deviceRotationDegrees) const;
    bool setExposureMode(ExposureControlMode mode);
    void setManualExposureTimeNs(int64_t exposureTimeNs);
    void setManualSensitivity(int32_t sensitivity);
    // HDR+ bracketed: submits one AE-off frame per evOffsets entry (each <= 0)
    // relative to a delivered frame's CaptureResult exposure/sensitivity,
    // tagged with requestId. The repeating request is untouched. False when
    // nothing was submitted (no session, no manual exposure support, unknown
    // sensitivity coordinate).
    bool captureExposureBracket(uint64_t requestId, int64_t baseExposureTimeNs, int32_t baseReportedSensitivity,
                                const std::vector<float>& evOffsets);
    // Allows the once-per-session DCG ISO calibration frame that
    // captureExposureBracket needs in Auto; off unless HDR+ Bracketed is selected.
    void setSensitivityCalibrationWanted(bool wanted);
    void setExposureCompensationSteps(int32_t steps);
    // White balance. setWhiteBalanceMode returns false when the requested
    // preset/manual mode is not advertised (caller keeps previous UI state;
    // the snapshot confirms accepted values). setWhiteBalanceTempTint clamps
    // into range and enters manual-gains mode.
    bool setWhiteBalanceMode(WhiteBalanceControlMode mode, int64_t requestId);
    void setWhiteBalanceTempTint(int32_t temperatureK, int32_t tint, int32_t editedAxes, int64_t requestId);
    // Locked entry preserves the displayed coordinates while capturing the
    // live HAL gains baseline. Uses absolute gains when no HAL seed exists.
    void setWhiteBalanceLocked(int32_t temperatureK, int32_t tint, int64_t requestId);
    void setOisEnabled(bool enabled);
    // NDK antibanding values OFF=0, 50HZ=1, 60HZ=2, AUTO=3. Out-of-range clamps to AUTO.
    void setAntibandingMode(uint8_t mode);
    void setAutoMinFps(int fps);
    void setVideoMode(bool video, int fps);
    void setShutterAngleDegrees(double degrees);
    bool setRecordingFps(int fps);
    // Diagnostic mirror of the SessionEngine ingress toggle so SELECTION logs
    // the effective per-session value. CameraSessionPolicy owns restarts.
    void setExperimentalZeroCopyEnabled(bool enabled);
    // Camera2-native spot metering used only when ordinary Camera2 Auto AE owns
    // exposure. Coordinates are normalized in sensor/source space.
    void setSpotMeteringTargetSensorNormalized(bool active, float x, float y);
    bool setFocusMode(FocusControlMode mode, uint64_t requestId);
    void focusAtSensorNormalized(float x, float y, uint64_t requestId);
    void acknowledgeFocusRequest(uint64_t requestId);
    void clearTapAf(uint64_t requestId);
    void setManualFocusNormalized(float normalized, uint64_t requestId);
    // Called on the serial camera lane; independent of UI snapshot polling.
    void tickControls(int64_t nowMs);
    // True once the repeating request (which always carries the lens-shading
    // map when supported) is running, so a still can claim upcoming results.
    bool stillCaptureMetadataReady();

    void shutdown();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// This device's built-in lens profile (system-property match, or the
// debug.rawr.camera_profile override); the Lens settings start from it.
[[nodiscard]] const CameraProfile& deviceBuiltInCameraProfile();

}  // namespace rawrcam::camera
