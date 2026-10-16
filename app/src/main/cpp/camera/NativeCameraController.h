#pragma once

#include <android/native_window.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
    std::function<void(uint64_t)> captureFailed;
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
    // HDR+ bracketed: submits one AE-off frame per evOffsets entry (> 0: a
    // long frame, shutter first up to 1/15 s; <= 0: a dark frame, gain
    // first) relative to a delivered frame's CaptureResult exposure/sensitivity,
    // tagged with requestId. The repeating request is untouched. False when
    // nothing was submitted (no session, no manual exposure support, unknown
    // sensitivity coordinate).
    bool captureExposureBracket(uint64_t requestId, int64_t baseExposureTimeNs, int32_t baseReportedSensitivity,
                                const std::vector<float>& evOffsets);
    // Most a positive-EV long frame can gain over the current repeating
    // exposure (EV); nullopt when unknown.
    std::optional<double> longFrameRoomEv() const;
    // False when the shutter/gain limits would leave long frames under
    // 0.5 EV brighter (they are skipped); true when unknown.
    bool longFramesWorthwhile() const;
    // Measures the reported/requested ISO ratio during startup when lens
    // session keys change the sensor mode (a ratio of 1 is a valid result;
    // keys do not imply DCG), before exposing the viewfinder.
    // Enabling it on an uncalibrated live session restarts that session.
    // Off unless HDR+ Bracketed is selected; never probes during video.
    void setSensitivityCalibrationWanted(bool wanted);
    // File keeping measured calibrations across launches (once per camera).
    void setSensitivityCalibrationStore(const std::string& path);
    void setExposureCompensationSteps(int32_t steps);
    // HDR+ Bracketed highlight headroom (EV <= 0), rounded to compensation
    // steps and added to the user's EV in Auto exposure. Lock-free: any
    // thread may call it; the camera lane applies it on its next tick.
    void setHdrHeadroomEv(float ev);
    // Headroom EV (<= 0) a delivered frame was exposed with: its applied
    // compensation minus the user's, bounded by the current/previous headroom
    // request. 0 for manual exposure or unknown compensation.
    float hdrHeadroomEvFor(int32_t aeMode, std::optional<int32_t> appliedSteps) const;
    // rawr auto exposure (opt-in, color/RawrAutoExposure.h). When enabled
    // and eligible (A/S/I mode, manual sensor control, stills, no spot AE,
    // known ISO coordinate) the repeating request runs AE OFF with the pair
    // the frame lane solves; S and I are then available on any camera with
    // manual sensor control. Lock-free for the frame lane: rawrAeContext and
    // setRawrAeRequest never wait on camera calls; the camera lane applies
    // requests on its tick.
    using RawrAeConfig = camera::RawrAeConfig;
    struct RawrAeContext {
        bool engaged = false;
        RawrAeConfig config;
        // 0 Auto, 1 shutter priority, 2 ISO priority (rawr_ae::Mode).
        int mode = 0;
        int64_t lockedShutterNs = 0;
        int32_t lockedIso = 0;
        // Effective solver limits, request ISO coordinate.
        int32_t isoMin = 0, isoMax = 0, isoAnalogMax = 0;
        int64_t shutterMinNs = 0, shutterMaxNs = 0, shutterStartNs = 0;
        // Sensor's longest exposure: bounds a user-locked S shutter, which the
        // handheld shutterMaxNs (Auto/I only) must not shorten.
        int64_t sensorShutterMaxNs = 0;
        int64_t flickerPeriodNs = 0;
        float userEv = 0.0f;
        // CaptureResult ISO / request ISO; 0 when unknown (taken as 1 without
        // sensor-mode keys; with them rawr AE waits for the probe).
        double reportedPerRequest = 0.0;
    };
    void setRawrAeConfig(const RawrAeConfig& config);
    [[nodiscard]] RawrAeContext rawrAeContext() const;
    void setRawrAeRequest(int64_t exposureTimeNs, int32_t sensitivity);
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
    // Hides the HAL's shutter/ISO priority modes so S/I locks fall back to Manual.
    void setAePriorityDisabled(bool disabled);
    void setVideoMode(bool video, int fps);
    void setShutterAngleDegrees(double degrees);
    bool setRecordingFps(int fps);
    // Diagnostic mirror of the SessionEngine ingress toggle so SELECTION logs
    // the effective per-session value. CameraSessionPolicy owns restarts.
    void setExperimentalZeroCopyEnabled(bool enabled);
    // Ingress reports every RAW image; the first one of a generation drops the
    // companion stream from the repeating request.
    void rawFrameArrived(uint64_t generation);
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
