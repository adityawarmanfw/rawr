#include "encoding/jpeg/JpegExif.h"
#include "encoding/SoftwareName.h"
#include "encoding/jpeg/JpegLensMetadata.h"
#include "metadata/ExifTime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace rawrcam::encoding::jpeg {
namespace {

void put16(std::vector<unsigned char>& out, std::uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 8u) & 0xffu));
}
void put32(std::vector<unsigned char>& out, std::uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 8u) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 16u) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 24u) & 0xffu));
}
void patch32(std::vector<unsigned char>& out, std::size_t pos, std::uint32_t v) {
    out[pos + 0] = static_cast<unsigned char>(v & 0xffu);
    out[pos + 1] = static_cast<unsigned char>((v >> 8u) & 0xffu);
    out[pos + 2] = static_cast<unsigned char>((v >> 16u) & 0xffu);
    out[pos + 3] = static_cast<unsigned char>((v >> 24u) & 0xffu);
}

struct IfdEntry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::vector<unsigned char> data;
};

std::vector<unsigned char> asciiData(std::string_view s) {
    std::vector<unsigned char> out(s.begin(), s.end());
    out.push_back(0);
    return out;
}
std::vector<unsigned char> shortData(std::uint16_t v) {
    std::vector<unsigned char> out;
    put16(out, v);
    return out;
}
std::vector<unsigned char> longData(std::uint32_t v) {
    std::vector<unsigned char> out;
    put32(out, v);
    return out;
}
std::vector<unsigned char> rationalData(double value, std::uint32_t denominator = 1000000u) {
    if (!std::isfinite(value) || value < 0.0) value = 0.0;
    const double scaled = std::round(value * static_cast<double>(denominator));
    const auto numerator = static_cast<std::uint32_t>(
        std::clamp(scaled, 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    std::vector<unsigned char> out;
    put32(out, numerator);
    put32(out, denominator);
    return out;
}

IfdEntry rationals(std::uint16_t tag, const std::uint32_t (*pairs)[2], std::uint32_t count) {
    IfdEntry entry{tag, 5, count, {}};
    for (std::uint32_t i = 0; i < count; ++i) {
        put32(entry.data, pairs[i][0]);
        put32(entry.data, pairs[i][1]);
    }
    return entry;
}
IfdEntry raw(std::uint16_t tag, std::uint16_t type, const void* data, std::uint32_t count) {
    const auto* begin = static_cast<const unsigned char*>(data);
    return {tag, type, count, {begin, begin + count}};
}

std::vector<IfdEntry> gpsIfd(const tinydng_gps& gps) {
    const char latitudeRef[2]{gps.latitude_ref, 0}, longitudeRef[2]{gps.longitude_ref, 0};
    const char speedRef[2]{gps.speed_ref, 0}, trackRef[2]{gps.track_ref, 0};
    std::vector<IfdEntry> ifd;
    if (gps.has_version) ifd.push_back(raw(0, 1, gps.version, 4));
    ifd.push_back(raw(1, 2, latitudeRef, 2));
    ifd.push_back(rationals(2, gps.latitude, 3));
    ifd.push_back(raw(3, 2, longitudeRef, 2));
    ifd.push_back(rationals(4, gps.longitude, 3));
    if (gps.has_altitude) {
        ifd.push_back(raw(5, 1, &gps.altitude_ref, 1));
        ifd.push_back(rationals(6, &gps.altitude, 1));
    }
    if (gps.has_time) ifd.push_back(rationals(7, gps.timestamp, 3));
    if (gps.has_speed) {
        ifd.push_back(raw(12, 2, speedRef, 2));
        ifd.push_back(rationals(13, &gps.speed, 1));
    }
    if (gps.has_track) {
        ifd.push_back(raw(14, 2, trackRef, 2));
        ifd.push_back(rationals(15, &gps.track, 1));
    }
    if (gps.map_datum[0]) ifd.push_back(raw(18, 2, gps.map_datum, std::uint32_t(std::strlen(gps.map_datum) + 1)));
    if (gps.processing_method_size) ifd.push_back(raw(27, 7, gps.processing_method, gps.processing_method_size));
    if (gps.has_time) ifd.push_back(raw(29, 2, gps.datestamp, 11));
    if (gps.has_horizontal_error) ifd.push_back(rationals(31, &gps.horizontal_error, 1));
    return ifd;
}

}  // namespace

// Builds a compact EXIF APP1 payload. TIFF offsets are relative to the TIFF header
// immediately following the six-byte "Exif\0\0" signature.
std::vector<unsigned char> buildExif(const JpegCaptureContext& c, std::uint32_t width, std::uint32_t height) {
    const auto time = metadata::exifTime(c.wallClockUnixMillis, c.utcOffsetMinutes);
    const auto& dateTime = time.dateTime;
    const auto& offset = time.offset;
    const auto& subsec = time.subsec;

    std::vector<IfdEntry> ifd0;
    if (!c.imageDescription.empty()) {
        ifd0.push_back(
            {0x010E, 2, static_cast<std::uint32_t>(c.imageDescription.size() + 1), asciiData(c.imageDescription)});
    }
    if (!c.deviceMake.empty())
        ifd0.push_back({0x010F, 2, static_cast<std::uint32_t>(c.deviceMake.size() + 1), asciiData(c.deviceMake)});
    if (!c.deviceModel.empty())
        ifd0.push_back({0x0110, 2, static_cast<std::uint32_t>(c.deviceModel.size() + 1), asciiData(c.deviceModel)});
    ifd0.push_back({0x0112, 3, 1, shortData(c.exifOrientation)});
    ifd0.push_back({0x0213, 3, 1, shortData(1)});
    const std::string software = rawrcam::encoding::kSoftwareName;
    ifd0.push_back({0x0131, 2, static_cast<std::uint32_t>(software.size() + 1), asciiData(software)});
    if (!dateTime.empty()) ifd0.push_back({0x0132, 2, 20, asciiData(dateTime)});
    // ExifIFDPointer placeholder; value is patched after IFD0 layout is known.
    ifd0.push_back({0x8769, 4, 1, longData(0)});
    if (c.gps) ifd0.push_back({0x8825, 4, 1, longData(0)});

    const std::string& original = c.preserveSourceTime ? c.dateTimeOriginal : dateTime;
    const std::string& digitized = c.preserveSourceTime ? c.dateTimeDigitized : dateTime;
    const std::string& originalOffset = c.preserveSourceTime ? c.offsetTimeOriginal : offset;
    const std::string& originalSubsec = c.preserveSourceTime ? c.subsecTimeOriginal : subsec;
    const std::string& digitizedOffset = c.preserveSourceTime ? c.offsetTimeDigitized : offset;
    const std::string& digitizedSubsec = c.preserveSourceTime ? c.subsecTimeDigitized : subsec;
    auto ascii = [](std::uint16_t tag, std::string_view s) {
        return IfdEntry{tag, 2, static_cast<std::uint32_t>(s.size() + 1), asciiData(s)};
    };

    std::vector<IfdEntry> exif;
    if (c.exposureTimeNs > 0)
        exif.push_back({0x829A, 5, 1, rationalData(static_cast<double>(c.exposureTimeNs) / 1.0e9, 1000000u)});
    if (c.aperture && validLensNumber(*c.aperture)) exif.push_back({0x829D, 5, 1, rationalData(*c.aperture, 10000u)});
    const auto apertureValue = c.apertureValue ? c.apertureValue : lensApertureValue(c.aperture);
    if (apertureValue) exif.push_back({0x9202, 5, 1, rationalData(*apertureValue)});
    if (c.maxApertureValue) exif.push_back({0x9205, 5, 1, rationalData(*c.maxApertureValue)});
    if (c.sensitivity > 0 && c.sensitivity <= 65535)
        exif.push_back({0x8827, 3, 1, shortData(static_cast<std::uint16_t>(c.sensitivity))});
    const auto version = std::max(c.sourceExifVersion.value_or(
        c.gps ? std::array<uint8_t, 4>{'0', '3', '0', '0'} : std::array<uint8_t, 4>{'0', '2', '3', '2'}),
        std::array<uint8_t, 4>{'0', '2', '3', '1'});
    exif.push_back({0x9000, 7, 4, {version.begin(), version.end()}});
    exif.push_back({0x9101, 7, 4, {1, 2, 3, 0}});
    if (!original.empty()) {
        exif.push_back(ascii(0x9003, original));
        if (!originalOffset.empty()) exif.push_back(ascii(0x9011, originalOffset));
    }
    if (!digitized.empty()) {
        exif.push_back(ascii(0x9004, digitized));
        if (!digitizedOffset.empty()) exif.push_back(ascii(0x9012, digitizedOffset));
    }
    if (!dateTime.empty()) exif.push_back(ascii(0x9010, offset));
    if (!dateTime.empty() && !subsec.empty()) exif.push_back(ascii(0x9290, subsec));
    if (!original.empty() && !originalSubsec.empty()) exif.push_back(ascii(0x9291, originalSubsec));
    if (!digitized.empty() && !digitizedSubsec.empty()) exif.push_back(ascii(0x9292, digitizedSubsec));
    if (c.focalLengthMm && validLensNumber(*c.focalLengthMm))
        exif.push_back({0x920A, 5, 1, rationalData(*c.focalLengthMm, 10000u)});
    LensMetadata generatedLens;
    const auto lens = lensMetadataForJpeg(c, generatedLens);
    if (lens.focalLength35mm && *lens.focalLength35mm > 0)
        exif.push_back({0xA405, 3, 1, shortData(*lens.focalLength35mm)});
    if (c.digitalZoomRatio && (*c.digitalZoomRatio)[1] != 0) {
        std::vector<unsigned char> ratio;
        put32(ratio, (*c.digitalZoomRatio)[0]);
        put32(ratio, (*c.digitalZoomRatio)[1]);
        exif.push_back({0xA404, 5, 1, ratio});
    }
    if (c.lensSpecification) {
        std::vector<unsigned char> spec;
        for (std::size_t index = 0; index < c.lensSpecification->size(); ++index) {
            const double value = (*c.lensSpecification)[index];
            const auto r = rationalData(value, index >= 2 && value == 0.0 ? 0u : 10000u);
            spec.insert(spec.end(), r.begin(), r.end());
        }
        exif.push_back({0xA432, 5, 4, spec});
    }
    if (!lens.make.empty()) exif.push_back(ascii(0xA433, lens.make));
    if (!lens.model.empty()) exif.push_back(ascii(0xA434, lens.model));
    if (!c.lensSerial.empty()) exif.push_back(ascii(0xA435, c.lensSerial));
    exif.push_back({0xA001, 3, 1, shortData(1)});  // sRGB
    exif.push_back({0xA000, 7, 4, {'0', '1', '0', '0'}});
    exif.push_back({0xA002, 4, 1, longData(width)});
    exif.push_back({0xA003, 4, 1, longData(height)});

    std::vector<unsigned char> tiff;
    tiff.insert(tiff.end(), {'I', 'I'});
    put16(tiff, 42);
    put32(tiff, 8);

    auto appendIfd = [&](const std::vector<IfdEntry>& entries) -> std::uint32_t {
        const std::uint32_t ifdOffset = static_cast<std::uint32_t>(tiff.size());
        put16(tiff, static_cast<std::uint16_t>(entries.size()));
        const std::size_t tableStart = tiff.size();
        tiff.resize(tiff.size() + entries.size() * 12u + 4u, 0);
        std::vector<std::pair<std::size_t, std::vector<unsigned char>>> external;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const std::size_t p = tableStart + i * 12u;
            tiff[p + 0] = static_cast<unsigned char>(e.tag & 0xffu);
            tiff[p + 1] = static_cast<unsigned char>(e.tag >> 8u);
            tiff[p + 2] = static_cast<unsigned char>(e.type & 0xffu);
            tiff[p + 3] = static_cast<unsigned char>(e.type >> 8u);
            patch32(tiff, p + 4, e.count);
            if (e.data.size() <= 4u) {
                std::copy(e.data.begin(), e.data.end(), tiff.begin() + static_cast<std::ptrdiff_t>(p + 8));
            } else {
                external.emplace_back(p + 8, e.data);
            }
        }
        for (auto& [offsetPos, bytes] : external) {
            patch32(tiff, offsetPos, static_cast<std::uint32_t>(tiff.size()));
            tiff.insert(tiff.end(), bytes.begin(), bytes.end());
            if ((tiff.size() & 1u) != 0u) tiff.push_back(0);
        }
        return ifdOffset;
    };

    std::sort(ifd0.begin(), ifd0.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
    std::sort(exif.begin(), exif.end(), [](const IfdEntry& a, const IfdEntry& b) { return a.tag < b.tag; });
    const std::uint32_t ifd0Offset = appendIfd(ifd0);
    const std::uint32_t exifOffset = appendIfd(exif);
    const std::uint32_t gpsOffset = c.gps ? appendIfd(gpsIfd(*c.gps)) : 0;
    // Locate ExifIFDPointer entry in IFD0 and patch its value.
    const std::size_t countPos = ifd0Offset;
    const std::uint16_t count =
        static_cast<std::uint16_t>(tiff[countPos] | (static_cast<std::uint16_t>(tiff[countPos + 1]) << 8u));
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t p = countPos + 2u + static_cast<std::size_t>(i) * 12u;
        const std::uint16_t tag = static_cast<std::uint16_t>(tiff[p] | (static_cast<std::uint16_t>(tiff[p + 1]) << 8u));
        if (tag == 0x8769u) patch32(tiff, p + 8u, exifOffset);
        if (tag == 0x8825u) patch32(tiff, p + 8u, gpsOffset);
    }

    std::vector<unsigned char> payload{'E', 'x', 'i', 'f', 0, 0};
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return payload;
}

}  // namespace rawrcam::encoding::jpeg
