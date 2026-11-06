#pragma once

#include <tinydng.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace rawrcam::metadata {

struct ExifTime {
    std::string dateTime;
    std::string offset;
    std::string subsec;
};

inline std::string trimExifText(const char* text) {
    if (!text) return {};
    std::string value(text);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    return value.substr(start);
}

inline std::string exifDateTime(const char* text) {
    auto value = trimExifText(text);
    if (value.size() < 19) return {};
    value.resize(19);
    for (size_t index = 0; index < 19; ++index) {
        const char expected = index == 4 || index == 7 || index == 13 || index == 16 ? ':' : index == 10 ? ' ' : '0';
        if (expected == '0' ? !std::isdigit(static_cast<unsigned char>(value[index])) : value[index] != expected)
            return {};
    }
    const auto pair = [&](size_t index) { return unsigned((value[index] - '0') * 10 + value[index + 1] - '0'); };
    const int year = int(pair(0) * 100 + pair(2));
    const auto month = pair(5), day = pair(8);
    const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (year == 0 || month < 1 || month > 12 || day < 1 ||
        day > days[month - 1] + (month == 2 && leap ? 1u : 0u) ||
        pair(11) > 23 || pair(14) > 59 || pair(17) > 59) return {};
    return value;
}

inline std::string exifOffset(const char* text) {
    auto value = trimExifText(text);
    if (value.size() == 6 && (value[0] == '+' || value[0] == '-') && value[3] == ':' &&
        std::isdigit(static_cast<unsigned char>(value[1])) && std::isdigit(static_cast<unsigned char>(value[2])) &&
        std::isdigit(static_cast<unsigned char>(value[4])) && std::isdigit(static_cast<unsigned char>(value[5]))) {
        const int hours = (value[1] - '0') * 10 + value[2] - '0';
        const int minutes = (value[4] - '0') * 10 + value[5] - '0';
        if (hours < 24 && minutes < 60) return value;
    }
    return {};
}

inline std::string exifSubsec(const char* text) {
    auto value = trimExifText(text);
    for (char digit : value)
        if (!std::isdigit(static_cast<unsigned char>(digit))) return {};
    return value;
}

inline ExifTime readExifTime(const tinydng_exif& image, const tinydng_exif* root,
                             char* tinydng_exif::*date, char* tinydng_exif::*offset,
                             char* tinydng_exif::*subsec) {
    ExifTime time;
    time.dateTime = exifDateTime(image.*date);
    const auto rootDate = root ? exifDateTime(root->*date) : std::string{};
    const auto* selected = time.dateTime.empty() ? root : &image;
    if (time.dateTime.empty()) time.dateTime = rootDate;
    if (time.dateTime.empty()) return {};
    time.offset = exifOffset(selected->*offset);
    time.subsec = exifSubsec(selected->*subsec);
    if (selected == &image && root && rootDate == time.dateTime) {
        const auto rootOffset = exifOffset(root->*offset);
        const auto rootSubsec = exifSubsec(root->*subsec);
        if ((time.offset.empty() || rootOffset.empty() || time.offset == rootOffset) &&
            (time.subsec.empty() || rootSubsec.empty() || time.subsec == rootSubsec)) {
            if (time.offset.empty()) time.offset = rootOffset;
            if (time.subsec.empty()) time.subsec = rootSubsec;
        }
    }
    return time;
}

inline ExifTime exifTime(std::int64_t millis, std::int16_t offsetMinutes) {
    if (millis <= 0) return {};
    const time_t seconds = static_cast<time_t>(millis / 1000 + std::int64_t(offsetMinutes) * 60);
    struct tm local{};
    char date[20]{};
    if (!gmtime_r(&seconds, &local) || !std::strftime(date, sizeof(date), "%Y:%m:%d %H:%M:%S", &local)) return {};
    const int minutes = std::abs(int(offsetMinutes));
    char offset[8]{};
    std::snprintf(offset, sizeof(offset), "%c%02d:%02d", offsetMinutes < 0 ? '-' : '+', minutes / 60, minutes % 60);
    return {date, offset, std::to_string(millis % 1000 + 1000).substr(1)};
}

}  // namespace rawrcam::metadata