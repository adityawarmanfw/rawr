#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace rawrcam::metadata {

// Camera attitude at the shutter press, measured against gravity.
//   pitchDeg: lens elevation above the horizon (0 = level, +90 = sky, -90 = ground).
//   rollDeg:  turn about the lens axis, positive = clockwise as the photographer sees it (the horizon then
//             leans counter-clockwise in the frame), measured from the nearest 90 degrees of device orientation.
//             Absent when the lens points near vertical, where roll has no meaning.
struct CaptureLevel {
    float pitchDeg = 0.0f;
    std::optional<float> rollDeg;
};

// XMP property namespace for the angles (a URN identifier; it is never fetched).
inline constexpr char kLevelXmpNamespace[] = "urn:rawr:ns:1.0/";

// JNI form: {pitch, roll} with NaN for "absent". A missing array or a non-finite pitch means no level.
inline std::optional<CaptureLevel> captureLevelFromValues(const float* values, std::size_t count) {
    if (!values || count < 2 || !std::isfinite(values[0])) return std::nullopt;
    CaptureLevel level;
    level.pitchDeg = std::fmax(-180.0f, std::fmin(180.0f, values[0]));
    if (std::isfinite(values[1])) level.rollDeg = std::fmax(-180.0f, std::fmin(180.0f, values[1]));
    return level;
}

// EXIF CameraElevationAngle (SRATIONAL): hundredths of a degree.
inline std::array<std::int32_t, 2> elevationAngleRational(float pitchDeg) {
    return {static_cast<std::int32_t>(std::lround(static_cast<double>(pitchDeg) * 100.0)), 100};
}

inline std::string formatAngle(float deg) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(deg));
    return buf;
}

// Property attributes for an rdf:Description element, including the namespace declaration.
inline std::string levelXmpAttributes(const CaptureLevel& level) {
    std::string out = std::string(" xmlns:rawr=\"") + kLevelXmpNamespace + "\" rawr:Pitch=\"" +
                      formatAngle(level.pitchDeg) + "\"";
    if (level.rollDeg) out += " rawr:Roll=\"" + formatAngle(*level.rollDeg) + "\"";
    return out;
}

// A complete XMP packet for TIFF/DNG tag 700.
inline std::string levelXmpPacket(const CaptureLevel& level) {
    return std::string("<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                       "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
                       " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                       "  <rdf:Description rdf:about=\"\"") +
           levelXmpAttributes(level) +
           "/>\n"
           " </rdf:RDF>\n"
           "</x:xmpmeta>\n"
           "<?xpacket end=\"w\"?>";
}

}  // namespace rawrcam::metadata
