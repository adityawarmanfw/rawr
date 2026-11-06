#include "metadata/GpsMetadata.h"
#include "encoding/jpeg/JpegExif.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

using GpsFields = std::map<uint16_t, std::vector<uint8_t>>;

static GpsFields ifdFields(const uint8_t* bytes, size_t size, uint16_t directoryTag = 34853, uint16_t onlyTag = 0) {
    const bool bigEndian = bytes[0] == 'M';
    const auto number = [&](size_t offset, size_t length) {
        assert(offset <= size && length <= size - offset);
        uint64_t value = 0;
        for (size_t index = 0; index < length; ++index)
            value |= uint64_t(bytes[offset + index]) << (8 * (bigEndian ? length - index - 1 : index));
        return value;
    };
    const bool bigTiff = number(2, 2) == 43;
    const size_t countSize = bigTiff ? 8 : 2;
    const size_t entrySize = bigTiff ? 20 : 12;
    const size_t inlineSize = bigTiff ? 8 : 4;
    const size_t root = number(bigTiff ? 8 : 4, inlineSize);
    const auto rootCount = number(root, countSize);
        size_t gpsOffset = directoryTag ? 0 : root;
    for (size_t index = 0; index < rootCount; ++index) {
        const size_t entry = root + countSize + index * entrySize;
        if (number(entry, 2) == directoryTag) gpsOffset = number(entry + (bigTiff ? 12 : 8), inlineSize);
    }
    GpsFields fields;
    if (!gpsOffset) return fields;
    const auto count = number(gpsOffset, countSize);
    assert(count <= 32);
    for (size_t index = 0; index < count; ++index) {
        const size_t entry = gpsOffset + countSize + index * entrySize;
        const auto tag = uint16_t(number(entry, 2));
        if (onlyTag && tag != onlyTag) continue;
        const auto type = number(entry + 2, 2);
        const auto values = number(entry + 4, bigTiff ? 8 : 4);
        assert(type == 1 || type == 2 || type == 3 || type == 4 || type == 5 || type == 7);
        const size_t length = values * (type == 5 ? 8 : type == 4 ? 4 : type == 3 ? 2 : 1);
        const size_t data = length > inlineSize ? number(entry + (bigTiff ? 12 : 8), inlineSize)
                                               : entry + (bigTiff ? 12 : 8);
        assert(data <= size && length <= size - data);
        std::vector<uint8_t> normalized{uint8_t(type), uint8_t(values)};
        for (size_t offset = 0; offset < length; ++offset)
            normalized.push_back(bytes[data + (type == 5 && bigEndian ? offset / 4 * 4 + 3 - offset % 4 : offset)]);
        assert(fields.emplace(tag, normalized).second);
    }
    return fields;
}

static void roundTrip(const std::optional<tinydng_gps>& gps,
                      std::optional<std::array<uint32_t, 2>> digitalZoom = std::nullopt,
                      bool legacyVersion = false) {
    tinydng_error error{};
    auto* context = tinydng_context_create(nullptr, &error);
    assert(context);
    uint16_t pixels[4] = {1, 2, 3, 4};
    tinydng_write_image image{};
    image.width = 2;
    image.height = 2;
    image.bits_per_sample = 16;
    image.samples_per_pixel = 1;
    image.data = reinterpret_cast<const uint8_t*>(pixels);
    image.data_size = sizeof(pixels);
    image.gps = gps ? &*gps : nullptr;
    tinydng_exif exif{};
    exif.has_exif_version = 1;
    std::memcpy(exif.exif_version, legacyVersion ? "0230" : "0300", 4);
    exif.has_digital_zoom = digitalZoom.has_value();
    if (digitalZoom) std::copy(digitalZoom->begin(), digitalZoom->end(), exif.digital_zoom_rational);
    image.exif = &exif;
    tinydng_write_options options{};
    options.as_dng = 1;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    assert(tinydng_write_memory(context, &image, &options, &bytes, &size, &error) == TINYDNG_OK);
    rawrcam::encoding::jpeg::JpegCaptureContext jpegContext;
    jpegContext.gps = gps;
    jpegContext.wallClockUnixMillis = 1'700'000'000'000;
    jpegContext.preserveSourceTime = legacyVersion;
    if (legacyVersion) jpegContext.sourceExifVersion = std::array<uint8_t, 4>{'0', '2', '3', '0'};
    jpegContext.digitalZoomRatio = digitalZoom;
    const auto jpegExif = rawrcam::encoding::jpeg::buildExif(jpegContext, 2, 2);
    assert(jpegExif.size() > 14 && std::memcmp(jpegExif.data(), "Exif\0\0II", 8) == 0);
    const auto jpegGps = ifdFields(jpegExif.data() + 6, jpegExif.size() - 6);
    const auto tiffGps = ifdFields(bytes, size);
    assert(tiffGps == jpegGps);
    assert(tiffGps.empty() == !gps.has_value());
    const auto jpegFields = ifdFields(jpegExif.data() + 6, jpegExif.size() - 6, 0x8769);
    const auto& version = jpegFields.at(0x9000);
    assert(std::string(version.begin() + 2, version.end()) == (legacyVersion ? "0231" : gps ? "0300" : "0232"));
    if (!gps) {
        GpsFields rootFields;
        auto read = [&] {
            const auto data = rawrcam::encoding::jpeg::buildExif(jpegContext, 2, 2);
            rootFields = ifdFields(data.data() + 6, data.size() - 6, 0);
            return ifdFields(data.data() + 6, data.size() - 6, 0x8769);
        };
        auto fields = read();
        const auto text = [&](uint16_t tag) {
            const auto& value = fields.at(tag);
            return std::string(value.begin() + 2, value.end() - 1);
        };
        for (uint16_t tag : {0x9010, 0x9011, 0x9012}) assert(text(tag) == "+00:00");
        for (uint16_t tag : {0x9290, 0x9291, 0x9292}) assert(text(tag) == "000");
        jpegContext.preserveSourceTime = true;
        jpegContext.sourceExifVersion = std::array<uint8_t, 4>{'0', '2', '3', '0'};
        jpegContext.dateTimeOriginal = "2026:05:01 12:34:56";
        jpegContext.dateTimeDigitized = "2026:05:02 01:02:03";
        jpegContext.offsetTimeOriginal = "+08:00";
        jpegContext.subsecTimeOriginal = "042";
        jpegContext.offsetTimeDigitized = "-03:30";
        jpegContext.subsecTimeDigitized = "567";
        fields = read();
        assert(text(0x9003) == jpegContext.dateTimeOriginal && text(0x9004) == jpegContext.dateTimeDigitized);
        const auto& modified = rootFields.at(0x0132);
        assert(std::string(modified.begin() + 2, modified.end() - 1) == "2023:11:14 22:13:20");
        assert((fields.at(0x9000) == std::vector<uint8_t>{7, 4, '0', '2', '3', '1'}));
        assert(text(0x9010) == "+00:00" && text(0x9290) == "000");
        assert(text(0x9011) == "+08:00" && text(0x9291) == "042");
        assert(text(0x9012) == "-03:30" && text(0x9292) == "567");
        jpegContext.offsetTimeOriginal.clear();
        jpegContext.subsecTimeOriginal.clear();
        jpegContext.offsetTimeDigitized.clear();
        jpegContext.subsecTimeDigitized.clear();
        fields = read();
        for (uint16_t tag : {0x9011, 0x9012, 0x9291, 0x9292}) assert(!fields.contains(tag));
        jpegContext.dateTimeDigitized.clear();
        fields = read();
        assert(text(0x9003) == jpegContext.dateTimeOriginal);
        for (uint16_t tag : {0x9004, 0x9012, 0x9292}) assert(!fields.contains(tag));
        jpegContext.dateTimeDigitized = "2026:05:02 01:02:03";
        jpegContext.dateTimeOriginal.clear();
        fields = read();
        assert(text(0x9004) == jpegContext.dateTimeDigitized && !fields.contains(0x9012) && !fields.contains(0x9292));
        assert(!fields.contains(0x9003) && !fields.contains(0x9011) && !fields.contains(0x9291));
        jpegContext.dateTimeDigitized.clear();
        fields = read();
        for (uint16_t tag : {0x9003, 0x9004, 0x9011, 0x9012, 0x9291, 0x9292}) assert(!fields.contains(tag));
        assert(text(0x9010) == "+00:00" && text(0x9290) == "000");
    }
    const auto jpegZoom = ifdFields(jpegExif.data() + 6, jpegExif.size() - 6, 0x8769, 0xA404);
    const auto tiffZoom = ifdFields(bytes, size, 0x8769, 0xA404);
    assert(jpegZoom == tiffZoom && jpegZoom.empty() == !digitalZoom.has_value());
    tinydng_document* document = nullptr;
    assert(tinydng_open_memory(context, bytes, size, nullptr, &document, &error) == TINYDNG_OK);
    const auto* loaded = tinydng_image_get(document, 0);
    assert(loaded && bool(loaded->gps.has_coordinates) == gps.has_value());
    if (gps) assert(rawrcam::metadata::sameGpsRecord(loaded->gps, *gps));
    tinydng_pixels decoded{};
    assert(tinydng_decode_image(context, document, 0, nullptr, &decoded, &error) == TINYDNG_OK);
    assert(decoded.size == sizeof(pixels) && std::memcmp(decoded.data, pixels, sizeof(pixels)) == 0);
    tinydng_pixels_free(context, &decoded);
    tinydng_document_destroy(context, document);
    tinydng_buffer_free(context, bytes);
    tinydng_context_destroy(context);
}

int main() {
    using namespace rawrcam::metadata;
    roundTrip(std::nullopt);
    auto zero = makeGps(0, 0, 0.0, std::nullopt, 0, 1);
    assert(zero && zero->has_coordinates && zero->has_altitude);
    assert(zero->latitude[0][0] == 0 && zero->longitude[0][0] == 0);
    assert(zero->altitude[0] == 0 && zero->altitude[1] != 0);
    assert(!zero->has_horizontal_error);
    assert(zero->altitude_ref == 0);
    assert(zero->version[0] == 2 && zero->version[1] == 4);
    auto high = makeGps(-90, -180, -12345.678, 0.0, 1793059200123LL);
    assert(high && high->latitude_ref == 'S' && high->longitude_ref == 'W');
    assert(high->altitude_ref == 1 && high->has_horizontal_error);
    assert(std::abs(double(high->altitude[0]) / high->altitude[1] - 12345.678) < 0.001);
    assert(high->has_time && high->timestamp[2][0] == 123 && high->timestamp[2][1] == 1000);
    assert(validGps(*high));
    roundTrip(*high, std::array<uint32_t, 2>{3, 2}, true);
    roundTrip(*zero, std::array<uint32_t, 2>{0, 1});

    auto absent = makeGps(12, 34, std::nullopt, std::nullopt, 0);
    assert(absent && !absent->has_altitude && !absent->has_time);
    roundTrip(*absent);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    assert(!makeGps(nan, 0, {}, {}, 0));
    assert(!makeGps(91, 0, {}, {}, 0));
    assert(!makeGps(0, -181, {}, {}, 0));
    auto invalidOptional = makeGps(0, 0, nan, -1.0, 0);
    assert(invalidOptional && !invalidOptional->has_altitude && !invalidOptional->has_horizontal_error);
    for (double invalid : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        const auto optional = makeGps(0, 0, invalid, invalid, 0);
        assert(optional && !optional->has_altitude && !optional->has_horizontal_error);
    }

    auto rounded = makeGps(12.999999999999, 179.999999999999, {}, {}, 0);
    assert(rounded && rounded->latitude[0][0] == 13 && rounded->longitude[0][0] == 180);
    assert(rounded->latitude[1][0] == 0 && rounded->latitude[2][0] == 0);
    high->latitude[1][1] = 0;
    assert(!validGps(*high));
    return 0;
}