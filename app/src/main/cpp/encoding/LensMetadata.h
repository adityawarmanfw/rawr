#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rawrcam::encoding {

struct LensMetadata {
    std::string make;
    std::string model;
    std::optional<std::uint16_t> focalLength35mm;
};

struct LensIdentityOptics {
    std::optional<float> focalLengthMm;
    std::optional<float> maxAperture;
};

inline bool validLensNumber(float value) noexcept {
    return std::isfinite(value) && value > 0.0f;
}

inline std::optional<float> widestLensAperture(const std::vector<float>& apertures) {
    if (apertures.empty()) return std::nullopt;
    const float widest = *std::min_element(apertures.begin(), apertures.end());
    return validLensNumber(widest) ? std::optional<float>(widest) : std::nullopt;
}

inline LensIdentityOptics lensIdentityOptics(const std::vector<float>& focalLengthsMm,
                                            std::optional<float> widestAperture) {
    LensIdentityOptics optics;
    if (focalLengthsMm.size() != 1 || !validLensNumber(focalLengthsMm.front())) return optics;
    optics.focalLengthMm = focalLengthsMm.front();
    optics.maxAperture = widestAperture;
    return optics;
}

inline std::optional<std::array<double, 4>> lensSpecification(const std::vector<float>& focalLengthsMm,
                                                            std::optional<float> widestAperture) {
    if (focalLengthsMm.empty()) return std::nullopt;
    const auto [shortest, longest] = std::minmax_element(focalLengthsMm.begin(), focalLengthsMm.end());
    const double widest = *shortest == *longest ? widestAperture.value_or(0.0f) : 0.0;
    return std::array<double, 4>{*shortest, *longest, widest, widest};
}

inline std::optional<double> lensApertureValue(std::optional<float> aperture) {
    if (!aperture || !validLensNumber(*aperture) || *aperture < 1.0f) return std::nullopt;
    return 2.0 * std::log2(double(*aperture));
}

inline std::optional<std::uint16_t> equivalentFocalLength35mm(
    std::optional<float> focalLengthMm, const std::array<float, 2>& sensorSizeMm) noexcept {
    if (!focalLengthMm || !validLensNumber(*focalLengthMm) ||
        !validLensNumber(sensorSizeMm[0]) || !validLensNumber(sensorSizeMm[1])) return std::nullopt;
    const double diagonal = std::hypot(double(sensorSizeMm[0]), double(sensorSizeMm[1]));
    const double equivalent = double(*focalLengthMm) * std::sqrt(36.0 * 36.0 + 24.0 * 24.0) / diagonal;
    if (equivalent < 1.0 || equivalent > 65535.0) return std::nullopt;
    return static_cast<std::uint16_t>(std::lround(equivalent));
}

inline std::string lensNumber(float value, bool fractional = false) {
    char buffer[64];
    // Shortest round-trip representation of the reported float, independent of locale.
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) return {};
    std::string text(buffer, result.ptr);
    if (fractional && text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

inline std::string lensAsciiIdentity(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    std::string text(value.substr(first, last - first + 1));
    for (char& c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e) c = '?';
    }
    return text;
}

inline bool lensModelIncludesMake(std::string_view model, std::string_view make) noexcept {
    if (model.size() < make.size()) return false;
    for (std::size_t i = 0; i < make.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(model[i]) != lower(make[i])) return false;
    }
    return model.size() == make.size() || model[make.size()] == ' ';
}

// Encoder-worker only: consumes frozen metadata; no device queries, I/O or shared state.
inline LensMetadata buildLensMetadata(std::string_view make, std::string_view model,
                                      std::optional<float> focalLengthMm, const LensIdentityOptics& optics,
                                      const std::array<float, 2>& sensorSizeMm, std::int32_t lensFacing) {
    LensMetadata result;
    result.make = lensAsciiIdentity(make);
    const auto displayModel = lensAsciiIdentity(model);
    result.focalLength35mm = equivalentFocalLength35mm(focalLengthMm, sensorSizeMm);
    if (result.make.empty() || lensModelIncludesMake(displayModel, result.make)) {
        result.model = displayModel;
    } else {
        result.model = result.make;
        if (!displayModel.empty()) result.model += " " + displayModel;
    }
    if (result.model.empty()) return result;
    switch (lensFacing) {
        case 0: result.model += " front"; break;
        case 1: result.model += " back"; break;
        case 2: result.model += " external"; break;
        default: break;
    }
    result.model += " camera";
    if (optics.focalLengthMm) result.model += " " + lensNumber(*optics.focalLengthMm) + "mm";
    if (optics.maxAperture) result.model += " f/" + lensNumber(*optics.maxAperture, true);
    return result;
}

}  // namespace rawrcam::encoding
