#include "camera/NativeCameraController.h"

#include <android/log.h>
#include <camera/NdkCameraMetadataTags.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <set>
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
#include "camera/SensitivityProbeStartup.h"
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

// `adb shell setprop debug.rawr.camera_profile generic|<built-in id>`
// overrides the device's matched built-in profile (and any user lens profile)
// for bring-up on new devices.
const std::string& profileIdFromSetprop() {
    static const std::string overridden = [] {
        const std::string value = systemProperty("debug.rawr.camera_profile");
        return isBuiltInProfileId(value) ? value : std::string();
    }();
    return overridden;
}

ProfileMatch deviceProfileMatch() {
    return matchBuiltInProfile([](const std::string& name) { return systemProperty(name.c_str()); });
}

const std::string& activeProfileId(const ProfileMatch& match) {
    const auto& overridden = profileIdFromSetprop();
    return overridden.empty() ? match.profileId : overridden;
}

std::string describeRoute(const LensRoute& route) { return serializeCameraProfile({"", {route}}); }

}  // namespace

struct NativeCameraController::Impl final : CameraEventSink {
    explicit Impl(PreviewCallbacks cb)
        : callbacks(std::move(cb)),
          ndkCallbacks(*this),
          requests([this](const std::string& line) { diag(line); }),
          results([this](const std::string& line) { diag(line); }) {
        // Unverified: matched a model nobody has tested these lenses on yet.
        diag("CAMERA_PROFILE_BUILTIN profile=" + builtInMatch.profileId + " model=" + deviceModel +
             " rule=" + builtInMatch.rule + " verified=" + (builtInMatch.verified ? "true" : "false") +
             " override=" + (profileIdFromSetprop().empty() ? "none" : profileIdFromSetprop()));
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
    const ProfileMatch builtInMatch = deviceProfileMatch();
    CameraProfile profile = builtInCameraProfile(activeProfileId(builtInMatch));
    std::string preferredCameraId;
    // cameraId/physicalCameraId routes whose HAL rejected the companion YUV
    // stream; they start RAW-only for the rest of this process.
    std::set<std::string> rawOnlyRoutes;
    std::string accessRoute = "direct";
    bool oisEnabledPreference = true;
    // User preference to bypass Camera2 AE priority; the HAL's advertised
    // support is kept so the preference can be turned back off live.
    bool aePriorityDisabledPreference = false;
    bool halShutterPriority = false;
    bool halIsoPriority = false;
    uint8_t antibandingPreference = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO;
    NativeCameraController::RawrAeConfig rawrAeConfig;
    // Frame lane -> camera lane: latest solved pair packed as
    // exposureNs << 24 | ISO, so a tick never reads a torn pair (0 = none).
    std::atomic<uint64_t> rawrAePending{0};
    // Last mains flicker Camera2 detected (1 = 50 Hz, 2 = 60 Hz); sticky,
    // AE OFF frames may stop reporting it.
    std::atomic<int32_t> lastSceneFlicker{0};
    mutable std::mutex rawrAeContextMutex;  // never held across camera calls
    NativeCameraController::RawrAeContext rawrAePublished;
    int autoMinFpsPreference = 15;
    int recordingFpsPreference = 0;
    // Ingress toggle, mirrored from SessionEngine for diagnostics. The bridge
    // allocation / AHB reader usage is fixed at session configure time, so the
    // value logged at SELECTION is the one this session configures with —
    // provided the setter ran before start (guaranteed by the Kotlin
    // start-gate for the initial sync).
    bool experimentalZeroCopyEnabled = false;
    std::atomic<uint64_t> companionDetachGeneration{0};
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

    // Long frames are capped by the handheld shutter limit and the manual
    // gain range; Camera2 AE's own frames may go further with digital gain.
    static constexpr int64_t kLongFrameMaxNs = 66'666'667;  // OIS keeps 1/15 s sharp
    static constexpr double kMinLongFrameGainEv = 0.5;
    // Most a long frame can gain over a frame at (exposure, reported ISO),
    // in EV; nullopt when unknown (no session, unknown ISO coordinate).
    std::optional<double> longFrameRoomEvLocked(int64_t exposureTimeNs, int32_t reportedSensitivity) const {
        if (!deviceSession.hasSession() || !control.capabilities.manualExposureSupported || exposureTimeNs <= 0 ||
            reportedSensitivity <= 0)
            return std::nullopt;
        double sensitivity = reportedSensitivity;
        if (const auto ratio = results.sensitivityReportedPerRequest(); ratio && *ratio > 0.0)
            sensitivity /= *ratio;
        else if (deviceSession.sensorModeOverridden())
            return std::nullopt;
        const auto& caps = control.capabilities;
        const int64_t timeMax = std::max(caps.exposureTimeMinNs, std::min(exposureTimeNs, caps.exposureTimeMaxNs));
        const int64_t longTimeMax = std::max(timeMax, std::min(kLongFrameMaxNs, caps.exposureTimeMaxNs));
        const double isoMax = std::max(caps.sensitivityMin, caps.sensitivityMax);
        return std::log2(double(longTimeMax) * isoMax / (double(exposureTimeNs) * sensitivity));
    }
    std::optional<double> longFrameRoomEv() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (!control.appliedExposureTimeNs || !control.appliedSensitivity) return std::nullopt;
        return longFrameRoomEvLocked(*control.appliedExposureTimeNs, *control.appliedSensitivity);
    }

    bool captureExposureBracket(uint64_t requestId, int64_t baseExposureTimeNs, int32_t baseReportedSensitivity,
                                const std::vector<float>& evOffsets) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!deviceSession.hasSession() || !deviceSession.request() || !control.capabilities.manualExposureSupported ||
            baseExposureTimeNs <= 0 || baseReportedSensitivity <= 0 || evOffsets.empty()) {
            diag("CAMERA_BRACKET_REJECTED session=" + std::to_string(deviceSession.hasSession() ? 1 : 0) +
                 " manual=" + std::to_string(control.capabilities.manualExposureSupported ? 1 : 0));
            return false;
        }
        // CaptureResult ISO -> request coordinate (forced DCG modes report a
        // different scale; same conversion as the M/I mode seed).
        int32_t sensitivity = baseReportedSensitivity;
        if (const auto ratio = results.sensitivityReportedPerRequest(); ratio && *ratio > 0.0) {
            sensitivity = static_cast<int32_t>(std::llround(baseReportedSensitivity / *ratio));
        } else if (deviceSession.sensorModeOverridden()) {
            diag("CAMERA_BRACKET_REJECTED reportedPerRequest=unavailable dcg=on");
            return false;
        }
        const auto& caps = control.capabilities;
        const int32_t isoMin = caps.sensitivityMin, isoMax = std::max(caps.sensitivityMin, caps.sensitivityMax);
        const int64_t timeMin = caps.exposureTimeMinNs;
        // Never longer than the preview frame (motion blur, capture time).
        const int64_t timeMax = std::max(timeMin, std::min(baseExposureTimeNs, caps.exposureTimeMaxNs));
        // Long frames may exceed it, up to a handheld limit.
        const int64_t longTimeMax = std::max(timeMax, std::min(kLongFrameMaxNs, caps.exposureTimeMaxNs));
        std::vector<CameraRequestPipeline::BracketExposure> exposures;
        // Long frames the shutter/gain limits leave nearly as dark as the
        // burst add little shadow signal but still cost the user a 15 fps
        // hold: skip them (the merge stays a uniform burst merge). The shutter
        // usually checks first (longFrameRoomEv) and freezes a longer burst.
        double bestLongEv = 0.0;
        bool anyLong = false;
        for (const float ev : evOffsets) {
            if (ev > 0.0f) {
                // Longer shutter first at the reference gain (more light, not
                // more gain), then gain for what the handheld limit leaves.
                const double target = double(baseExposureTimeNs) * double(sensitivity) * std::exp2(ev);
                const int32_t baseIso = std::clamp(sensitivity, isoMin, isoMax);
                const int64_t time =
                    std::clamp<int64_t>(std::llround(target / double(baseIso)), timeMin, longTimeMax);
                const auto iso = static_cast<int32_t>(
                    std::clamp<double>(std::round(target / double(time)), double(isoMin), double(isoMax)));
                exposures.push_back({time, iso});
                anyLong = true;
                bestLongEv = std::max(bestLongEv, std::log2(double(time) * double(iso) /
                                                            (double(baseExposureTimeNs) * double(sensitivity))));
                continue;
            }
            // Lower gain first, down to base ISO: at high ISO highlights clip
            // in the gain stage, so the same shutter at lower gain keeps all
            // the light and only adds headroom. A shorter shutter takes the
            // rest. The preview ISO may exceed the manual maximum (AE digital
            // gain), so the product, not the ISO, is matched.
            const double target = double(baseExposureTimeNs) * double(sensitivity) * std::exp2(ev);
            const auto iso = static_cast<int32_t>(std::clamp<double>(
                std::round(target / double(timeMax)), double(isoMin), double(isoMax)));
            const int64_t time = std::clamp<int64_t>(std::llround(target / double(iso)), timeMin, timeMax);
            exposures.push_back({time, iso});
        }
        if (anyLong && bestLongEv < kMinLongFrameGainEv) {
            char gain[16];
            std::snprintf(gain, sizeof(gain), "%.2f", bestLongEv);
            diag(std::string("CAMERA_BRACKET_SKIPPED longEv=") + gain + " limit=exposure");
            return false;
        }
        return requests.captureBracket(deviceSession.session(), deviceSession.request(), deviceSession.callbackContext(),
                                       control, exposures, requestId);
    }

    // Calibration is part of lens startup, before any preview is published.
    SensitivityProbeStartup sensitivityStartup;
    bool sensitivityCalibrationWanted = false;
    // Reported/requested ISO ratios already measured, per camera, lens and
    // profile (its sensor mode keys). A device constant: later sessions on
    // the same camera skip the probe and show the viewfinder at once.
    std::map<std::string, double> sensitivityRatioCache;
    std::string sensitivityCacheKeyLocked() const {
        const auto& context = deviceSession.cameraContext();
        if (!context || !deviceSession.sensorModeOverridden()) return {};
        return context->cameraId + "/" + context->lensId + "/" + profile.id;
    }
    // Saved across launches so the probe runs once per camera, ever.
    std::string sensitivityStorePath;
    void loadSensitivityStoreLocked() {
        std::ifstream in(sensitivityStorePath);
        std::string key;
        double ratio = 0.0;
        while (in >> std::quoted(key) >> ratio)
            if (std::isfinite(ratio) && ratio > 0.0) sensitivityRatioCache[key] = ratio;
    }
    void saveSensitivityStoreLocked() const {
        if (sensitivityStorePath.empty()) return;
        const std::string temp = sensitivityStorePath + ".tmp";
        {
            std::ofstream out(temp, std::ios::trunc);
            out.precision(9);
            for (const auto& [key, ratio] : sensitivityRatioCache) out << std::quoted(key) << ' ' << ratio << '\n';
            if (!out) return;
        }
        std::rename(temp.c_str(), sensitivityStorePath.c_str());
    }
    // Seeds this session from the cache, or records a fresh measurement.
    void syncSensitivityCacheLocked() {
        const std::string key = sensitivityCacheKeyLocked();
        if (key.empty()) return;
        if (const auto measured = results.sensitivityReportedPerRequest()) {
            auto& cached = sensitivityRatioCache[key];
            if (std::abs(cached - *measured) > 1e-6) {
                cached = *measured;
                saveSensitivityStoreLocked();
            }
        } else if (const auto it = sensitivityRatioCache.find(key); it != sensitivityRatioCache.end()) {
            results.seedSensitivityRatio(it->second);
            diag("CAMERA_SENSITIVITY_RATIO_CACHED ratio=" + std::to_string(it->second));
        }
    }
    bool sensitivityProbeEligibleLocked() const {
        return sensitivityCalibrationWanted && deviceSession.sensorModeOverridden() &&
               control.capabilities.manualExposureSupported && !control.videoMode && control.recordingFps <= 0;
    }
    bool probeSensitivityCoordinateLocked() {
        if (!control.appliedExposureTimeNs || *control.appliedExposureTimeNs <= 0) return false;
        const int64_t exposure =
            std::clamp(*control.appliedExposureTimeNs, control.capabilities.exposureTimeMinNs,
                       std::max(control.capabilities.exposureTimeMinNs, control.capabilities.exposureTimeMaxNs));
        const auto& caps = control.capabilities;
        // Geometric midpoint avoids the manual endpoints that can be clamped.
        const int32_t probeIso = static_cast<int32_t>(std::llround(
            std::sqrt(double(std::max(1, caps.sensitivityMin)) * double(std::max(1, caps.sensitivityMax)))));
        const bool sent = requests.captureBracket(deviceSession.session(), deviceSession.request(),
                                                  deviceSession.callbackContext(), control,
                                                  {{exposure, probeIso}}, metadata::kSensitivityProbeRequestId);
        diag(std::string("CAMERA_SENSITIVITY_PROBE sent=") + (sent ? "1" : "0") + " exposureNs=" +
             std::to_string(exposure) + " iso=" + std::to_string(probeIso));
        return sent;
    }

    void restartForSensitivityCalibrationLocked(std::unique_lock<std::mutex>& lock) {
        syncSensitivityCacheLocked();
        if (!active || starting || !sensitivityStartup.finished() || !sensitivityProbeEligibleLocked() ||
            results.sensitivityReportedPerRequest()) return;
        // Enabling bracket mode after preview appeared must use a new startup,
        // never insert a calibration flash into the running viewfinder.
        active = false;
        if (!retireSessionLocked(lock, kStopRetireTimeout, "CAMERA_CALIBRATION_RESTART")) {
            forceRetireSessionLocked(lock, "CAMERA_CALIBRATION_RESTART");
        }
        teardownCameraResourcesLocked(lock);
        active = true;
        startLocked(lock);
    }

    bool submitRepeatingLocked() {
        publishHeadroomLocked();
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
        sensitivityStartup.reset();
        rawrAePending.store(0, std::memory_order_relaxed);
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
        halShutterPriority = control.capabilities.shutterPrioritySupported;
        halIsoPriority = control.capabilities.isoPrioritySupported;
        applyAePriorityPreferenceLocked();
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
        // Verified models stream RAW alone. Elsewhere a small YUV stream rides
        // along: some HALs (MediaTek) accept a RAW-only session but never
        // deliver a RAW buffer. `debug.rawr.companion_stream 0|1` overrides.
        const std::string companionOverride = systemProperty("debug.rawr.companion_stream");
        const std::string companionRouteKey = route.cameraId + '/' + route.physicalCameraId;
        const bool companionStream =
            companionOverride == "1" || (companionOverride != "0" && !builtInMatch.verified &&
                                         !rawOnlyRoutes.count(companionRouteKey));
        std::ostringstream selection;
        selection << "CAMERA_NDK_SELECTION profile=" << profile.id << " model=" << deviceModel
                  << " lensId=" << route.lensId << " cameraId=" << route.cameraId
                  << " physicalCameraId=" << (route.physicalCameraId.empty() ? "none" : route.physicalCameraId)
                  << " generation=" << deviceSession.generation() << " raw=" << route.stream.width << 'x'
                  << route.stream.height << " rawFormat=" << geometry::rawPixelFormatName(route.stream.format)
                  << " ingress=" << (experimentalZeroCopyEnabled ? "imported_AHB_STORAGE_direct" : "vkCmdCopyImage")
                  << " zeroCopy=" << (experimentalZeroCopyEnabled ? "true" : "false") << " accessRoute=" << accessRoute
                  << " companion=" << (companionStream ? 1 : 0);
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
        const auto stillCurrent = [&] {
            return active && !shutDown && setupGeneration == deviceSession.generation();
        };
        const bool opened = deviceSession.open(route, ndkCallbacks,
            [this](const std::string& line) { diag(line); }, lock, stillCurrent);
        // A stop or lens switch may have retired this start while open waited.
        // Its teardown owns the old window; do not roll back a newer generation.
        if (!stillCurrent()) return;
        if (!opened || !deviceSession.createSession(window, selected, control, meteringRequestLocked(),
                                         ndkCallbacks, requests, [this](const std::string& line) { diag(line); },
                                         companionStream)) {
            // Some HALs reject RAW + YUV at configure time (observed: physical
            // RAW16 + YUV on a Xiaomi logical camera). Retry this route once
            // RAW-only through a full reopen: the failed configure can take
            // the device down with it.
            if (opened && companionStream && companionOverride != "1" &&
                rawOnlyRoutes.insert(companionRouteKey).second) {
                rollbackCameraStartLocked(lock, "companion_session_rejected");
                if (shutDown) return;
                diag("CAMERA_COMPANION_FALLBACK route=" + companionRouteKey + " retry=raw_only");
                active = true;
                startLocked(lock);
                return;
            }
            rollbackCameraStartLocked(lock, "device_or_session");
            return;
        }
        starting = false;
        // The companion only has to start the pipeline. Producing it on every
        // frame holds each capture result back about a frame behind its RAW
        // image, which starves the preview pairer.
        companionDetachGeneration.store(companionStream ? deviceSession.generation() : 0, std::memory_order_release);
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
    void cameraCaptureFailed(uint64_t callbackGeneration) override {
        // Set once at construction; the receiver checks the generation.
        if (callbacks.captureFailed) callbacks.captureFailed(callbackGeneration);
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
        const auto& frame = *actions.frame;
        if (frame.sceneFlicker == ACAMERA_STATISTICS_SCENE_FLICKER_50HZ ||
            frame.sceneFlicker == ACAMERA_STATISTICS_SCENE_FLICKER_60HZ)
            lastSceneFlicker.store(frame.sceneFlicker, std::memory_order_relaxed);
        const bool autoSettled = frame.aeMode > ACAMERA_CONTROL_AE_MODE_OFF &&
            (frame.aeState == ACAMERA_CONTROL_AE_STATE_CONVERGED ||
             frame.aeState == ACAMERA_CONTROL_AE_STATE_LOCKED ||
             frame.aeState == ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED || frame.aeState < 0);
        const bool wasReady = sensitivityStartup.finished();
        syncSensitivityCacheLocked();
        const auto decision = sensitivityStartup.observe(frame, sensitivityProbeEligibleLocked(),
            results.sensitivityReportedPerRequest().has_value(), autoSettled, SensitivityProbeStartup::Clock::now());
        actions.frame->suppressPreview = decision.suppressPreview;
        if (decision.requestProbe && !probeSensitivityCoordinateLocked()) sensitivityStartup.submissionFailed();
        if (frame.optimizedStillRequestId == metadata::kSensitivityProbeRequestId)
            diag("CAMERA_SENSITIVITY_PROBE_RESULT requested=" + std::to_string(frame.requestedSensitivity.value_or(0)) +
                 " reported=" + std::to_string(frame.sensitivity) + " ratio=" +
                 std::to_string(results.sensitivityReportedPerRequest().value_or(0.0)));
        if (!wasReady && sensitivityStartup.finished())
            diag("CAMERA_CALIBRATION_PREVIEW_READY timeout=" + std::to_string(sensitivityStartup.timedOut()) +
                 " timestampNs=" + std::to_string(frame.timestampNs));
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
    float evStepEvLocked() const {
        const auto& caps = control.capabilities;
        return caps.evStepDenominator > 0 && caps.evStepNumerator > 0
                   ? float(caps.evStepNumerator) / float(caps.evStepDenominator)
                   : 0.0f;
    }
    // Frame-lane requests land here; the camera lane applies them on its tick.
    std::atomic<float> pendingHdrHeadroomEv{0.0f};
    void applyHdrHeadroomLocked() {
        // Stills only: video keeps Camera2 AE exposure.
        const bool video = control.videoMode || control.recordingFps > 0;
        const float ev = video ? 0.0f : pendingHdrHeadroomEv.load(std::memory_order_relaxed);
        const float step = evStepEvLocked();
        const int32_t target =
            step > 0.0f ? std::clamp(int32_t(std::lround(std::min(0.0f, ev) / step)),
                                     control.capabilities.evMinSteps - control.capabilities.evMaxSteps, 0)
                        : 0;
        if (target == control.hdrHeadroomSteps) return;
        // Glide at most 0.2 EV per tick (100 ms): each step re-converges AE a
        // little instead of all at once; the viewfinder normalizes per frame.
        const int32_t maxStep = step > 0.0f ? std::max(1, int32_t(std::lround(0.2f / step))) : 1;
        const int32_t steps = control.hdrHeadroomSteps +
                              std::clamp(target - control.hdrHeadroomSteps, -maxStep, maxStep);
        control.previousHdrHeadroomSteps = control.hdrHeadroomSteps;
        control.hdrHeadroomSteps = steps;
        if (steps == target) diag("CAMERA_CONTROL_REQUEST hdrHeadroomSteps=" + std::to_string(steps));
        if (control.exposureMode == ExposureControlMode::Auto && deviceSession.request() && deviceSession.session())
            submitRepeatingLocked();
        else
            publishHeadroomLocked();
    }
    // Lock-free mirror for the per-frame lift (the frame lane must not wait
    // on camera calls holding mutex). Published whenever these inputs change.
    std::atomic<int32_t> publishedUserEvSteps{0};
    std::atomic<int32_t> publishedHeadroomFloorSteps{0};
    std::atomic<float> publishedEvStepEv{0.0f};
    void publishHeadroomLocked() {
        publishedUserEvSteps.store(
            std::clamp(control.requestedEvSteps, control.capabilities.evMinSteps, control.capabilities.evMaxSteps),
            std::memory_order_relaxed);
        publishedHeadroomFloorSteps.store(std::min(control.hdrHeadroomSteps, control.previousHdrHeadroomSteps),
                                          std::memory_order_relaxed);
        publishedEvStepEv.store(evStepEvLocked(), std::memory_order_relaxed);
    }
    float hdrHeadroomEvFor(int32_t aeMode, std::optional<int32_t> appliedSteps) const {
        const int32_t floor = publishedHeadroomFloorSteps.load(std::memory_order_relaxed);
        if (floor >= 0 || !appliedSteps || aeMode == ACAMERA_CONTROL_AE_MODE_OFF) return 0.0f;
        const int32_t user = publishedUserEvSteps.load(std::memory_order_relaxed);
        return float(std::clamp(*appliedSteps - user, floor, 0)) * publishedEvStepEv.load(std::memory_order_relaxed);
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
    void applyAePriorityPreferenceLocked() {
        // rawr AE solves S/I itself wherever the sensor takes manual exposure.
        const bool rawrPriority = rawrAeConfig.enabled && control.capabilities.manualExposureSupported;
        control.capabilities.shutterPrioritySupported = (halShutterPriority || rawrPriority) && !aePriorityDisabledPreference;
        control.capabilities.isoPrioritySupported = (halIsoPriority || rawrPriority) && !aePriorityDisabledPreference;
        control.capabilities.halShutterPrioritySupported = halShutterPriority && !aePriorityDisabledPreference;
        control.capabilities.halIsoPrioritySupported = halIsoPriority && !aePriorityDisabledPreference;
    }
    void setAePriorityDisabledValue(bool disabled) {
        std::lock_guard<std::mutex> lock(mutex);
        if (aePriorityDisabledPreference == disabled) return;
        aePriorityDisabledPreference = disabled;
        applyAePriorityPreferenceLocked();
        // A running S/I mode the HAL may no longer own drops to Auto, as on restore.
        const bool priorityMode = control.exposureMode == ExposureControlMode::ShutterPriority ||
                                  control.exposureMode == ExposureControlMode::IsoPriority;
        if (disabled && priorityMode) control.exposureMode = ExposureControlMode::Auto;
        diag(std::string("CAMERA_AE_PRIORITY_PREFERENCE disabled=") + (disabled ? "true" : "false"));
        submitRepeatingLocked();
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
        std::unique_lock<std::mutex> lock(mutex);
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
        if (!video) restartForSensitivityCalibrationLocked(lock);
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
    bool rawrAeEligibleLocked() const {
        const bool coordinateKnown =
            results.sensitivityReportedPerRequest().has_value() || !deviceSession.sensorModeOverridden();
        const bool rawrMode = control.exposureMode == ExposureControlMode::Auto ||
                              control.exposureMode == ExposureControlMode::ShutterPriority ||
                              control.exposureMode == ExposureControlMode::IsoPriority;
        return rawrAeConfig.enabled && rawrMode &&
               control.capabilities.manualExposureSupported && control.capabilities.sensitivityMax > 0 &&
               !control.videoMode && control.recordingFps <= 0 && !control.spotAeActive && coordinateKnown &&
               deviceSession.request() && deviceSession.session();
    }
    void publishRawrAeContextLocked(bool engaged) {
        NativeCameraController::RawrAeContext c;
        c.engaged = engaged;
        c.config = rawrAeConfig;
        c.mode = control.exposureMode == ExposureControlMode::ShutterPriority ? 1
                 : control.exposureMode == ExposureControlMode::IsoPriority   ? 2
                                                                              : 0;
        c.lockedShutterNs = control.requestedExposureTimeNs;
        c.lockedIso = control.requestedSensitivity;
        const auto& caps = control.capabilities;
        const int32_t sensorIsoMin = std::max(1, caps.sensitivityMin);
        const int32_t sensorIsoMax = std::max(sensorIsoMin, caps.sensitivityMax);
        const auto& context = deviceSession.cameraContext();
        const int32_t analog = context && context->maxAnalogSensitivity > 0
                                   ? std::clamp(context->maxAnalogSensitivity, sensorIsoMin, sensorIsoMax)
                                   : sensorIsoMax;
        c.isoMin = rawrAeConfig.isoMin > 0 ? std::clamp(rawrAeConfig.isoMin, sensorIsoMin, sensorIsoMax) : sensorIsoMin;
        c.isoMax = rawrAeConfig.isoMax > 0 ? std::clamp(rawrAeConfig.isoMax, c.isoMin, sensorIsoMax) : sensorIsoMax;
        c.isoAnalogMax = std::clamp(analog, c.isoMin, c.isoMax);
        c.shutterMinNs = std::max<int64_t>(1, caps.exposureTimeMinNs);
        const int64_t sensorShutterMax = std::max(c.shutterMinNs, caps.exposureTimeMaxNs);
        int64_t ceiling = rawrAeConfig.shutterMaxNs;
        if (ceiling <= 0) {
            // Handheld ceiling (OIS keeps 1/15 s sharp), never below the Auto FPS floor's frame.
            ceiling = kLongFrameMaxNs;
            if (autoMinFpsPreference >= 5 && autoMinFpsPreference < 30)
                ceiling = std::min<int64_t>(ceiling, 1'000'000'000LL / autoMinFpsPreference);
        }
        c.shutterMaxNs = std::clamp(ceiling, c.shutterMinNs, sensorShutterMax);
        c.shutterStartNs = std::clamp<int64_t>(rawrAeConfig.shutterStartNs > 0 ? rawrAeConfig.shutterStartNs : 33'333'333,
                                               c.shutterMinNs, c.shutterMaxNs);
        const uint8_t banding = antibandingPreference;
        const int32_t mains = banding == ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ   ? 1
                              : banding == ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ ? 2
                              : banding == ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO
                                  ? lastSceneFlicker.load(std::memory_order_relaxed)
                                  : 0;
        c.flickerPeriodNs = mains == 1 ? 10'000'000 : mains == 2 ? 8'333'333 : 0;
        c.userEv = float(std::clamp(control.requestedEvSteps, caps.evMinSteps, caps.evMaxSteps)) * evStepEvLocked();
        c.reportedPerRequest = results.sensitivityReportedPerRequest().value_or(0.0);
        std::lock_guard<std::mutex> published(rawrAeContextMutex);
        rawrAePublished = c;
    }
    void applyRawrAeLocked() {
        const bool eligible = rawrAeEligibleLocked();
        publishRawrAeContextLocked(eligible);
        const uint64_t pending = rawrAePending.load(std::memory_order_relaxed);
        const auto exposure = static_cast<int64_t>(pending >> 24);
        const auto sensitivity = static_cast<int32_t>(pending & 0xFFFFFFu);
        const bool want = eligible && exposure > 0 && sensitivity > 0;
        // A pair solved before eligibility was lost is stale when it returns.
        if (!eligible) rawrAePending.store(0, std::memory_order_relaxed);
        if (!want) {
            if (control.rawrAeActive) {
                control.rawrAeActive = false;
                diag("CAMERA_RAWR_AE active=false");
                if (deviceSession.request() && deviceSession.session()) submitRepeatingLocked();
            }
            return;
        }
        if (control.rawrAeActive && control.rawrAeExposureTimeNs == exposure && control.rawrAeSensitivity == sensitivity)
            return;
        if (!control.rawrAeActive) diag("CAMERA_RAWR_AE active=true");
        control.rawrAeActive = true;
        control.rawrAeExposureTimeNs = exposure;
        control.rawrAeSensitivity = sensitivity;
        submitRepeatingLocked();
    }
    void setRawrAeConfigValue(const NativeCameraController::RawrAeConfig& config) {
        std::lock_guard<std::mutex> lock(mutex);
        const bool toggled = rawrAeConfig.enabled != config.enabled;
        rawrAeConfig = config;
        if (!config.enabled) rawrAePending.store(0, std::memory_order_relaxed);
        if (toggled) {
            diag(std::string("CAMERA_RAWR_AE_PREFERENCE enabled=") + (config.enabled ? "true" : "false"));
            // rawr owns S/I on cameras without HAL priority: the UI's modes change.
            applyAePriorityPreferenceLocked();
            const bool priorityMode = control.exposureMode == ExposureControlMode::ShutterPriority ||
                                      control.exposureMode == ExposureControlMode::IsoPriority;
            const bool supported = control.exposureMode == ExposureControlMode::ShutterPriority
                                       ? control.capabilities.shutterPrioritySupported
                                       : control.capabilities.isoPrioritySupported;
            if (priorityMode && !supported) control.exposureMode = ExposureControlMode::Auto;
        }
        applyRawrAeLocked();
    }
    void tickControls(int64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex);
        applyRawrAeLocked();
        applyHdrHeadroomLocked();
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
const CameraProfile& deviceBuiltInCameraProfile() { return builtInCameraProfile(activeProfileId(deviceProfileMatch())); }

bool NativeCameraController::setProfile(CameraProfile profile) { return impl_->setProfile(std::move(profile)); }
void NativeCameraController::setExperimentalZeroCopyEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->experimentalZeroCopyEnabled = enabled;
}
void NativeCameraController::rawFrameArrived(uint64_t generation) {
    uint64_t expected = generation;
    if (generation == 0 || impl_->companionDetachGeneration.load(std::memory_order_acquire) != generation ||
        !impl_->companionDetachGeneration.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
        return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->active || generation != impl_->deviceSession.generation()) return;
    if (impl_->deviceSession.detachCompanionTarget([this](const std::string& line) { impl_->diag(line); }))
        impl_->submitRepeatingLocked();
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
std::optional<double> NativeCameraController::longFrameRoomEv() const { return impl_->longFrameRoomEv(); }
bool NativeCameraController::longFramesWorthwhile() const {
    const auto room = impl_->longFrameRoomEv();
    return !room || *room >= Impl::kMinLongFrameGainEv;
}
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
bool NativeCameraController::captureExposureBracket(uint64_t requestId, int64_t baseExposureTimeNs,
                                                    int32_t baseReportedSensitivity, const std::vector<float>& evOffsets) {
    return impl_->captureExposureBracket(requestId, baseExposureTimeNs, baseReportedSensitivity, evOffsets);
}
void NativeCameraController::setSensitivityCalibrationStore(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sensitivityStorePath = path;
    impl_->loadSensitivityStoreLocked();
}
void NativeCameraController::setSensitivityCalibrationWanted(bool wanted) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool changed = impl_->sensitivityCalibrationWanted != wanted;
    impl_->sensitivityCalibrationWanted = wanted;
    if (changed && wanted) impl_->restartForSensitivityCalibrationLocked(lock);
}
void NativeCameraController::setExposureCompensationSteps(int32_t value) { impl_->setEvSteps(value); }
void NativeCameraController::setRawrAeConfig(const RawrAeConfig& config) { impl_->setRawrAeConfigValue(config); }
NativeCameraController::RawrAeContext NativeCameraController::rawrAeContext() const {
    std::lock_guard<std::mutex> lock(impl_->rawrAeContextMutex);
    return impl_->rawrAePublished;
}
void NativeCameraController::setRawrAeRequest(int64_t exposureTimeNs, int32_t sensitivity) {
    if (exposureTimeNs <= 0 || exposureTimeNs >= (int64_t(1) << 40) || sensitivity <= 0 || sensitivity > 0xFFFFFF)
        return;
    impl_->rawrAePending.store(uint64_t(exposureTimeNs) << 24 | uint64_t(sensitivity), std::memory_order_relaxed);
}
void NativeCameraController::setHdrHeadroomEv(float ev) {
    impl_->pendingHdrHeadroomEv.store(std::isfinite(ev) ? std::min(0.0f, ev) : 0.0f, std::memory_order_relaxed);
}
float NativeCameraController::hdrHeadroomEvFor(int32_t aeMode, std::optional<int32_t> appliedSteps) const {
    return impl_->hdrHeadroomEvFor(aeMode, appliedSteps);
}
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
void NativeCameraController::setAePriorityDisabled(bool disabled) { impl_->setAePriorityDisabledValue(disabled); }
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
