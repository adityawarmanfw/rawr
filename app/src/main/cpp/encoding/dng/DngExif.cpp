#include "encoding/dng/DngExif.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>

#include "geometry/OrientationTransform.h"

namespace rawrcam::encoding::dng {
namespace {

using rawrcam::metadata::rectWidth;
using rawrcam::metadata::validRect;

std::string tiffAsciiDescription(std::string text) {
    for (char& ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c > 0x7f || (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) {
            ch = '?';
        }
    }
    return text;
}

std::string exifDateTime(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04d:%02u:%02u %02u:%02u:%02u", y, mo, d, h, mi, s);
    return std::string(buf);
}

std::string exifOffset(std::int16_t minutes) {
    const char sign = minutes < 0 ? '-' : '+';
    const int absolute = minutes < 0 ? -static_cast<int>(minutes) : static_cast<int>(minutes);
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d", sign, absolute / 60, absolute % 60);
    return std::string(buf);
}

std::string exifSubseconds(std::uint32_t nanosecond) {
    char buf[10]{};
    std::snprintf(buf, sizeof(buf), "%09u", nanosecond);
    std::string out(buf);
    while (out.size() > 1 && out.back() == '0') out.pop_back();
    return out;
}

// Quantize a float to {round(v*1e6), 1e6} int32 pair (matches legacy writer).
void quantizeSRational(float value, int32_t& num, int32_t& den) {
    constexpr double denD = 1000000.0;
    const double scaled = std::round(static_cast<double>(value) * denD);
    num = static_cast<int32_t>(
        std::clamp(scaled, double(std::numeric_limits<int32_t>::min()), double(std::numeric_limits<int32_t>::max())));
    den = 1000000;
}

void quantizeURational(float value, uint32_t& num, uint32_t& den) {
    constexpr double denD = 1000000.0;
    const double scaled = std::round(std::max(0.0, static_cast<double>(value)) * denD);
    num = static_cast<uint32_t>(std::min(scaled, double(std::numeric_limits<uint32_t>::max())));
    den = 1000000;
}

}  // namespace

void fillDngExif(TinyDngWriteParams& p, const rawrcam::imaging::RawSnapshot& frame,
                 const DngCaptureContext& captureContext) {
    const auto& ctx = *frame.metadata.cameraContext;
    tinydng_exif& ex = p.exif;
    if (!captureContext.deviceMake.empty()) {
        p.make = captureContext.deviceMake;
        p.lensMake = captureContext.deviceMake;
    }
    const auto& displayModel = captureContext.deviceDisplayModel.empty()
        ? captureContext.deviceModel : captureContext.deviceDisplayModel;
    if (!displayModel.empty()) {
        p.model = displayModel;
        p.lensModel = captureContext.deviceModel;
    }
    p.software = "RawrCam";
    p.description = tiffAsciiDescription(captureContext.imageDescription.empty() ? "Captured with Rawr"
                                                                                 : captureContext.imageDescription);
    switch (rawrcam::geometry::presentationQuarterTurns(ctx.geometry.sensorOrientationDegrees,
                                                        captureContext.deviceRotationDegrees)) {
        case 1u:
            ex.orientation = 6;
            break;  // RightTop
        case 2u:
            ex.orientation = 3;
            break;  // BottomRight
        case 3u:
            ex.orientation = 8;
            break;  // LeftBottom
        default:
            ex.orientation = 1;
            break;  // TopLeft
    }
    if (frame.metadata.exposureTimeNs > 0) {
        const uint64_t us = static_cast<uint64_t>(frame.metadata.exposureTimeNs + 500) / 1000u;
        if (us <= std::numeric_limits<uint32_t>::max()) {
            ex.exposure_time[0] = int32_t(us);
            ex.exposure_time[1] = 1000000;
            ex.has_exposure_time = 1;
            const double seconds = double(frame.metadata.exposureTimeNs) * 1.0e-9;
            if (seconds > 0.0) {
                quantizeSRational(float(-std::log2(seconds)), ex.shutter_speed[0], ex.shutter_speed[1]);
                ex.has_shutter_speed = 1;
            }
        }
    }
    if (frame.metadata.sensitivity > 0 && frame.metadata.sensitivity <= 65534) {
        ex.iso = frame.metadata.sensitivity;
        ex.has_iso = 1;
    }
    if (frame.metadata.aperture && *frame.metadata.aperture > 0.0f) {
        quantizeURational(*frame.metadata.aperture, ex.fnumber_rational[0], ex.fnumber_rational[1]);
        ex.has_fnumber = 1;
        quantizeURational(float(2.0 * std::log2(*frame.metadata.aperture)), ex.max_aperture_rational[0],
                          ex.max_aperture_rational[1]);
        // ApertureValue written from the same float path as legacy (unsigned).
        quantizeSRational(float(2.0 * std::log2(*frame.metadata.aperture)), ex.aperture_value[0], ex.aperture_value[1]);
        ex.has_aperture_value = 1;
    }
    if (!ctx.availableApertures.empty()) {
        const float amin = *std::min_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
        if (amin > 0.0f) {
            quantizeURational(float(2.0 * std::log2(amin)), ex.max_aperture_rational[0], ex.max_aperture_rational[1]);
            ex.has_max_aperture_value = 1;
        }
    }
    if (frame.metadata.focalLengthMm && *frame.metadata.focalLengthMm > 0.0f) {
        quantizeURational(*frame.metadata.focalLengthMm, ex.focal_length_rational[0], ex.focal_length_rational[1]);
        ex.has_focal_length = 1;
        const double sw = ctx.sensorPhysicalSizeMm[0], sh = ctx.sensorPhysicalSizeMm[1];
        if (sw > 0.0 && sh > 0.0) {
            const double eq =
                *frame.metadata.focalLengthMm * std::sqrt(36.0 * 36.0 + 24.0 * 24.0) / std::sqrt(sw * sw + sh * sh);
            if (eq >= 1.0 && eq <= 65535.0) {
                ex.focal_length_35mm = uint16_t(std::lround(eq));
                ex.has_focal_length_35mm = 1;
            }
        }
    }
    if (frame.metadata.focusDistanceDiopters && *frame.metadata.focusDistanceDiopters > 0.000001f) {
        quantizeURational(1.0f / *frame.metadata.focusDistanceDiopters, ex.subject_distance_rational[0],
                          ex.subject_distance_rational[1]);
        ex.has_subject_distance = 1;
    }
    ex.exposure_mode = uint16_t(frame.metadata.aeMode == 0 ? 1 : 0);
    ex.has_exposure_mode = 1;
    ex.white_balance = uint16_t(frame.metadata.awbMode == 0 ? 1 : 0);
    ex.has_white_balance = 1;
    ex.sensing_method = 2;
    ex.has_sensing_method = 1;
    ex.flash = (frame.metadata.flashState == 3 || frame.metadata.flashState == 4) ? 1 : 0;
    ex.has_flash = 1;
    if (validRect(frame.metadata.scalerCropRegion) && validRect(ctx.geometry.activeArray)) {
        const double fullW = rectWidth(ctx.geometry.activeArray);
        const double cropW = rectWidth(frame.metadata.scalerCropRegion);
        if (cropW > 0.0 && fullW >= cropW) {
            quantizeURational(float(fullW / cropW), ex.digital_zoom_rational[0], ex.digital_zoom_rational[1]);
            ex.has_digital_zoom = 1;
        }
    }
    if (captureContext.wallClockUnixMillis > 0) {
        const time_t localSeconds = static_cast<time_t>(captureContext.wallClockUnixMillis / 1000 +
                                                        int64_t(captureContext.utcOffsetMinutes) * 60);
        struct tm local{};
        if (gmtime_r(&localSeconds, &local)) {
            p.datetime = exifDateTime(local.tm_year + 1900, unsigned(local.tm_mon + 1), unsigned(local.tm_mday),
                                      unsigned(local.tm_hour), unsigned(local.tm_min), unsigned(local.tm_sec));
            p.dateOriginal = p.datetime;
            p.dateDigitized = p.datetime;
            // Legacy writer emits OffsetTime* only when the offset is non-zero.
            if (captureContext.utcOffsetMinutes) {
                p.offsetTime = exifOffset(captureContext.utcOffsetMinutes);
                p.offsetOrig = p.offsetTime;
                p.offsetDig = p.offsetTime;
            }
            const uint32_t ns = uint32_t(captureContext.wallClockUnixMillis % 1000) * 1000000u;
            if (ns) {
                p.subsec = exifSubseconds(ns);
                p.subsecOrig = p.subsec;
                p.subsecDig = p.subsec;
            }
        }
    }
    if (!ctx.availableFocalLengthsMm.empty()) {
        const auto [fminIt, fmaxIt] =
            std::minmax_element(ctx.availableFocalLengthsMm.begin(), ctx.availableFocalLengthsMm.end());
        ex.lens_spec[0] = *fminIt;
        ex.lens_spec[1] = *fmaxIt;
        if (!ctx.availableApertures.empty()) {
            const float amin = *std::min_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
            const float amax = *std::max_element(ctx.availableApertures.begin(), ctx.availableApertures.end());
            ex.lens_spec[2] = amin > 0 ? amin : 0;
            ex.lens_spec[3] = amax > 0 ? amax : 0;
        }
        ex.has_lens_spec = 1;
    }
}

}  // namespace rawrcam::encoding::dng
