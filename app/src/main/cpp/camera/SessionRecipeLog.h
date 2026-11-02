#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace rawrcam::camera {

// What one capture session was configured with, as one log line. Bundles from
// different devices become comparable: the line states every stream, the
// session-parameter policy and the key counts per scope instead of leaving
// them to be pieced together from separate lines.
struct SessionRecipeFacts {
    struct Companion {
        std::string format;  // "YUV_420_888" or "PRIVATE"
        int32_t width = 0;
        int32_t height = 0;
        bool targeted = false;
        std::string source;  // "profile" or "automatic"
        std::string shape;   // "raw" (matches the RAW aspect) or "fallback"
        std::string tuning;  // reader images, usage, physical, detach and fallback, as ":key=value" parts
    };
    std::string lensId;
    std::string cameraId;
    std::string physicalCameraId;
    std::string rawFormat;
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
    std::string requestTemplate;  // "preview" or "still"
    std::string fpsRange;         // "12..30", or "none" when the session parameters carry none
    std::string sessionParams = "cadence";  // cadence, none or full
    std::string requestDefaults = "rawr";    // rawr or minimal
    std::string cameraApi = "native";        // the camera API that ran the session
    uint32_t opMode = 0;                     // Java backend only
    // Per output stream use case (Java Camera2 only): raw, photo, companion, aux JPEG.
    int64_t rawUseCase = 0, photoUseCase = 0, companionUseCase = 0, auxJpegUseCase = 0;
    int sessionKeys = 0;
    int requestKeys = 0;
    int stillKeys = 0;
    int decisionKeys = 0;
    std::optional<Companion> companion;
    std::optional<std::pair<uint32_t, uint32_t>> auxJpeg;
    std::optional<std::pair<uint32_t, uint32_t>> photoStream;
    std::optional<uint64_t> rawBufferUsage;         // profile-fixed; nullopt = chosen by the ingress ladder
    std::optional<uint64_t> photoBufferUsage;  // profile-fixed; nullopt = the app default
    int32_t startupDelayMs = 0;
    int32_t frameRate = 0;
    std::string vendorGate;
    std::string gateHold;       // "configured" or "closed"; meaningful with a gate
    std::string stillStrategy;  // direct, decisionFrame or stopRepeatThenStill
    bool stopViewfinderForPhotos = false;
};

namespace detail {
inline std::string hex(uint64_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}
inline std::string size(uint32_t width, uint32_t height) { return std::to_string(width) + "x" + std::to_string(height); }
inline std::string orNone(const std::string& value) { return value.empty() ? "none" : value; }
}  // namespace detail

inline std::string describeSessionRecipe(const SessionRecipeFacts& f) {
    using namespace detail;
    std::string aspect = "unknown";
    if (f.rawWidth > 0 && f.rawHeight > 0) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%.3f", double(f.rawWidth) / double(f.rawHeight));
        aspect = buffer;
    }
    std::string line = "CAMERA_SESSION_RECIPE lens=" + orNone(f.lensId) + " camera=" + orNone(f.cameraId) +
                       " physical=" + orNone(f.physicalCameraId) + " raw=" + orNone(f.rawFormat) + ":" +
                       size(f.rawWidth, f.rawHeight) + " aspect=" + aspect + " template=" + orNone(f.requestTemplate) +
                       " fps=" + orNone(f.fpsRange) + " sessionParams=" + orNone(f.sessionParams) +
                       " requestDefaults=" + orNone(f.requestDefaults) + " cameraApi=" + orNone(f.cameraApi) +
                       " keys=session:" + std::to_string(f.sessionKeys) + ",request:" + std::to_string(f.requestKeys) +
                       ",still:" + std::to_string(f.stillKeys) + ",decision:" + std::to_string(f.decisionKeys);
    if (f.companion) {
        const auto& c = *f.companion;
        line += " companion=" + c.format + ":" + std::to_string(c.width) + "x" + std::to_string(c.height) +
                (c.targeted ? ":targeted" : ":untargeted") + ":source=" + c.source + ":shape=" + c.shape + c.tuning;
    } else {
        line += " companion=none";
    }
    line += " auxJpeg=" + (f.auxJpeg ? size(f.auxJpeg->first, f.auxJpeg->second) : std::string("none"));
    line += " photoStream=" + (f.photoStream ? size(f.photoStream->first, f.photoStream->second) : "none");
    line += " rawBufferUsage=" + (f.rawBufferUsage ? hex(*f.rawBufferUsage) : std::string("ladder"));
    line += " photoBufferUsage=" + (f.photoBufferUsage ? hex(*f.photoBufferUsage) : std::string("default"));
    line += " startupDelayMs=" + std::to_string(f.startupDelayMs) + " frameRate=" + std::to_string(f.frameRate) +
            " gate=" + orNone(f.vendorGate) + (f.vendorGate.empty() ? "" : ":" + orNone(f.gateHold)) +
            " still=" + orNone(f.stillStrategy) + " stopViewfinderForPhotos=" + (f.stopViewfinderForPhotos ? "1" : "0");
    // Only when set, so lines of sessions without them stay comparable with older bundles.
    if (f.opMode) line += " opMode=" + std::to_string(f.opMode);
    if (f.rawUseCase || f.photoUseCase || f.companionUseCase || f.auxJpegUseCase)
        line += " useCases=raw:" + std::to_string(f.rawUseCase) + ",photo:" + std::to_string(f.photoUseCase) +
                ",companion:" + std::to_string(f.companionUseCase) + ",auxJpeg:" + std::to_string(f.auxJpegUseCase);
    return line;
}

}  // namespace rawrcam::camera
