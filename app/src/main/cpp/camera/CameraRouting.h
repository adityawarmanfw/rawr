#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "camera/CameraWorkarounds.h"
#include "camera/CompanionStreamSize.h"
#include "color/DcpProfile.h"
#include "geometry/LensDistortion.h"
#include "geometry/RawGeometry.h"

namespace rawrcam::camera {

// How long the app keeps a vendor gate (LensRoute::vendorGate) held.
enum class GateHold : uint8_t {
    UntilConfigured,  // released as soon as the session is configured
    UntilClosed,      // kept until the device closes
};

// Which camera API runs a lens. Operating mode and stream use case have no NDK equivalent, so a lens that
// sets either runs on Java Camera2; "auto" picks that, anything else stays on the native API.
enum class CameraApi : uint8_t { Auto, Native, Java };

[[nodiscard]] constexpr const char* cameraApiName(CameraApi b) noexcept {
    return b == CameraApi::Native ? "native" : b == CameraApi::Java ? "java" : "auto";
}

// What the session-parameter request carries when the session is created.
enum class SessionParamsPolicy : uint8_t {
    Cadence,  // the AE target fps range and the session-scope keys (the default)
    None,     // no session parameters at all: a plain createCaptureSession
    Full,     // the whole repeating request: defaults, controls and the keys of both scopes
};

// Which request defaults the app writes into every frame request.
enum class RequestDefaults : uint8_t {
    Rawr,     // the app's defaults (the default)
    Minimal,  // leaves the processing overrides to the template: distortion and hot-pixel modes, the hot-pixel
              // map, face detection and the AE priority tag. Controls, shading map and keys stay.
};

[[nodiscard]] constexpr const char* sessionParamsName(SessionParamsPolicy p) noexcept {
    return p == SessionParamsPolicy::None ? "none" : p == SessionParamsPolicy::Full ? "full" : "cadence";
}
[[nodiscard]] constexpr const char* requestDefaultsName(RequestDefaults d) noexcept {
    return d == RequestDefaults::Minimal ? "minimal" : "rawr";
}

// One Camera2 metadata entry to inject. The tag is a metadata name (public or
// vendor, e.g. "vivo.control.forceSensorMode") or a numeric id ("0x80020000").
// Session keys go into the session parameters of every capture session;
// request keys go into every capture request (stills and video both come from
// the repeating RAW request).
struct CameraKeySetting {
    enum class Type : uint8_t { Byte, Int32, Int64, Float, Double };
    // Still: only on full-size still requests (LensRoute::photoStream), never
    // on the session or the repeating request. Decision: only on the one-shot
    // frame sent before those stills (see CameraDeviceSession::submitFullSizeDecision).
    // SessionAndRequest: the same value in the session parameters and on every capture request, for a
    // mode whose HAL reads the key when it configures streams and again per request.
    enum class Scope : uint8_t { Session, Request, Still, Decision, SessionAndRequest };
    std::string tag;
    Type type = Type::Int32;
    Scope scope = Scope::Session;
    std::vector<double> values;
    bool enabled = true;
    // A value computed per capture instead of fixed: StampMs is the capture's millisecond stamp plus
    // generatorOffset. Only gate presets (VendorGatePreset.h) use it; it is not part of the profile JSON.
    enum class Generator : uint8_t { None, StampMs };
    Generator generator = Generator::None;
    double generatorOffset = 0;
};

// Does a key declared with `declared` go out when the keys of `phase` (Session, Request, Still or
// Decision) are submitted?
[[nodiscard]] constexpr bool keyAppliesIn(CameraKeySetting::Scope declared, CameraKeySetting::Scope phase) noexcept {
    using Scope = CameraKeySetting::Scope;
    return declared == phase ||
           (declared == Scope::SessionAndRequest && (phase == Scope::Session || phase == Scope::Request));
}

// Black/white levels for a lens. Dynamic uses the camera's characteristics
// and per-frame dynamic levels; Static replaces both on every frame (needed
// when a vendor sensor mode changes the readout bit depth).
struct LevelOverride {
    bool isStatic = false;
    std::array<float, 4> blackRggb{0, 0, 0, 0};
    float white = 0.0f;
};

// Opt-in requests beyond the advertised ISO / exposure-time ranges (some HALs
// accept them). Zero keeps the advertised bound. The shutter is capped at
// kExtendedShutterMaxNs; longer than the viewfinder cap it runs only on capture.
struct ExposureOverride {
    bool extended = false;
    int32_t isoMax = 0;
    int64_t shutterMaxNs = 0;
};
inline constexpr int64_t kExtendedShutterMaxNs = 25'000'000'000LL;
// AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, the only usage bit the vivo full-size
// RAW modes accept besides none (profile buffers "usage": "gpuDataBuffer").
inline constexpr uint64_t kReaderUsageGpuDataBuffer = 1ull << 24;
inline constexpr int32_t kExtendedIsoMax = 409'600;

// How one UI lens maps onto the device. A camera profile is the full set of
// these for a device: either a built-in default or the user's configuration.
struct LensRoute {
    std::string lensId;            // App lens id; also the capture-screen label.
    std::string cameraId;          // Camera2 id to open (logical, or a hidden/physical id).
    std::string physicalCameraId;  // Physical sub-camera of cameraId to stream from; empty for none.
    geometry::RawStreamPreference preferredStream;
    // The stream actually configured. Zero until CameraDeviceSession::select
    // negotiates it against the camera's advertised RAW outputs.
    geometry::RawStreamOption stream;
    std::vector<CameraKeySetting> vendorKeys;
    LevelOverride levels;
    // Absent inherits lens levels; explicit dynamic uses this stream's sensor result.
    std::optional<LevelOverride> viewfinderLevels;
    std::optional<LevelOverride> photoLevels;
    ExposureOverride exposure;
    // Full-size vendor modes stream a size the camera doesn't advertise: use
    // preferredStream as is instead of negotiating it against the RAW outputs.
    bool highResLive = false;
    // AHB usage for the RAW reader instead of the ingress ladder's choice; the
    // vivo full-size modes configure only without CPU-read / sampled-image bits.
    std::optional<uint64_t> rawBufferUsage;
    CameraApi cameraApi = CameraApi::Auto;
    // Camera2 session operating mode (SessionConfiguration type); 0 = the normal mode. Java Camera2 only.
    uint32_t opMode = 0;
    // Stream use case per output (OutputConfiguration.setStreamUseCase); 0 = not set. Java Camera2 only.
    int64_t rawStreamUseCase = 0;
    int64_t photoStreamUseCase = 0;
    int64_t companionStreamUseCase = 0;
    int64_t auxJpegStreamUseCase = 0;
    // What the session parameters carry; None conflicts with session-scope keys, which need them.
    SessionParamsPolicy sessionParams = SessionParamsPolicy::Cadence;
    RequestDefaults requestDefaults = RequestDefaults::Rawr;
    // Provider behaviours this lens avoids, on top of the device's (BuiltInProfile::workarounds).
    CameraWorkarounds workarounds;
    // Vendor state the app holds while the device is open (see VendorCameraGate).
    std::string vendorGate;
    // How long the gate is held; absent: until the device closes on a capture-stream route, else until configured.
    std::optional<GateHold> gateHold;
    // Full-size stills from a second RAW stream that only still requests fill
    // (vivo seamless route): `stream` is then the binned viewfinder, and the
    // HAL switches the sensor to full size for each still. Unlisted size.
    std::optional<geometry::RawStreamPreference> photoStream;
    // Most full-size stills one merge may request (1 = none: the HAL fails
    // bursts); 0 = the app's own cap.
    uint32_t mergeFrameLimit = 0;
    // Submit a burst one still at a time, each after the previous frame is in
    // (the HAL fails queued full-size stills but takes them back to back).
    bool mergeFramesOneByOne = false;
    // AHB usage for the photoStream reader; nullopt keeps GPU_DATA_BUFFER.
    std::optional<uint64_t> photoBufferUsage;
    // Explicit recipe template shared by session, repeating and still requests.
    std::optional<bool> stillTemplate;  // Absent: preview repeating, still capture (legacy).
    // Fixed AE target fps range on the session and every request; 0 = the app's cadence.
    int32_t frameRate = 0;
    // Required companion size; the format and request targeting are explicit.
    // Recovery must never remove this output from the recipe.
    std::optional<std::pair<uint32_t, uint32_t>> companionSize;
    // Explicit recipe opt-out, including the automatic capture-stream companion.
    bool disableCompanion = false;
    bool companionPrivate = false;
    bool companionTargeted = true;
    // The companion's own reader: AHB usage (absent: PRIVATE asks for GPU sampling, YUV for the reader default),
    // and how many images it holds (0: 2).
    std::optional<uint64_t> companionBufferUsage;
    uint32_t companionBufferCount = 0;
    // Companion output tied to the route's physical camera; absent: it follows the route.
    std::optional<bool> companionPhysical;
    // Automatic size only: what to do when the camera has no output with the RAW's shape.
    CompanionFallback companionFallback = CompanionFallback::Base;
    // Drop the companion from the repeating request after the first accepted frame; absent: only an
    // automatic companion is dropped, a profile-sized one stays.
    std::optional<bool> companionDetach;
    int32_t startupDelayMs = 0;
    // Pause the viewfinder for the still sequence, then restore it.
    bool stopViewfinderForPhotos = false;
    // Extra JPEG output configured with the session but never targeted.
    std::optional<std::pair<uint32_t, uint32_t>> auxJpegSize;
    std::optional<uint64_t> auxJpegBufferUsage;  // absent: the reader default
    // The viewfinder RAW reader: images it holds (0: the app's 6; 6..16 allowed), whether a profile-fixed
    // usage is checked with AHardwareBuffer_isSupported before the reader is created (CPU-only when it
    // fails), and whether a fixed reader that stays silent takes part in the usage ladder.
    uint32_t rawBufferCount = 0;
    bool rawBufferTestFirst = false;
    bool rawBufferFallBack = false;
    // Sensor colour layout when the camera reports it wrong. sensorLayoutBlock: side of
    // the same-colour blocks the RAW actually holds (1 Bayer, 2 quad, 4 for
    // 4x4); ingress remosaics them to Bayer. colourOrder: block-level colour
    // order (CfaPattern code) replacing the camera's; nullopt keeps it.
    uint32_t sensorLayoutBlock = 1;
    std::optional<uint32_t> colourOrder;
    // Remosaic quality (imaging::CfaRemosaicQuality): live = viewfinder, video
    // and merge frames (GPU: Fast 0 / Standard 1); photo = single stills
    // (CPU: Standard 1 / High 2).
    uint32_t liveRemosaic = 1;
    uint32_t photoRemosaic = 2;
    // Lens distortion calibration when the camera reports none (vendor
    // calibration): undistorts photo renders and goes into the DNG as a
    // WarpRectilinear opcode. Defined on its own frame size, scaled to others.
    std::optional<geometry::LensDistortionModel> distortion;
    // Mirror of the corrected image (geometry::kLensMirror* bits): photo,
    // video and viewfinder flip it, the DNG composes it into Orientation.
    uint32_t mirror = 0;
    // Camera profile (.dcp) replacing the camera's colour calibration for the
    // DNG and the render: its file, and the profile read from it (null when
    // the file is missing or unusable; dcpError says why, the lens still opens).
    std::string dcpPath;
    color::DcpProfilePtr dcp;
    std::string dcpError;
};

// Operating mode and stream use case exist only on the Java API.
[[nodiscard]] inline bool needsJavaCameraApi(const LensRoute& lens) noexcept {
    return lens.opMode != 0 || lens.rawStreamUseCase != 0 || lens.photoStreamUseCase != 0 ||
           lens.companionStreamUseCase != 0 || lens.auxJpegStreamUseCase != 0;
}

// What Java Camera2 does not take on: stopping the viewfinder for photos (the vivo decision sequence), proven on the NDK
// only. A vendor gate is taken by the native session before either API opens the camera. nullptr: no conflict.
[[nodiscard]] inline const char* javaCameraApiConflict(const LensRoute& lens) noexcept {
    if (lens.stopViewfinderForPhotos) return "stopViewfinderForPhotos";
    return nullptr;
}

// `debug.rawr.camera_api java|native`: runs every lens on one API, for comparing the two on the same route.
// Lenses the forced API can't run keep their own: a lens with stop-repeating stills stays on the NDK, a lens with an operating mode
// or use case stays on Java. Returns what happened for the log ("" when no override is set).
[[nodiscard]] inline std::string applyCameraApiOverride(LensRoute& lens, const std::string& value) {
    if (value == "java") {
        if (const char* conflict = javaCameraApiConflict(lens)) return std::string("kept reason=") + conflict;
        lens.cameraApi = CameraApi::Java;
        return "java";
    }
    if (value == "native") {
        if (needsJavaCameraApi(lens)) return "kept reason=operating mode or use case";
        lens.cameraApi = CameraApi::Native;
        return "native";
    }
    return value.empty() ? std::string() : "ignored value=" + value;
}

[[nodiscard]] inline CameraApi effectiveCameraApi(const LensRoute& lens) noexcept {
    if (lens.cameraApi != CameraApi::Auto) return lens.cameraApi;
    return needsJavaCameraApi(lens) ? CameraApi::Java : CameraApi::Native;
}

// True when the lens carries vendor keys or any recipe field beyond a plain RAW stream.
inline bool carriesRecipe(const LensRoute& route) {
    return !route.vendorKeys.empty() || route.photoStream || route.companionSize || route.auxJpegSize || route.frameRate > 0 ||
           route.stillTemplate || route.startupDelayMs > 0 || route.disableCompanion || route.companionPrivate ||
           !route.companionTargeted || route.stopViewfinderForPhotos || route.rawBufferUsage || route.photoBufferUsage ||
           !route.vendorGate.empty() || route.highResLive || route.workarounds.any() || route.gateHold ||
           route.sessionParams != SessionParamsPolicy::Cadence || route.requestDefaults != RequestDefaults::Rawr ||
           route.companionBufferUsage || route.companionBufferCount || route.companionPhysical || route.companionDetach ||
           route.companionFallback != CompanionFallback::Base || route.auxJpegBufferUsage || route.rawBufferCount ||
           route.rawBufferTestFirst || route.rawBufferFallBack || route.cameraApi != CameraApi::Auto || needsJavaCameraApi(route);
}

// What a lens avoids: its own workarounds and the presets the lens's shape implies, so imported and
// user-edited profiles keep the protection a vendor mode needs. Quirks live on lenses only, so the
// lens editor shows everything the camera runs with.
// A vendor gate implies holdTapFocus; an unlisted (vendor full-size) stream implies the rest except noTapFocus:
// Xiaomi full-size lenses focus fine, and tap AF is a lens decision (add the noTapFocus quirk to opt out).
inline CameraWorkarounds effectiveWorkarounds(const LensRoute& lens) {
    CameraWorkarounds workarounds = lens.workarounds;
    if (!lens.vendorGate.empty()) workarounds.holdTapFocus = true;
    if (lens.highResLive) {
        workarounds.appPriorityAe = true;
        workarounds.alwaysSendFrameRate = true;
        workarounds.noAutoCompanion = true;
    }
    return workarounds;
}

inline bool companionStreamEnabled(const LensRoute& route, bool forced, bool automatic) {
    return !route.disableCompanion && (route.photoStream || route.companionSize || forced || automatic);
}

inline bool requiresStillExposureGuard(const LensRoute& route) {
    return route.photoStream && (route.companionSize || route.disableCompanion);
}

inline std::optional<LevelOverride> staticLevelsForStream(const LensRoute& route, bool capture) {
    const auto& override = capture ? route.photoLevels : route.viewfinderLevels;
    const auto& levels = override ? *override : route.levels;
    return levels.isStatic ? std::optional<LevelOverride>(levels) : std::nullopt;
}

struct CameraProfile {
    std::string id;
    std::vector<LensRoute> lenses;
};

// Which devices a built-in profile serves. Exact ro.product.model values win
// over vendor property prefixes, which catch unlisted regional variants.
struct ProfileMatchRules {
    std::vector<std::string> models;
    std::vector<std::string> verifiedModels;  // Models the profile was tested on.
    std::vector<std::pair<std::string, std::string>> propertyPrefixes;  // (property, value prefix)
};

struct BuiltInProfile {
    CameraProfile profile;
    std::string name;  // Marketing name, for logs.
    int version = 1;   // Bumped when the defaults change.
    ProfileMatchRules match;
};

// The built-in profile chosen for a device, and why.
struct ProfileMatch {
    std::string profileId = "generic";
    std::string rule = "none";  // "model", "property:<name>", or "none" (generic).
    bool verified = false;      // The device's model is in verifiedModels.
};

using PropertyReader = std::function<std::string(const std::string& name)>;

// Device profiles embedded from camera/profiles/*.json, in file-name order.
[[nodiscard]] const std::vector<BuiltInProfile>& builtInProfiles();
// Built-in profile by id; "generic" (logical back camera 0, negotiated RAW
// stream, no vendor keys) for "generic" or an unknown id.
[[nodiscard]] const CameraProfile& builtInCameraProfile(const std::string& profileId);
[[nodiscard]] bool isBuiltInProfileId(const std::string& profileId);
// Picks the device's profile from system properties (ro.product.model first).
[[nodiscard]] ProfileMatch matchBuiltInProfile(const PropertyReader& property);

[[nodiscard]] std::optional<LensRoute> routeForLens(const CameraProfile& profile, const std::string& lensId);
// Explicit camera-id override (debug intent / diagnostics): the profile's
// route for that camera when it has one, otherwise a negotiated generic route.
[[nodiscard]] LensRoute routeForCameraId(const CameraProfile& profile, const std::string& cameraId);

}  // namespace rawrcam::camera
