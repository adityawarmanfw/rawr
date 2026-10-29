#include "metadata/GpsMetadata.h"
#include "encoding/jpeg/JpegExif.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

using GpsFields = std::map<uint16_t, std::vector<uint8_t>>;

static GpsFields gpsFields(const uint8_t* bytes, size_t size) {
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
    size_t gpsOffset = 0;
    for (size_t index = 0; index < rootCount; ++index) {
        const size_t entry = root + countSize + index * entrySize;
        if (number(entry, 2) == 34853) gpsOffset = number(entry + (bigTiff ? 12 : 8), inlineSize);
    }
    GpsFields fields;
    if (!gpsOffset) return fields;
    const auto count = number(gpsOffset, countSize);
    assert(count <= 32);
    for (size_t index = 0; index < count; ++index) {
        const size_t entry = gpsOffset + countSize + index * entrySize;
        const auto tag = uint16_t(number(entry, 2));
        const auto type = number(entry + 2, 2);
        const auto values = number(entry + 4, bigTiff ? 8 : 4);
        assert(type == 1 || type == 2 || type == 5 || type == 7);
        const size_t length = values * (type == 5 ? 8 : 1);
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

static uint32_t read32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

static void write32(uint8_t* bytes, uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes[index] = uint8_t(value >> (8 * index));
}

static void checkJpegProfile(const std::vector<uint8_t>& payload) {
    assert(payload.size() > 14 && std::memcmp(payload.data(), "Exif\0\0II", 8) == 0);
    const auto* bytes = payload.data() + 6;
    const size_t size = payload.size() - 6;
    const auto root = read32(bytes + 4);
    assert(root + 2 <= size);
    const unsigned rootCount = unsigned(bytes[root]) | unsigned(bytes[root + 1]) << 8;
    uint32_t exif = 0;
    for (unsigned index = 0; index < rootCount; ++index) {
        const size_t position = root + 2 + index * 12;
        assert(position + 12 <= size);
        if (bytes[position] == 0x69 && bytes[position + 1] == 0x87) exif = read32(bytes + position + 8);
    }
    assert(exif && exif + 2 <= size);
    const unsigned count = unsigned(bytes[exif]) | unsigned(bytes[exif + 1]) << 8;
    unsigned verified = 0;
    for (unsigned index = 0; index < count; ++index) {
        const size_t position = exif + 2 + index * 12;
        assert(position + 12 <= size);
        const auto* entry = bytes + position;
        const unsigned tag = unsigned(entry[0]) | unsigned(entry[1]) << 8;
        if (tag == 0x9000) {
            assert(entry[2] == 7 && read32(entry + 4) == 4 && std::memcmp(entry + 8, "0300", 4) == 0);
            verified |= 1;
        } else if (tag == 0x9101) {
            assert(entry[2] == 7 && read32(entry + 4) == 4 && read32(entry + 8) == 0x00030201);
            verified |= 2;
        } else if (tag == 0xA002 || tag == 0xA003) {
            assert(entry[2] == 4 && read32(entry + 4) == 1 && read32(entry + 8) == 2);
            verified |= tag == 0xA002 ? 4 : 8;
        }
    }
    assert(verified == 15);
}

static void roundTrip(const tinydng_gps& gps) {
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
    image.gps = &gps;
    tinydng_exif exif{};
    exif.has_exif_version = 1;
    std::memcpy(exif.exif_version, "0300", 4);
    image.exif = &exif;
    tinydng_write_options options{};
    options.as_dng = 1;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    assert(tinydng_write_memory(context, &image, &options, &bytes, &size, &error) == TINYDNG_OK);
    const std::vector<uint8_t> original(bytes, bytes + size);
    rawrcam::encoding::jpeg::JpegCaptureContext jpegContext;
    jpegContext.gps = gps;
    const auto jpegExif = rawrcam::encoding::jpeg::buildExif(jpegContext, 2, 2);
    checkJpegProfile(jpegExif);
    const auto jpegGps = gpsFields(jpegExif.data() + 6, jpegExif.size() - 6);
    const auto tiffGps = gpsFields(bytes, size);
    for (const auto& field : tiffGps) {
        const auto other = jpegGps.find(field.first);
        if (other == jpegGps.end() || field.second != other->second)
            std::fprintf(stderr, "GPS mismatch tag=%u tiffBytes=%zu jpegBytes=%zu\n", field.first,
                field.second.size(), other == jpegGps.end() ? 0 : other->second.size());
    }
    assert(tiffGps == jpegGps);
    const auto root = read32(bytes + 4);
    assert(root + 2 < size);
    const auto entries = unsigned(bytes[root]) | unsigned(bytes[root + 1]) << 8;
    uint32_t gpsOffset = 0;
    size_t gpsPointerOffset = 0;
    uint32_t exifOffset = 0;
    size_t orientationEntry = 0;
    for (unsigned index = 0; index < entries; ++index) {
        const auto* entry = bytes + root + 2 + index * 12;
        if (entry[0] == 0x69 && entry[1] == 0x87) exifOffset = read32(entry + 8);
        if (entry[0] == 0x12 && entry[1] == 0x01) orientationEntry = size_t(entry - bytes);
        if (entry[0] == 0x25 && entry[1] == 0x88) {
            assert(entry[2] == 4 && read32(entry + 4) == 1);
            gpsOffset = read32(entry + 8);
            gpsPointerOffset = size_t(entry + 8 - bytes);
        }
    }
    assert(gpsOffset > 0 && gpsOffset + 2 < size);
    const auto gpsEntries = unsigned(bytes[gpsOffset]) | unsigned(bytes[gpsOffset + 1]) << 8;
    bool altitudeSeen = false;
    for (unsigned index = 0; index < gpsEntries; ++index) {
        const auto* entry = bytes + gpsOffset + 2 + index * 12;
        if (entry[0] == 6 && entry[1] == 0) {
            assert(entry[2] == 5 && read32(entry + 4) == 1);
            const auto offset = read32(entry + 8);
            assert(offset + 8 <= size);
            assert(read32(bytes + offset) == gps.altitude[0]);
            assert(read32(bytes + offset + 4) == gps.altitude[1]);
            altitudeSeen = true;
        }
    }
    assert(altitudeSeen == bool(gps.has_altitude));
    tinydng_document* document = nullptr;
    assert(tinydng_open_memory(context, bytes, size, nullptr, &document, &error) == TINYDNG_OK);
    const auto* loaded = tinydng_image_get(document, 0);
    assert(loaded && loaded->gps.has_coordinates);
    assert(loaded->gps.latitude_ref == gps.latitude_ref);
    assert(loaded->gps.has_altitude == gps.has_altitude);
    assert(loaded->gps.altitude[0] == gps.altitude[0]);
    assert(loaded->gps.altitude[1] == gps.altitude[1]);
    tinydng_document_destroy(context, document);
    tinydng_buffer_free(context, bytes);
    for (const bool bigEndian : {false, true}) {
        for (const bool bigTiff : {false, true}) {
            options.big_endian = bigEndian;
            options.bigtiff = bigTiff;
            assert(tinydng_write_memory(context, &image, &options, &bytes, &size, &error) == TINYDNG_OK);
            assert(gpsFields(bytes, size) == jpegGps);
            assert(tinydng_open_memory(context, bytes, size, nullptr, &document, &error) == TINYDNG_OK);
            const auto* result = tinydng_image_get(document, 0);
            assert(result && result->gps.has_coordinates && result->gps.latitude[0][0] == gps.latitude[0][0]);
            assert(result->gps.has_altitude == gps.has_altitude && result->gps.altitude_ref == gps.altitude_ref);
            tinydng_document_destroy(context, document);
            if (bigTiff && !bigEndian) {
                std::vector<uint8_t> corrupted(bytes, bytes + size);
                const auto bigRoot = read32(corrupted.data() + 8);
                const auto bigCount = read32(corrupted.data() + bigRoot);
                for (uint32_t index = 0; index < bigCount; ++index) {
                    auto* entry = corrupted.data() + bigRoot + 8 + index * 20;
                    if (entry[0] == 0x25 && entry[1] == 0x88) std::memset(entry + 4, 0xff, 8);
                }
                assert(tinydng_open_memory(context, corrupted.data(), corrupted.size(), nullptr, &document, &error) == TINYDNG_OK);
                assert(!tinydng_image_get(document, 0)->gps.has_coordinates);
                tinydng_document_destroy(context, document);
                if (gps.has_horizontal_error) {
                    corrupted.assign(bytes, bytes + size);
                    uint32_t directory = 0;
                    for (uint32_t index = 0; index < bigCount; ++index) {
                        const auto* entry = corrupted.data() + bigRoot + 8 + index * 20;
                        if (entry[0] == 0x25 && entry[1] == 0x88) directory = read32(entry + 12);
                    }
                    const uint32_t directoryCount = read32(corrupted.data() + directory);
                    for (uint32_t index = 0; index < directoryCount; ++index) {
                        auto* entry = corrupted.data() + directory + 8 + index * 20;
                        if (entry[0] == 31 && entry[1] == 0) std::memset(entry + 4, 0xff, 8);
                    }
                    assert(tinydng_open_memory(context, corrupted.data(), corrupted.size(), nullptr, &document, &error) == TINYDNG_OK);
                    const auto* malformed = tinydng_image_get(document, 0);
                    assert(malformed->gps.has_coordinates && !malformed->gps.has_horizontal_error);
                    tinydng_document_destroy(context, document);
                }
            }
            tinydng_buffer_free(context, bytes);
        }
    }
    for (unsigned damage = 0; damage < 5; ++damage) {
        auto corrupted = original;
        if (damage == 0) std::memset(corrupted.data() + gpsPointerOffset, 0xff, 4);
        if (damage == 4) std::memset(corrupted.data() + gpsOffset, 0xff, 2);
        for (unsigned index = 0; index < gpsEntries; ++index) {
            auto* entry = corrupted.data() + gpsOffset + 2 + index * 12;
            if (entry[0] == 2 && entry[1] == 0) {
                if (damage == 1) std::memset(corrupted.data() + read32(entry + 8) + 4, 0, 4);
                if (damage == 3) std::memset(entry + 4, 0xff, 4);
            }
            if (damage == 2 && entry[0] == 5 && entry[1] == 0) entry[8] = 9;
        }
        assert(tinydng_open_memory(context, corrupted.data(), corrupted.size(), nullptr, &document, &error) == TINYDNG_OK);
        const auto* result = tinydng_image_get(document, 0);
        assert(result && result->width == 2 && result->height == 2);
        if (damage != 2) assert(!rawrcam::metadata::sanitizeGps(result->gps));
        else assert(result->gps.has_coordinates && !result->gps.has_altitude);
        tinydng_pixels decoded{};
        assert(tinydng_decode_image(context, document, 0, nullptr, &decoded, &error) == TINYDNG_OK);
        assert(decoded.size == sizeof(pixels) && std::memcmp(decoded.data, pixels, sizeof(pixels)) == 0);
        tinydng_pixels_free(context, &decoded);
        tinydng_document_destroy(context, document);
    }
    {
        auto malformed = original;
        assert(exifOffset);
        const unsigned count = unsigned(malformed[exifOffset]) | unsigned(malformed[exifOffset + 1]) << 8;
        for (unsigned index = 0; index < count; ++index) {
            const size_t position = exifOffset + 2 + index * 12;
            if (malformed[position] != 0 || malformed[position + 1] != 0x90) continue;
            const auto dataOffset = uint32_t(malformed.size());
            malformed.insert(malformed.end(), {'0', 0, '3', 0, '0', 0, '0', 0});
            malformed[position + 2] = 3;
            write32(malformed.data() + position + 8, dataOffset);
        }
        assert(tinydng_open_memory(context, malformed.data(), malformed.size(), nullptr, &document, &error) == TINYDNG_OK);
        assert(!tinydng_image_get(document, 0)->exif.has_exif_version);
        tinydng_document_destroy(context, document);
    }
    for (unsigned variation = 0; variation < 3; ++variation) {
        auto malformed = original;
        const size_t gpsEntry = gpsPointerOffset - 8;
        const size_t unknown = variation == 0 ? gpsEntry : orientationEntry;
        assert(unknown);
        malformed[unknown] = 0x25;
        malformed[unknown + 1] = 0x88;
        malformed[unknown + 2] = malformed[unknown + 3] = 0xff;
        if (variation == 2)
            for (size_t index = 0; index < 12; ++index) std::swap(malformed[unknown + index], malformed[gpsEntry + index]);
        assert(tinydng_open_memory(context, malformed.data(), malformed.size(), nullptr, &document, &error) == TINYDNG_OK);
        const auto* loadedGps = tinydng_image_get(document, 0);
        assert(loadedGps->has_gps_directory && !loadedGps->gps.has_coordinates);
        tinydng_document_destroy(context, document);
    }
    tinydng_context_destroy(context);
}

int main() {
    using namespace rawrcam::metadata;
    const char* methods[] = {nullptr, "gps", "network", "fused", nullptr};
    for (int provider = 0; provider < 5; ++provider) {
        const auto gps = makeGps(0, 0, {}, {}, 0, provider);
        assert(gps);
        const char* method = methods[provider];
        assert(gps->processing_method_size == (method ? 8 + std::strlen(method) : 0));
        if (method) {
            assert(std::memcmp(gps->processing_method, "ASCII\0\0\0", 8) == 0);
            assert(std::memcmp(gps->processing_method + 8, method, std::strlen(method)) == 0);
        }
        roundTrip(*gps);
    }
    auto zero = makeGps(0, 0, 0.0, std::nullopt, 0, 1);
    assert(zero && zero->has_coordinates && zero->has_altitude);
    assert(zero->latitude[0][0] == 0 && zero->longitude[0][0] == 0);
    assert(zero->altitude[0] == 0 && zero->altitude[1] != 0);
    assert(!zero->has_horizontal_error);
    assert(zero->altitude_ref == 0);
    assert(zero->version[0] == 2 && zero->version[1] == 4);
    auto reservedVersion = *zero;
    reservedVersion.version[0] = 255;
    const auto normalizedVersion = sanitizeGps(reservedVersion);
    assert(normalizedVersion && normalizedVersion->version[0] == 2 && normalizedVersion->version[1] == 4);
    auto malformedMethod = *zero;
    std::memcpy(malformedMethod.processing_method, "ASCIIxxx", 8);
    const auto withoutMethod = sanitizeGps(malformedMethod);
    assert(withoutMethod && withoutMethod->processing_method_size == 0);
    malformedMethod = *zero;
    malformedMethod.processing_method[8] = 255;
    assert(sanitizeGps(malformedMethod)->processing_method_size == 0);

    auto high = makeGps(-90, -180, -12345.678, 0.0, 1793059200123LL);
    assert(high && high->latitude_ref == 'S' && high->longitude_ref == 'W');
    assert(high->altitude_ref == 1 && high->has_horizontal_error);
    assert(std::abs(double(high->altitude[0]) / high->altitude[1] - 12345.678) < 0.001);
    assert(high->has_time && high->timestamp[2][0] == 123 && high->timestamp[2][1] == 1000);
    assert(validGps(*high));
    roundTrip(*high);
    roundTrip(*zero);

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