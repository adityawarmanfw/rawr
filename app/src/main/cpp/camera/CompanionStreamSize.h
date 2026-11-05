#pragma once

#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <utility>
#include <vector>

namespace rawrcam::camera {

// What an automatic companion size does when the camera advertises no output with the RAW stream's shape.
enum class CompanionFallback : uint8_t {
    Base,     // the smallest output of at least 320x240, whatever its shape (the default)
    Nearest,  // the output whose shape is closest to the RAW's, among those at least 320 wide
    None,     // no companion
};

struct CompanionSizeChoice {
    int32_t width = 0;
    int32_t height = 0;
    bool sameShape = false;  // Matches the RAW stream's aspect ratio.
};

// Within 2% of the RAW stream's aspect ratio.
inline bool sameAspect(int32_t width, int32_t height, int32_t rawWidth, int32_t rawHeight) {
    const int64_t a = int64_t(width) * rawHeight, b = int64_t(height) * rawWidth;
    return std::llabs(a - b) * 50 <= b;
}

// Size of the YUV companion stream from the camera's advertised YUV outputs.
// The base pick is the smallest output of at least 320x240. When its shape
// differs from the RAW stream's (a 16:9 RAW gets a 4:3 YUV), the smallest
// same-shape output at least 320 wide replaces it: a vendor sensor mode can
// fail every frame when the ISP must also produce the other shape. Without a
// same-shape output the base pick stays. {0, 0} when nothing qualifies.
inline CompanionSizeChoice pickCompanionSize(const std::vector<std::pair<int32_t, int32_t>>& yuvSizes,
                                             int32_t rawWidth, int32_t rawHeight,
                                             CompanionFallback fallback = CompanionFallback::Base) {
    const auto smaller = [](const CompanionSizeChoice& best, int32_t w, int32_t h) {
        return best.width == 0 || int64_t(w) * h < int64_t(best.width) * best.height;
    };
    CompanionSizeChoice base;
    for (const auto& [w, h] : yuvSizes)
        if (w >= 320 && h >= 240 && smaller(base, w, h)) base = {w, h, false};
    if (rawWidth <= 0 || rawHeight <= 0) return base;
    if (base.width > 0 && sameAspect(base.width, base.height, rawWidth, rawHeight)) {
        base.sameShape = true;
        return base;
    }
    CompanionSizeChoice shaped;
    for (const auto& [w, h] : yuvSizes)
        if (w >= 320 && h > 0 && sameAspect(w, h, rawWidth, rawHeight) && smaller(shaped, w, h))
            shaped = {w, h, true};
    if (shaped.width > 0) return shaped;
    if (fallback == CompanionFallback::None) return {};
    if (fallback == CompanionFallback::Nearest) {
        // |w/h - rawW/rawH|, compared by cross-multiplication in floating point; the smaller output wins a tie.
        const double rawAspect = double(rawWidth) / double(rawHeight);
        CompanionSizeChoice nearest;
        double best = 0;
        for (const auto& [w, h] : yuvSizes) {
            if (w < 320 || h <= 0) continue;
            const double d = std::abs(double(w) / double(h) - rawAspect);
            if (nearest.width == 0 || d < best - 1e-9 || (d < best + 1e-9 && smaller(nearest, w, h))) {
                nearest = {w, h, false};
                best = d;
            }
        }
        if (nearest.width > 0) return nearest;
    }
    return base;
}

}  // namespace rawrcam::camera
