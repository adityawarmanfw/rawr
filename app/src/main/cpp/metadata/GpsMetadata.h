#pragma once

#include "tinydng.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>

namespace rawrcam::metadata {

inline bool gpsRational(double value, uint32_t (&output)[2]) {
    if (!std::isfinite(value) || value < 0 || value > UINT32_MAX) return false;
    uint32_t denominator = 1'000'000;
    while (denominator > 1 && value * denominator > double(UINT32_MAX) - 0.5) denominator /= 10;
    const auto numerator = std::llround(value * denominator);
    if (numerator < 0 || uint64_t(numerator) > UINT32_MAX) return false;
    output[0] = uint32_t(numerator);
    output[1] = denominator;
    return true;
}

inline void gpsDegrees(double value, uint32_t (&output)[3][2]) {
    const auto ticks = uint64_t(std::llround(std::abs(value) * 3'600'000'000.0));
    output[0][0] = uint32_t(ticks / 3'600'000'000ULL);
    output[0][1] = 1;
    output[1][0] = uint32_t(ticks / 60'000'000ULL % 60);
    output[1][1] = 1;
    output[2][0] = uint32_t(ticks % 60'000'000ULL);
    output[2][1] = 1'000'000;
}

inline bool validGpsCoordinate(const uint32_t (&value)[3][2], double limit) {
    return tinydng_gps_coordinate_valid(value, limit) != 0;
}

inline bool validGps(const tinydng_gps& gps) {
    return gps.has_coordinates && (gps.latitude_ref == 'N' || gps.latitude_ref == 'S') &&
        (gps.longitude_ref == 'E' || gps.longitude_ref == 'W') &&
        validGpsCoordinate(gps.latitude, 90) && validGpsCoordinate(gps.longitude, 180);
}

inline bool validGpsTime(const tinydng_gps& gps) {
    if (!gps.has_time || gps.datestamp[10] != '\0') return false;
    for (size_t index = 0; index < 10; ++index) {
        const char digit = gps.datestamp[index];
        if (index == 4 || index == 7) { if (digit != ':') return false; }
        else if (digit < '0' || digit > '9') return false;
    }
    const auto pair = [&](size_t index) { return (gps.datestamp[index] - '0') * 10 + gps.datestamp[index + 1] - '0'; };
    const int year = pair(0) * 100 + pair(2);
    const int month = pair(5), day = pair(8);
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (year == 0 || month < 1 || month > 12 || day < 1 ||
        day > days[month - 1] + (month == 2 && leap ? 1 : 0)) return false;
    for (const auto& part : gps.timestamp) if (!part[1]) return false;
    return double(gps.timestamp[0][0]) / gps.timestamp[0][1] < 24 &&
        double(gps.timestamp[1][0]) / gps.timestamp[1][1] < 60 &&
        double(gps.timestamp[2][0]) / gps.timestamp[2][1] < 61;
}

inline std::optional<tinydng_gps> sanitizeGps(tinydng_gps gps) {
    if (!validGps(gps)) return std::nullopt;
    gps.has_version = 1;
    gps.version[0] = 2;
    gps.version[1] = 4;
    gps.version[2] = gps.version[3] = 0;
    if (!gps.altitude[1] || gps.altitude_ref > 3) gps.has_altitude = 0;
    if (!gps.horizontal_error[1]) gps.has_horizontal_error = 0;
    if (!validGpsTime(gps)) gps.has_time = 0;
    if (!std::memchr(gps.map_datum, 0, sizeof(gps.map_datum))) gps.map_datum[0] = 0;
    if (!tinydng_gps_method_valid(&gps)) gps.processing_method_size = 0;
    if (!gps.speed[1] || (gps.speed_ref != 'K' && gps.speed_ref != 'M' && gps.speed_ref != 'N')) gps.has_speed = 0;
    if (!gps.track[1] || (gps.track_ref != 'T' && gps.track_ref != 'M') ||
        (gps.track[1] && double(gps.track[0]) / gps.track[1] >= 360)) gps.has_track = 0;
    return gps;
}

inline bool sameGpsRecord(const tinydng_gps& left, const tinydng_gps& right) {
    return left.has_version == right.has_version &&
        (!left.has_version || std::memcmp(left.version, right.version, 4) == 0) &&
        left.latitude_ref == right.latitude_ref && left.longitude_ref == right.longitude_ref &&
        std::memcmp(left.latitude, right.latitude, sizeof(left.latitude)) == 0 &&
        std::memcmp(left.longitude, right.longitude, sizeof(left.longitude)) == 0 &&
        left.has_altitude == right.has_altitude && (!left.has_altitude ||
            (left.altitude_ref == right.altitude_ref && std::memcmp(left.altitude, right.altitude, sizeof(left.altitude)) == 0)) &&
        left.has_time == right.has_time && (!left.has_time ||
            (std::memcmp(left.datestamp, right.datestamp, sizeof(left.datestamp)) == 0 &&
             std::memcmp(left.timestamp, right.timestamp, sizeof(left.timestamp)) == 0)) &&
        std::strcmp(left.map_datum, right.map_datum) == 0 &&
        left.has_horizontal_error == right.has_horizontal_error && (!left.has_horizontal_error ||
            std::memcmp(left.horizontal_error, right.horizontal_error, sizeof(left.horizontal_error)) == 0) &&
        left.processing_method_size == right.processing_method_size &&
        std::memcmp(left.processing_method, right.processing_method, left.processing_method_size) == 0 &&
        left.has_speed == right.has_speed && (!left.has_speed || (left.speed_ref == right.speed_ref &&
            std::memcmp(left.speed, right.speed, sizeof(left.speed)) == 0)) &&
        left.has_track == right.has_track && (!left.has_track || (left.track_ref == right.track_ref &&
            std::memcmp(left.track, right.track, sizeof(left.track)) == 0));
}

struct SourceGps {
    std::optional<tinydng_gps> gps;
    std::optional<std::array<uint8_t, 4>> exifVersion;
};

inline std::optional<std::array<uint8_t, 4>> gpsExifVersion(const tinydng_exif* exif) {
    if (!exif || !exif->has_exif_version) return std::nullopt;
    for (const char* supported : {"0200", "0210", "0220", "0221", "0230", "0231", "0232", "0300"}) {
        if (std::memcmp(exif->exif_version, supported, 4) == 0) {
            std::array<uint8_t, 4> version{};
            std::memcpy(version.data(), supported, version.size());
            return version;
        }
    }
    return std::nullopt;
}

inline SourceGps readSourceGps(const tinydng_image_info& image, const tinydng_image_info* root) {
    const auto* selected = image.has_gps_directory ? &image : root;
    if (!selected || !selected->has_gps_directory) return {};
    auto gps = sanitizeGps(selected->gps);
    if (!gps) return {};
    const auto* exif = selected->exif.has_exif_version ? &selected->exif : (root ? &root->exif : nullptr);
    const auto version = gpsExifVersion(exif);
    if (selected == &image && root && root != &image && root->has_gps_directory) {
        const auto rootGps = sanitizeGps(root->gps);
        if (!rootGps || !sameGpsRecord(*gps, *rootGps)) return {};
        if (gps->has_altitude && gps->altitude_ref <= 1) {
            const auto rootVersion = gpsExifVersion(&root->exif);
            if (version.has_value() != rootVersion.has_value() ||
                (version && ((*version)[1] == '3') != ((*rootVersion)[1] == '3'))) return {};
        }
    }
    SourceGps result{gps, version};
    if (!result.exifVersion && result.gps->has_altitude && result.gps->altitude_ref <= 1)
        result.gps->has_altitude = 0;
    if (result.exifVersion && (*result.exifVersion)[1] != '3' && result.gps->altitude_ref >= 2)
        result.gps->has_altitude = 0;
    if (!result.exifVersion) result.exifVersion = std::array<uint8_t, 4>{'0', '3', '0', '0'};
    return result;
}

inline std::optional<tinydng_gps> makeGps(double latitude, double longitude, std::optional<double> altitude,
                                        std::optional<double> horizontalError, int64_t fixUnixMillis,
                                        int provider = 0) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || std::abs(latitude) > 90 ||
        std::abs(longitude) > 180) return std::nullopt;
    tinydng_gps gps{};
    gps.has_version = 1;
    gps.version[0] = 2;
    gps.version[1] = 4;
    gps.has_coordinates = 1;
    gps.latitude_ref = latitude < 0 ? 'S' : 'N';
    gps.longitude_ref = longitude < 0 ? 'W' : 'E';
    gpsDegrees(latitude, gps.latitude);
    gpsDegrees(longitude, gps.longitude);
    std::memcpy(gps.map_datum, "WGS-84", 7);
    const char* method = provider == 1 ? "gps" : provider == 2 ? "network" : provider == 3 ? "fused" : nullptr;
    if (method) {
        std::memcpy(gps.processing_method, "ASCII\0\0\0", 8);
        const size_t length = std::strlen(method);
        std::memcpy(gps.processing_method + 8, method, length);
        gps.processing_method_size = 8 + length;
    }
    if (altitude && gpsRational(std::abs(*altitude), gps.altitude)) {
        gps.has_altitude = 1;
        gps.altitude_ref = *altitude < 0 ? 1 : 0;
    }
    if (horizontalError && gpsRational(*horizontalError, gps.horizontal_error)) gps.has_horizontal_error = 1;
    if (fixUnixMillis > 0 && fixUnixMillis < 253402300800000LL) {
        const time_t seconds = time_t(fixUnixMillis / 1000);
        std::tm utc{};
        if (gmtime_r(&seconds, &utc) && std::strftime(gps.datestamp, sizeof(gps.datestamp), "%Y:%m:%d", &utc)) {
            gps.timestamp[0][0] = uint32_t(utc.tm_hour);
            gps.timestamp[0][1] = 1;
            gps.timestamp[1][0] = uint32_t(utc.tm_min);
            gps.timestamp[1][1] = 1;
            gps.timestamp[2][0] = uint32_t(utc.tm_sec * 1000 + fixUnixMillis % 1000);
            gps.timestamp[2][1] = 1000;
            gps.has_time = 1;
        }
    }
    return gps;
}

}