#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "geometry/RawGeometry.h"

namespace rawrcam::camera {

// One Camera2 metadata entry to inject. The tag is a metadata name (public or
// vendor, e.g. "vivo.control.forceSensorMode") or a numeric id ("0x80020000").
// Session keys go into the session parameters of every capture session;
// request keys go into every capture request (stills and video both come from
// the repeating RAW request).
struct CameraKeySetting {
    enum class Type : uint8_t { Byte, Int32, Int64, Float, Double };
    // Still: only on full-size still requests (LensRoute::captureStream), never
    // on the session or the repeating request.
    enum class Scope : uint8_t { Session, Request, Still };
    std::string tag;
    Type type = Type::Int32;
    Scope scope = Scope::Session;
    std::vector<double> values;
    bool enabled = true;
};

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
// RAW modes accept besides none (profile "readerUsage": "gpuDataBuffer").
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
    std::vector<CameraKeySetting> keys;
    LevelOverride levels;
    ExposureOverride exposure;
    // Full-size vendor modes stream a size the camera doesn't advertise: use
    // preferredStream as is instead of negotiating it against the RAW outputs.
    bool unlistedStream = false;
    // AHB usage for the RAW reader instead of the ingress ladder's choice; the
    // vivo full-size modes configure only without CPU-read / sampled-image bits.
    std::optional<uint64_t> readerUsage;
    // Vendor state the app holds while the device is open (see VendorCameraGate).
    std::string vendorGate;
    // Full-size stills from a second RAW stream that only still requests fill
    // (vivo seamless route): `stream` is then the binned viewfinder, and the
    // HAL switches the sensor to full size for each still. Unlisted size.
    std::optional<geometry::RawStreamPreference> captureStream;
    // Most full-size stills one merge may request (1 = none: the HAL fails
    // bursts); 0 = the app's own cap.
    uint32_t captureBurst = 0;
    // Submit a burst one still at a time, each after the previous frame is in
    // (the HAL fails queued full-size stills but takes them back to back).
    bool captureBurstSequential = false;
    // Sensor colour layout when the camera reports it wrong. cfaBlock: side of
    // the same-colour blocks the RAW actually holds (1 Bayer, 2 quad, 4 for
    // 4x4); ingress remosaics them to Bayer. cfaOrder: block-level colour
    // order (CfaPattern code) replacing the camera's; nullopt keeps it.
    uint32_t cfaBlock = 1;
    std::optional<uint32_t> cfaOrder;
};

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
