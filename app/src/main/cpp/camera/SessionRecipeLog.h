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
    };
    std::string lensId;
    std::string cameraId;
    std::string physicalCameraId;
    std::string rawFormat;
    uint32_t rawWidth = 0;
    uint32_t rawHeight = 0;
    std::string requestTemplate;  // "preview" or "still"
    std::string fpsRange;         // "12..30", or "none" when the session parameters carry none
    bool sessionParameters = true;
    int sessionKeys = 0;
    int requestKeys = 0;
    int stillKeys = 0;
    int decisionKeys = 0;
    std::optional<Companion> companion;
    std::optional<std::pair<uint32_t, uint32_t>> auxJpeg;
    std::optional<std::pair<uint32_t, uint32_t>> captureStream;
    std::optional<uint64_t> readerUsage;         // profile-fixed; nullopt = chosen by the ingress ladder
    std::optional<uint64_t> captureReaderUsage;  // profile-fixed; nullopt = the app default
    int32_t startupDelayMs = 0;
    int32_t fixedFps = 0;
    std::string vendorGate;
    std::string gateHold;       // "configured" or "closed"; meaningful with a gate
    std::string stillStrategy;  // direct, decisionFrame or stopRepeatThenStill
    bool stopRepeatingForStill = false;
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
                       " fps=" + orNone(f.fpsRange) + " sessionParameters=" + (f.sessionParameters ? "1" : "0") +
                       " keys=session:" + std::to_string(f.sessionKeys) + ",request:" + std::to_string(f.requestKeys) +
                       ",still:" + std::to_string(f.stillKeys) + ",decision:" + std::to_string(f.decisionKeys);
    if (f.companion) {
        const auto& c = *f.companion;
        line += " companion=" + c.format + ":" + std::to_string(c.width) + "x" + std::to_string(c.height) +
                (c.targeted ? ":targeted" : ":untargeted") + ":source=" + c.source + ":shape=" + c.shape;
    } else {
        line += " companion=none";
    }
    line += " auxJpeg=" + (f.auxJpeg ? size(f.auxJpeg->first, f.auxJpeg->second) : std::string("none"));
    line += " captureStream=" + (f.captureStream ? size(f.captureStream->first, f.captureStream->second) : "none");
    line += " readerUsage=" + (f.readerUsage ? hex(*f.readerUsage) : std::string("ladder"));
    line += " captureReaderUsage=" + (f.captureReaderUsage ? hex(*f.captureReaderUsage) : std::string("default"));
    line += " startupDelayMs=" + std::to_string(f.startupDelayMs) + " fixedFps=" + std::to_string(f.fixedFps) +
            " gate=" + orNone(f.vendorGate) + (f.vendorGate.empty() ? "" : ":" + orNone(f.gateHold)) +
            " still=" + orNone(f.stillStrategy) + " stopRepeatingForStill=" + (f.stopRepeatingForStill ? "1" : "0");
    return line;
}

}  // namespace rawrcam::camera
