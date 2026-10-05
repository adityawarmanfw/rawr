#include "camera/NativeCameraController.h"

#include <android/log.h>
#include <camera/NdkCameraMetadataTags.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#include "camera/CameraCallbacks.h"
#include "camera/CameraControlCapabilities.h"
#include "camera/CameraDeviceSession.h"
#include "camera/CameraFocusControls.h"
#include "camera/CameraProfileJson.h"
#include "camera/CameraRequestPipeline.h"
#include "camera/CameraResultProcessor.h"
#include "camera/CameraWhiteBalanceControls.h"
#include "geometry/OrientationTransform.h"
#include "metadata/MetadataDiagnostics.h"

namespace rawrcam::camera {
namespace {
constexpr const char* kTag = "RawrCamCameraNDK";
#define CAMI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
#define CAME(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)

std::string systemProperty(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

// `adb shell setprop debug.rawr.camera_profile generic|v2562` overrides the
// model-based built-in profile (and any user lens profile) for bring-up on
// new devices.
const std::string& profileIdFromSetprop() {
    static const std::string overridden = [] {
        const std::string value = systemProperty("debug.rawr.camera_profile");
        return value == "generic" || value == "v2562" ? value : std::string();
    }();
    return overridden;
}

const std::string& activeProfileId(const std::string& model) {
    const auto& overridden = profileIdFromSetprop();
    return overridden.empty() ? builtInProfileIdForModel(model) : overridden;
}

std::string describeRoute(const LensRoute& route) { return serializeCameraProfile({"", {route}}); }

}  // namespace

struct NativeCameraController::Impl final : CameraEventSink {
    explicit Impl(PreviewCallbacks cb)
        : callbacks(std::move(cb)),
          ndkCallbacks(*this),
          requests([this](const std::string& line) { diag(line); }),
          results([this](const std::string& line) { diag(line); }) {
        publishProfileLocked();
    }
    ~Impl() {
        shutdown();
        ndkCallbacks.revoke();
    }
    CameraDeviceSession deviceSession;
    PreviewCallbacks callbacks;
    CameraCallbacks ndkCallbacks;
    CameraRequestPipeline requests;
    CameraResultProcessor results;

    CameraControlState control{};
    CameraFocusControls focusControls;
    CameraSpotMeteringControls spotMetering;
    CameraCadencePolicy cadencePolicy;
    mutable std::mutex mutex;
    // Bounded retire for worker-driven stop/start-failure paths. Healthy
    // retires complete in ~0.4s; beyond this the HAL is wedged (fatal device
    // error with no onClosed) and the caller force-retires instead of
    // blocking cameraWorker forever.
    static constexpr std::chrono::milliseconds kStopRetireTimeout{4000};
    bool active = false;
    bool starting = false;
    bool shutDown = false;
    // Empty or unknown lens ids resolve to the profile's first lens at start.
    std::string lensId;
    // Built-in profile for this device model until the app supplies the
    // user's lens profile (setProfile).
    const std::string deviceModel = systemProperty("ro.product.model");
    CameraProfile profile = builtInCameraProfile(activeProfileId(deviceModel));
    std::string preferredCameraId;
    std::string accessRoute = "direct";
    bool oisEnabledPreference = true;
    uint8_t antibandingPreference = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO;
    int autoMinFpsPreference = 15;
    int recordingFpsPreference = 0;
    // Ingress toggle, mirrored from SessionEngine for diagnostics. The bridge
    // allocation / AHB reader usage is fixed at session configure time, so the
    // value logged at SELECTION is the one this session configures with —
    // provided the setter ran before start (guaranteed by the Kotlin
    // start-gate for the initial sync).
    bool experimentalZeroCopyEnabled = false;
    void diag(const std::string& line) {
        CAMI("%s", line.c_str());
        if (callbacks.diagnostic) callbacks.diagnostic(line);
    }

    void setActive(bool value) {
        std::unique_lock<std::mutex> lock(mutex);
        if (active == value) return;
        active = value;
        if (active) {
            startLocked(lock);
            return;
        }
        // Bounded wait: after a fatal device error (e.g. ERROR_CAMERA_DEVICE
        // from a stop racing a start) onClosed may never arrive. An
        // indefinite wait wedged cameraWorker forever -> all later starts
        // queued behind it never ran -> black viewfinder with live UI until
        // process restart. Force retirement so the next start recovers.
        if (!retireSessionLocked(lock, kStopRetireTimeout, "CAMERA_STOP")) {
            forceRetireSessionLocked(lock, "CAMERA_STOP");
        }
        teardownCameraResourcesLocked(lock);
    }

    void setLens(const std::string& value) {
        std::unique_lock<std::mutex> lock(mutex);
        if (lensId == value) return;

        const bool restart = active;
        if (!restart) {
            lensId = value;
            return;
        }

        // Reject any late result from the retiring lens while its session drains.
        active = false;
        if (!retireSessionLocked(lock, std::chrono::seconds(3), "CAMERA_LENS_SWITCH")) {
            diag("CAMERA_LENS_SWITCH_ABORT from=" + lensId + " to=" + value + " reason=session_close_timeout");
            return;
        }

        // Session is now definitively closed. It is safe to release the remaining
        // old-camera resources and create an independent vendor-session context.
        teardownCameraResourcesLocked(lock);
        lensId = value;
        active = true;
        diag("CAMERA_LENS_SWITCH_RESTART lensId=" + lensId +
             " generation=" + std::to_string(deviceSession.generation()));
        startLocked(lock);
    }

    void publishProfileLocked() {
        control.capabilities.cameraProfileId = profile.id;
        control.capabilities.profileLenses.clear();
        for (const auto& lens : profile.lenses) control.capabilities.profileLenses.emplace_back(lens.lensId, lens.lensId);
    }

    // Replaces the lens profile. Restarts the camera only when the running
    // lens's route actually changed (or the lens is gone).
    bool setProfile(CameraProfile next) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!profileIdFromSetprop().empty()) {
            diag("CAMERA_PROFILE_USER_IGNORED reason=setprop_override profile=" + profile.id);
            return false;
        }
        const auto before = routeForLens(profile, lensId);
        profile = std::move(next);
        publishProfileLocked();
        auto after = routeForLens(profile, lensId);
        if (!after && !profile.lenses.empty()) after = profile.lenses.front();
        const bool routeChanged = !before || !after || describeRoute(*before) != describeRoute(*after);
        diag("CAMERA_PROFILE_APPLIED profile=" + profile.id + " lenses=" + std::to_string(profile.lenses.size()) +
             " lensId=" + lensId + " routeChanged=" + (routeChanged ? "true" : "false"));
        // One line per lens (logcat truncates long lines): enough to rebuild
        // the profile JSON from a diagnostics bundle or `adb logcat`.
        for (const auto& lens : profile.lenses) diag("CAMERA_PROFILE_LENS " + describeRoute(lens));
        if (!routeChanged || !active || !preferredCameraId.empty()) return true;
        active = false;
        if (!retireSessionLocked(lock, std::chrono::seconds(3), "CAMERA_PROFILE_SWITCH")) {
            diag("CAMERA_PROFILE_SWITCH_ABORT reason=session_close_timeout");
            return true;
        }
        teardownCameraResourcesLocked(lock);
        active = true;
        startLocked(lock);
        return true;
    }

    std::optional<LensRoute> selectedRouteLocked() const {
        auto route = !preferredCameraId.empty() ? std::optional<LensRoute>(routeForCameraId(profile, preferredCameraId))
                                                : routeForLens(profile, lensId);
        // `adb shell setprop debug.rawr.raw_format raw10|raw16` forces the
        // stream format (largest size) to exercise both ingress paths.
        const std::string forcedFormat = systemProperty("debug.rawr.raw_format");
        if (route && (forcedFormat == "raw10" || forcedFormat == "raw16")) {
            route->preferredStream = {
                forcedFormat == "raw10" ? geometry::RawPixelFormat::Raw10 : geometry::RawPixelFormat::Raw16, 0, 0};
        }
        return route;
    }

    bool submitRepeatingLocked() {
        return requests.submit(deviceSession.session(), deviceSession.request(), deviceSession.callbackContext(),
                               control, meteringRequestLocked());
    }

    CameraMeteringRequest meteringRequestLocked() const {
        return focusControls.request(spotMetering.region(
            control, deviceSession.cameraContext() ? &deviceSession.cameraContext()->geometry : nullptr));
    }

    void rollbackCameraStartLocked(std::unique_lock<std::mutex>& lock, const std::string& reason) {
        active = false;
        starting = false;
        if (deviceSession.hasSession()) {
            if (!retireSessionLocked(lock, kStopRetireTimeout, "CAMERA_START_FAILURE"))
                forceRetireSessionLocked(lock, "CAMERA_START_FAILURE");
        } else
            deviceSession.rollbackUnacceptedContext();
        teardownCameraResourcesLocked(lock);
        diag("CAMERA_NDK_START_ROLLBACK reason=" + reason);
    }

    void startLocked(std::unique_lock<std::mutex>& lock) {
        if (!active || deviceSession.hasDevice() || starting || shutDown) return;
        if (preferredCameraId.empty() && !routeForLens(profile, lensId) && !profile.lenses.empty()) {
            if (!lensId.empty())
                diag("CAMERA_LENS_FALLBACK from=" + lensId + " to=" + profile.lenses.front().lensId +
                     " reason=lens_not_in_profile");
            lensId = profile.lenses.front().lensId;
        }
        const auto requested = selectedRouteLocked();
        if (!requested) {
            diag("CAMERA_NDK_SELECTION_REJECT lensId=" + lensId + " profile=" + profile.id +
                 " reason=lens_not_in_profile");
            active = false;
            return;
        }
        requests.retire();
        results.reset();
        focusControls.reset(control);
        starting = true;
        const std::optional<LensRoute> selected =
            deviceSession.select(*requested, [this](const std::string& line) { diag(line); });
        if (!selected) {
            active = false;
            starting = false;
            return;
        }
        const LensRoute& route = *selected;
        // Session recreation (notably DCG ON/OFF) must not erase the user's
        // semantic exposure intent. Preserve request-side controls only when
        // reopening the same physical camera; a lens/camera change gets a fresh
        // control state because its legal ranges and capabilities may differ.
        const CameraControlState previousControl = control;
        const bool sameCameraRestart =
            !previousControl.capabilities.cameraId.empty() && previousControl.capabilities.cameraId == route.cameraId;
        control = deviceSession.initialControls(route);
        publishProfileLocked();
        control.focusRequestId = previousControl.focusRequestId;
        control.whiteBalanceRequestId = previousControl.whiteBalanceRequestId;

        if (sameCameraRestart) {
            const auto mode = previousControl.exposureMode;
            const bool modeSupported =
                mode == ExposureControlMode::Auto ||
                (mode == ExposureControlMode::Manual && control.capabilities.manualExposureSupported) ||
                (mode == ExposureControlMode::ShutterPriority && control.capabilities.shutterPrioritySupported) ||
                (mode == ExposureControlMode::IsoPriority && control.capabilities.isoPrioritySupported);
            control.exposureMode = modeSupported ? mode : ExposureControlMode::Auto;
            control.requestedExposureTimeNs =
                std::clamp(previousControl.requestedExposureTimeNs, control.capabilities.exposureTimeMinNs,
                           control.capabilities.exposureTimeMaxNs);
            control.requestedSensitivity =
                std::clamp(previousControl.requestedSensitivity, control.capabilities.sensitivityMin,
                           control.capabilities.sensitivityMax);
            control.requestedEvSteps = std::clamp(previousControl.requestedEvSteps, control.capabilities.evMinSteps,
                                                  control.capabilities.evMaxSteps);
            // Preserve white-balance intent across same-camera restarts (e.g.
            // DCG ON/OFF). A lens/camera change gets fresh WB state because
            // the newly read capabilities/seed may differ.
            const auto wbMode = previousControl.whiteBalanceMode;
            const bool wbSupported =
                wbMode == WhiteBalanceControlMode::Auto ||
                (wbMode == WhiteBalanceControlMode::ManualTempTint && control.capabilities.manualGainsSupported) ||
                std::find(control.capabilities.supportedAwbModes.begin(), control.capabilities.supportedAwbModes.end(),
                          static_cast<uint8_t>(wbMode)) != control.capabilities.supportedAwbModes.end();
            control.whiteBalanceMode = wbSupported ? wbMode : WhiteBalanceControlMode::Auto;
            control.requestedWhiteBalanceTemperatureK =
                std::clamp(previousControl.requestedWhiteBalanceTemperatureK, 2000, 10000);
            control.requestedWhiteBalanceTint = std::clamp(previousControl.requestedWhiteBalanceTint, -50, 50);
            // Same-sensor restart keeps the manual entry baseline so an
            // in-progress manual session doesn't snap to absolute gains.
            // A new camera gets fresh state from readInitial (above).
            control.manualWhiteBalanceEntryGains = previousControl.manualWhiteBalanceEntryGains;
            control.hasManualWhiteBalanceEntryGains = previousControl.hasManualWhiteBalanceEntryGains;
            control.manualWhiteBalanceEntryTempK = previousControl.manualWhiteBalanceEntryTempK;
            control.manualWhiteBalanceEntryTint = previousControl.manualWhiteBalanceEntryTint;
            control.focusMode = previousControl.focusMode;
            control.requestedManualFocusNormalized = previousControl.requestedManualFocusNormalized;
            diag("CAMERA_CONTROL_INTENT_RESTORED cameraId=" + route.cameraId +
                 " exposureMode=" + std::to_string(static_cast<int>(control.exposureMode)) +
                 " shutterNs=" + std::to_string(control.requestedExposureTimeNs) +
                 " sensitivity=" + std::to_string(control.requestedSensitivity) +
                 " wbMode=" + std::to_string(static_cast<int>(control.whiteBalanceMode)));
        }
        control.oisEnabled = oisEnabledPreference;
        control.requestedAntibandingMode = antibandingPreference;
        control.autoMinFps = std::clamp(autoMinFpsPreference, 5, 30);
        const auto fixedFpsSupported = [&] {
            return std::any_of(control.capabilities.aeTargetFpsRanges.begin(),
                               control.capabilities.aeTargetFpsRanges.end(), [&](const auto& range) {
                                   return range[0] == recordingFpsPreference && range[1] == recordingFpsPreference;
                               });
        };
        control.recordingFps = recordingFpsPreference > 0 && fixedFpsSupported() ? recordingFpsPreference : 0;
        cadencePolicy.setRecordingFps(control.recordingFps);
        if (!sameCameraRestart) cadencePolicy.clearShutterAngle();
        applyHeldShutterLocked();
        diag(describeCameraControlCapabilities(control));

        diag(metadata::describe(*deviceSession.cameraContext()));
        std::ostringstream selection;
        selection << "CAMERA_NDK_SELECTION profile=" << profile.id << " model=" << deviceModel
                  << " lensId=" << route.lensId << " cameraId=" << route.cameraId
                  << " physicalCameraId=" << (route.physicalCameraId.empty() ? "none" : route.physicalCameraId)
                  << " generation=" << deviceSession.generation() << " raw=" << route.stream.width << 'x'
                  << route.stream.height << " rawFormat=" << geometry::rawPixelFormatName(route.stream.format)
                  << " ingress=" << (experimentalZeroCopyEnabled ? "imported_AHB_STORAGE_direct" : "vkCmdCopyImage")
                  << " zeroCopy=" << (experimentalZeroCopyEnabled ? "true" : "false") << " accessRoute=" << accessRoute;
        diag(selection.str());
        const auto setupGeneration = deviceSession.generation();
        const auto setupContext = deviceSession.cameraContext();
        const auto setupCapabilities = control.capabilities;
        diag("CAMERA_START_CONFIGURE_BEGIN generation=" + std::to_string(setupGeneration));
        // Session-facing operations must never run under the camera mutex.
        lock.unlock();
        bool configured = false;
        ANativeWindow* window = nullptr;
        try {
            configured = callbacks.configurePreview &&
                         callbacks.configurePreview(setupGeneration, *setupContext, setupCapabilities);
            if (configured && callbacks.createRawWindow)
                window = callbacks.createRawWindow(setupGeneration, route.stream.width, route.stream.height,
                                                   route.stream.format);
        } catch (...) {
            configured = false;
        }
        lock.lock();
        if (!active || shutDown || setupGeneration != deviceSession.generation()) {
            lock.unlock();
            if (window && callbacks.destroyRawWindow) callbacks.destroyRawWindow(setupGeneration);
            lock.lock();
            return;
        }
        if (!configured || !window) {
            rollbackCameraStartLocked(lock, configured ? "create_raw_window" : "configure_preview");
            return;
        }
        deviceSession.setRawWindow(window);
        if (!deviceSession.open(route, ndkCallbacks, [this](const std::string& line) { diag(line); }) ||
            !deviceSession.createSession(window, selected, control, meteringRequestLocked(),
                                         ndkCallbacks, requests, [this](const std::string& line) { diag(line); })) {
            rollbackCameraStartLocked(lock, "device_or_session");
            return;
        }
        starting = false;
        diag("CAMERA_START_DONE generation=" + std::to_string(deviceSession.generation()));
    }

    bool retireSessionLocked(std::unique_lock<std::mutex>& lock, std::optional<std::chrono::milliseconds> timeout,
                             const char* prefix) {
        return deviceSession.retire(lock, timeout, prefix, [this](const std::string& line) { diag(line); });
    }

    // Last-resort recovery when onClosed never arrives (observed after
    // ACAMERA_ERROR_CAMERA_DEVICE from a stop racing a start). Orphans the
    // closing session: onSessionClosed only clears matching pointers and
    // deletes its own context, so a late arrival stays harmless, while the
    // leaked context is bounded to one small struct per wedge. Must be
    // followed by teardownCameraResourcesLocked() so the next start reopens
    // the device instead of no-op'ing on the leaked handle.
    void forceRetireSessionLocked(std::unique_lock<std::mutex>&, const char* prefix) {
        deviceSession.forceRetire(prefix, [this](const std::string& line) { diag(line); });
    }

    void teardownCameraResourcesLocked(std::unique_lock<std::mutex>& lock) {
        focusControls.reset(control);
        if (deviceSession.hasSession()) {
            diag("CAMERA_NDK_TEARDOWN_DEFERRED reason=session_not_retired");
            return;
        }
        const auto retired = deviceSession.release([this](const std::string& line) { diag(line); });
        requests.retire();
        starting = false;
        // Session cleanup takes its mutex. Generation scopes cleanup to its reader.
        lock.unlock();
        if (retired.destroy && callbacks.destroyRawWindow) callbacks.destroyRawWindow(retired.generation);
        lock.lock();
    }

    void shutdown() {
        std::unique_lock<std::mutex> lock(mutex);
        if (shutDown) return;
        shutDown = true;
        active = false;

        // Callback contexts retain CallbackLifetime, whose owner is revoked in
        // ~Impl. A broken HAL must not block app shutdown indefinitely.
        if (!retireSessionLocked(lock, kStopRetireTimeout, "CAMERA_SHUTDOWN")) {
            forceRetireSessionLocked(lock, "CAMERA_SHUTDOWN");
        }
        teardownCameraResourcesLocked(lock);
    }

    void steerAfToFacesLocked() {
        const auto change = focusControls.steerFaces(
            control, deviceSession.cameraContext() ? &deviceSession.cameraContext()->geometry : nullptr,
            deviceSession.request() && deviceSession.session());
        if (change == FaceFocusChange::None) return;
        submitRepeatingLocked();
        if (change == FaceFocusChange::Cleared) {
            diag("CAMERA_FACE_AF_REGION cleared reason=no_faces");
            return;
        }
        const auto& face = control.faceDetections.front();
        const auto region = focusControls.request(std::nullopt).afRegion;
        std::ostringstream d;
        d << "CAMERA_FACE_AF_REGION face=" << face.x << ',' << face.y << ',' << face.w << ',' << face.h
          << " score=" << static_cast<int>(face.score) << " region=" << (*region)[0] << ',' << (*region)[1] << ','
          << (*region)[2] << ',' << (*region)[3];
        diag(d.str());
    }

    void cameraDeviceEvent(uint64_t callbackGeneration, const std::string& message) override {
        // Diagnostic publication does not enter the session mutex.
        std::lock_guard<std::mutex> lock(mutex);
        if (callbackGeneration == deviceSession.generation()) diag(message);
    }
    void cameraSessionClosed(uint64_t callbackGeneration, ACameraCaptureSession* session) override {
        std::lock_guard<std::mutex> lock(mutex);
        diag("CAMERA_NDK_SESSION_CLOSED generation=" + std::to_string(callbackGeneration));
        deviceSession.sessionClosed(callbackGeneration, session);
    }
    void cameraCaptureCompleted(uint64_t callbackGeneration, const ACaptureRequest* requestCopy,
                                const ACameraMetadata* result) override {
        std::unique_lock<std::mutex> lock(mutex);
        results.auditCallback(callbackGeneration, deviceSession.generation(), active,
                              bool(deviceSession.cameraContext()), result != nullptr);
        // Validate before decoding a request's borrowed provenance pointer.
        if (!active || callbackGeneration != deviceSession.generation() || !deviceSession.cameraContext() || !result)
            return;
        auto actions = results.process(requestCopy, result, control, deviceSession.cameraContext(),
                                       deviceSession.staticLevels(), deviceSession.sensorModeOverridden(),
                                       requests.latestSubmittedSerial());
        if (!actions.frame) return;
        if (actions.fallbackToAuto) {
            control.exposureMode = ExposureControlMode::Auto;
            submitRepeatingLocked();
        }
        if (!actions.optimizedStill) steerAfToFacesLocked();
        if (focusControls.finishTrigger()) submitRepeatingLocked();
        lock.unlock();
        if (callbacks.submitMetadata) (void)callbacks.submitMetadata(*actions.frame);
        lock.lock();
        if (!active || callbackGeneration != deviceSession.generation()) return;
    }

    bool stillMetadataReady() {
        std::lock_guard<std::mutex> lock(mutex);
        return deviceSession.request() && deviceSession.session() && deviceSession.cameraContext();
    }

    CameraControlState getControlState() const {
        std::lock_guard<std::mutex> lock(mutex);
        CameraControlState snapshot = control;
        snapshot.videoMode = cadencePolicy.videoMode();
        snapshot.videoPreviewFps = cadencePolicy.videoFps();
        snapshot.requestedShutterAngleDegrees = cadencePolicy.shutterAngle();
        snapshot.shutterAngleChoices = cadencePolicy.shutterChoices(control.capabilities.exposureTimeMinNs,
                                                                    control.capabilities.exposureTimeMaxNs);
        publishWhiteBalanceEstimate(snapshot);
        return snapshot;
    }

    bool setExposureModeValue(ExposureControlMode mode) {
        std::lock_guard<std::mutex> lock(mutex);
        if (mode == ExposureControlMode::Manual && !control.capabilities.manualExposureSupported) {
            diag("CAMERA_CONTROL_REQUEST exposureMode=M rejected=unsupported");
            return false;
        }
        if (mode == ExposureControlMode::ShutterPriority && !control.capabilities.shutterPrioritySupported) {
            diag("CAMERA_CONTROL_REQUEST exposureMode=S rejected=unsupported");
            return false;
        }
        if (mode == ExposureControlMode::IsoPriority && !control.capabilities.isoPrioritySupported) {
            diag("CAMERA_CONTROL_REQUEST exposureMode=I rejected=unsupported");
            return false;
        }
        const auto sensitivityReportedPerRequest = results.sensitivityReportedPerRequest();
        if (mode != ExposureControlMode::Auto && control.exposureMode != mode) {
            // Seed application-owned axes from the most recently applied exposure.
            // Shutter is already in one coordinate. Sensitivity is not: forced DCG
            // can report roughly 8x the request coordinate, so convert it through the
            // provenance-learned ratio instead of copying CaptureResult ISO verbatim.
            if (control.appliedExposureTimeNs) {
                control.requestedExposureTimeNs =
                    std::clamp(*control.appliedExposureTimeNs, control.capabilities.exposureTimeMinNs,
                               control.capabilities.exposureTimeMaxNs);
            }
            if ((mode == ExposureControlMode::Manual || mode == ExposureControlMode::IsoPriority) &&
                control.appliedSensitivity) {
                if (sensitivityReportedPerRequest && *sensitivityReportedPerRequest > 0.0) {
                    const auto inferredRequestSensitivity = static_cast<int32_t>(std::llround(
                        static_cast<double>(*control.appliedSensitivity) / *sensitivityReportedPerRequest));
                    control.requestedSensitivity =
                        std::clamp(inferredRequestSensitivity, control.capabilities.sensitivityMin,
                                   control.capabilities.sensitivityMax);
                    diag("CAMERA_SENSITIVITY_COORDINATE_SEED reported=" + std::to_string(*control.appliedSensitivity) +
                         " request=" + std::to_string(control.requestedSensitivity) +
                         " reportedPerRequest=" + std::to_string(*sensitivityReportedPerRequest));
                } else if (!deviceSession.sensorModeOverridden()) {
                    control.requestedSensitivity =
                        std::clamp(*control.appliedSensitivity, control.capabilities.sensitivityMin,
                                   control.capabilities.sensitivityMax);
                } else {
                    diag("CAMERA_SENSITIVITY_COORDINATE_SEED retainedRequest=" +
                         std::to_string(control.requestedSensitivity) + " reported=" +
                         std::to_string(*control.appliedSensitivity) + " reportedPerRequest=unavailable dcg=on");
                }
            }
        }
        control.exposureMode = mode;
        if (mode != ExposureControlMode::Auto) control.spotAeActive = false;
        if (mode == ExposureControlMode::Manual || mode == ExposureControlMode::ShutterPriority) {
            cadencePolicy.bindShutter(control.requestedExposureTimeNs, control.capabilities.exposureTimeMinNs,
                                      control.capabilities.exposureTimeMaxNs);
            applyHeldShutterLocked();
        }
        const char* modeName = mode == ExposureControlMode::Manual            ? "M"
                               : mode == ExposureControlMode::ShutterPriority ? "S"
                               : mode == ExposureControlMode::IsoPriority     ? "I"
                                                                              : "A";
        diag(std::string("CAMERA_CONTROL_REQUEST exposureMode=") + modeName);
        submitRepeatingLocked();
        return true;
    }
    void setManualExposure(int64_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        cadencePolicy.clearShutterAngle();
        control.requestedExposureTimeNs = cadencePolicy.heldExposure(value, control.capabilities.exposureTimeMinNs,
                                                                     control.capabilities.exposureTimeMaxNs);
        diag("CAMERA_CONTROL_REQUEST shutterNs=" + std::to_string(control.requestedExposureTimeNs));
        submitRepeatingLocked();
    }
    void setManualIso(int32_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (control.capabilities.sensitivityMax > 0)
            control.requestedSensitivity =
                std::clamp(value, control.capabilities.sensitivityMin, control.capabilities.sensitivityMax);
        diag("CAMERA_CONTROL_REQUEST sensitivity=" + std::to_string(control.requestedSensitivity));
        submitRepeatingLocked();
    }
    void setEvSteps(int32_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        control.requestedEvSteps = std::clamp(value, control.capabilities.evMinSteps, control.capabilities.evMaxSteps);
        diag("CAMERA_CONTROL_REQUEST evSteps=" + std::to_string(control.requestedEvSteps));
        submitRepeatingLocked();
    }
    bool setWhiteBalanceModeValue(WhiteBalanceControlMode mode, int64_t requestId) {
        std::lock_guard<std::mutex> lock(mutex);
        const bool accepted = requestWhiteBalanceMode(control, mode, requestId);
        diag("CAMERA_CONTROL_REQUEST wbMode=" + std::to_string(static_cast<int>(mode)) +
             " accepted=" + (accepted ? "true" : "false"));
        if (accepted) submitRepeatingLocked();
        return accepted;
    }
    void setWhiteBalanceTempTint(int32_t temperatureK, int32_t tint, int32_t editedAxes, int64_t requestId) {
        std::lock_guard<std::mutex> lock(mutex);
        const bool accepted = requestWhiteBalanceTempTint(control, temperatureK, tint, editedAxes, requestId);
        diag("CAMERA_CONTROL_REQUEST wbTempK=" + std::to_string(control.requestedWhiteBalanceTemperatureK) +
             " wbTint=" + std::to_string(control.requestedWhiteBalanceTint) +
             " accepted=" + (accepted ? "true" : "false"));
        if (accepted) submitRepeatingLocked();
    }
    void setWhiteBalanceLockedValue(int32_t temperatureK, int32_t tint, int64_t requestId) {
        std::lock_guard<std::mutex> lock(mutex);
        const bool accepted = requestWhiteBalanceLocked(control, temperatureK, tint, requestId);
        diag("CAMERA_CONTROL_REQUEST wbLocked tempK=" + std::to_string(control.requestedWhiteBalanceTemperatureK) +
             " tint=" + std::to_string(control.requestedWhiteBalanceTint) +
             " accepted=" + (accepted ? "true" : "false"));
        if (accepted) submitRepeatingLocked();
    }
    void setOisEnabledValue(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex);
        oisEnabledPreference = enabled;
        control.oisEnabled = enabled;
        diag(std::string("CAMERA_OIS_PREFERENCE enabled=") + (enabled ? "true" : "false") +
             " supported=" + (control.capabilities.oisSupported ? "true" : "false"));
        submitRepeatingLocked();
    }

    void setAntibandingModeValue(uint8_t mode) {
        std::lock_guard<std::mutex> lock(mutex);
        if (mode > 3) mode = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO;
        if (antibandingPreference == mode) return;
        antibandingPreference = mode;
        control.requestedAntibandingMode = mode;
        diag("CAMERA_ANTIBANDING_PREFERENCE mode=" + std::to_string(mode));
        submitRepeatingLocked();
    }
    bool updatePreviewFloorLocked() {
        if (cadencePolicy.recording()) return false;
        const auto fps = std::clamp(cadencePolicy.previewFloor(), 5, 30);
        if (autoMinFpsPreference == fps) return false;
        autoMinFpsPreference = fps;
        control.autoMinFps = fps;
        diag("CAMERA_AUTO_MIN_FPS_PREFERENCE fps=" + std::to_string(fps));
        return true;
    }
    void setAutoMinFpsValue(int fps) {
        std::lock_guard<std::mutex> lock(mutex);
        cadencePolicy.setPhotoFloor(fps);
        if (updatePreviewFloorLocked()) submitRepeatingLocked();
    }
    bool holdsShutter() const {
        return control.exposureMode == ExposureControlMode::Manual ||
               control.exposureMode == ExposureControlMode::ShutterPriority;
    }
    void applyHeldShutterLocked() {
        if (!holdsShutter()) return;
        control.requestedExposureTimeNs =
            cadencePolicy.resolveHeldExposure(control.requestedExposureTimeNs, control.capabilities.exposureTimeMinNs,
                                              control.capabilities.exposureTimeMaxNs);
    }
    void setVideoModeValue(bool video, int fps) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto sanitizedFps = fps > 0 ? fps : 30;
        if (cadencePolicy.videoMode() == video && cadencePolicy.videoFps() == sanitizedFps) return;
        const bool enteringVideo = video && !cadencePolicy.videoMode();
        cadencePolicy.setVideoMode(video, fps);
        if (enteringVideo)
            cadencePolicy.bindShutter(control.requestedExposureTimeNs, control.capabilities.exposureTimeMinNs,
                                      control.capabilities.exposureTimeMaxNs);
        updatePreviewFloorLocked();
        applyHeldShutterLocked();
        submitRepeatingLocked();
    }
    void setShutterAngle(double degrees) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!holdsShutter() || !cadencePolicy.selectShutterAngle(degrees, control.capabilities.exposureTimeMinNs,
                                                                 control.capabilities.exposureTimeMaxNs))
            return;
        applyHeldShutterLocked();
        diag("CAMERA_CONTROL_REQUEST shutterAngle=" + std::to_string(degrees) +
             " shutterNs=" + std::to_string(control.requestedExposureTimeNs));
        submitRepeatingLocked();
    }
    bool setRecordingFpsValue(int fps) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fps == 0) {
            recordingFpsPreference = 0;
            control.recordingFps = 0;
            cadencePolicy.setRecordingFps(0);
            updatePreviewFloorLocked();
            applyHeldShutterLocked();
            if (deviceSession.session()) (void)submitRepeatingLocked();
            return true;
        }
        if (fps < 1 || control.capabilities.exposureTimeMinNs > 1'000'000'000LL / fps ||
            !std::any_of(control.capabilities.aeTargetFpsRanges.begin(), control.capabilities.aeTargetFpsRanges.end(),
                         [fps](const auto& range) { return range[0] == fps && range[1] == fps; })) {
            diag("CAMERA_RECORDING_FPS_UNSUPPORTED fps=" + std::to_string(fps));
            return false;
        }
        if (recordingFpsPreference == fps) return true;
        const int previous = recordingFpsPreference;
        const auto previousCadence = cadencePolicy;
        const auto previousExposure = control.requestedExposureTimeNs;
        recordingFpsPreference = fps;
        control.recordingFps = fps;
        cadencePolicy.setRecordingFps(fps);
        applyHeldShutterLocked();
        diag("CAMERA_RECORDING_FPS fps=" + std::to_string(fps));
        if (submitRepeatingLocked()) return true;
        recordingFpsPreference = previous;
        control.recordingFps = previous;
        cadencePolicy = previousCadence;
        control.requestedExposureTimeNs = previousExposure;
        (void)submitRepeatingLocked();
        return false;
    }
    void setSpotMeteringTarget(float nx, float ny, bool active) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!spotMetering.requestTarget(control, active, {nx, ny})) return;
        const auto point = spotMetering.point();
        if (active) {
            diag("CAMERA_AE_REGION_REQUEST active=true source=" + std::to_string(point.x) + "," +
                 std::to_string(point.y) + " maxRegions=" + std::to_string(control.capabilities.maxAeRegions));
        } else
            diag("CAMERA_AE_REGION_REQUEST active=false");
        submitRepeatingLocked();
    }

    bool setFocusModeValue(FocusControlMode mode, uint64_t requestId) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!focusControls.requestMode(control, mode, requestId)) return false;
        diag("CAMERA_CONTROL_REQUEST focusMode=" + std::to_string(static_cast<int>(mode)));
        if (mode == FocusControlMode::Continuous)
            diag("CAMERA_CONTROL_REQUEST tapAfCleared reason=focus_mode_continuous");
        submitRepeatingLocked();
        return true;
    }
    void setManualFocus(float normalized, uint64_t requestId) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!focusControls.requestManualFocus(control, normalized, requestId)) return;
        diag("CAMERA_CONTROL_REQUEST manualFocusNormalized=" + std::to_string(control.requestedManualFocusNormalized));
        submitRepeatingLocked();
    }
    void clearTapAf(uint64_t requestId) {
        std::lock_guard<std::mutex> lock(mutex);
        control.focusRequestId = std::max(control.focusRequestId, requestId);
        focusControls.clearTap(control);
        if (deviceSession.request() && deviceSession.session()) {
            diag("CAMERA_CONTROL_REQUEST tapAfCleared reason=explicit_clear");
            submitRepeatingLocked();
        }
    }
    void tickControls(int64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex);
        if (focusControls.expireTap(control, nowMs) && deviceSession.request() && deviceSession.session()) {
            diag("CAMERA_CONTROL_REQUEST tapAfCleared reason=explicit_clear");
            submitRepeatingLocked();
        }
    }
    void focusAt(float nx, float ny, uint64_t requestId) {
        std::lock_guard<std::mutex> lock(mutex);
        control.focusRequestId = std::max(control.focusRequestId, requestId);
        if (!std::isfinite(nx) || !std::isfinite(ny) || !deviceSession.request() || !deviceSession.cameraContext() ||
            !control.capabilities.tapAfSupported) {
            diag(std::string("CAMERA_CONTROL_REQUEST tapAfIgnored hasRequest=") +
                 (deviceSession.request() ? "true" : "false") +
                 " hasContext=" + (deviceSession.cameraContext() ? "true" : "false") +
                 " supported=" + (control.capabilities.tapAfSupported ? "true" : "false"));
            return;
        }
        const auto plan = focusControls.planTap(control, deviceSession.cameraContext()->geometry, {nx, ny});
        if (!plan) {
            diag("CAMERA_CONTROL_REQUEST tapAfIgnored reason=bad_crop");
            return;
        }
        // Keep the two submissions ordered: CANCEL the existing trigger first,
        // then install the new region/START using the same controller lock.
        if (plan->cancelFirst) {
            focusControls.cancelTrigger();
            submitRepeatingLocked();
        }
        focusControls.acceptTap(
            control, *plan,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        std::ostringstream d;
        d << "CAMERA_CONTROL_REQUEST tapAfSensor=" << std::clamp(nx, 0.0f, 1.0f) << ',' << std::clamp(ny, 0.0f, 1.0f)
          << " region=" << plan->region[0] << ',' << plan->region[1] << ',' << plan->region[2] << ','
          << plan->region[3];
        diag(d.str());
        submitRepeatingLocked();
    }
};

NativeCameraController::NativeCameraController(PreviewCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
NativeCameraController::~NativeCameraController() = default;
void NativeCameraController::setActive(bool active) { impl_->setActive(active); }
void NativeCameraController::setLensId(const std::string& lensId) { impl_->setLens(lensId); }
bool NativeCameraController::setProfile(CameraProfile profile) { return impl_->setProfile(std::move(profile)); }
void NativeCameraController::setExperimentalZeroCopyEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->experimentalZeroCopyEnabled = enabled;
}
void NativeCameraController::setPreferredCameraId(const std::string& id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->preferredCameraId = id;
}
void NativeCameraController::setAccessRoute(const std::string& route) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->accessRoute = route.empty() ? "direct" : route;
}
CameraControlState NativeCameraController::controlState() const { return impl_->getControlState(); }
int NativeCameraController::videoRotationDegrees(int deviceRotationDegrees) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->deviceSession.cameraContext() || impl_->deviceSession.cameraContext()->lensFacing < 0) return -1;
    return geometry::videoRotationDegrees(
        impl_->deviceSession.cameraContext()->geometry.sensorOrientationDegrees, deviceRotationDegrees,
        impl_->deviceSession.cameraContext()->lensFacing == ACAMERA_LENS_FACING_FRONT);
}
bool NativeCameraController::setExposureMode(ExposureControlMode mode) { return impl_->setExposureModeValue(mode); }
void NativeCameraController::setManualExposureTimeNs(int64_t value) { impl_->setManualExposure(value); }
void NativeCameraController::setManualSensitivity(int32_t value) { impl_->setManualIso(value); }
void NativeCameraController::setExposureCompensationSteps(int32_t value) { impl_->setEvSteps(value); }
bool NativeCameraController::setWhiteBalanceMode(WhiteBalanceControlMode mode, int64_t requestId) {
    return impl_->setWhiteBalanceModeValue(mode, requestId);
}
void NativeCameraController::setWhiteBalanceTempTint(int32_t temperatureK, int32_t tint, int32_t editedAxes,
                                                     int64_t requestId) {
    impl_->setWhiteBalanceTempTint(temperatureK, tint, editedAxes, requestId);
}
void NativeCameraController::setWhiteBalanceLocked(int32_t temperatureK, int32_t tint, int64_t requestId) {
    impl_->setWhiteBalanceLockedValue(temperatureK, tint, requestId);
}
void NativeCameraController::setOisEnabled(bool enabled) { impl_->setOisEnabledValue(enabled); }
void NativeCameraController::setAntibandingMode(uint8_t mode) { impl_->setAntibandingModeValue(mode); }
void NativeCameraController::setAutoMinFps(int fps) { impl_->setAutoMinFpsValue(fps); }
void NativeCameraController::setVideoMode(bool video, int fps) { impl_->setVideoModeValue(video, fps); }
void NativeCameraController::setShutterAngleDegrees(double degrees) { impl_->setShutterAngle(degrees); }
void NativeCameraController::tickControls(int64_t nowMs) { impl_->tickControls(nowMs); }
bool NativeCameraController::setRecordingFps(int fps) {
    // Camera route/DCG setup can expose control capabilities before the first
    // repeating request is accepted. Retry a bounded number of times without
    // holding the camera mutex; unsupported FPS still returns false.
    for (int attempt = 0; attempt < (fps > 0 ? 8 : 1); ++attempt) {
        if (impl_->setRecordingFpsValue(fps)) return true;
        if (fps > 0 && attempt < 7) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
}
void NativeCameraController::setSpotMeteringTargetSensorNormalized(bool active, float x, float y) {
    impl_->setSpotMeteringTarget(x, y, active);
}
bool NativeCameraController::setFocusMode(FocusControlMode mode, uint64_t requestId) {
    return impl_->setFocusModeValue(mode, requestId);
}
void NativeCameraController::focusAtSensorNormalized(float x, float y, uint64_t requestId) {
    impl_->focusAt(x, y, requestId);
}
void NativeCameraController::acknowledgeFocusRequest(uint64_t requestId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->control.focusRequestId = std::max(impl_->control.focusRequestId, requestId);
}
void NativeCameraController::clearTapAf(uint64_t requestId) { impl_->clearTapAf(requestId); }
void NativeCameraController::setManualFocusNormalized(float value, uint64_t requestId) {
    impl_->setManualFocus(value, requestId);
}
bool NativeCameraController::stillCaptureMetadataReady() { return impl_->stillMetadataReady(); }
void NativeCameraController::shutdown() { impl_->shutdown(); }

}  // namespace rawrcam::camera
