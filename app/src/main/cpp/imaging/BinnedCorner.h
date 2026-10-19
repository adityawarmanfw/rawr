#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace rawrcam::imaging {

// Some HALs skip the sensor switch for a full-size still and return the binned
// readout (width/d x height/d) in the top-left corner of the full-size buffer,
// leaving the rest at exactly zero. Sensor data never reads 0 once a black
// level sits above it, so a sparse grid outside the corner tells the two
// apart without scanning the frame.
//
// Returns d (4 or 2) for such a frame, or 1 for a full frame. Packed RAW16.
inline uint32_t binnedCornerDivisor(const uint16_t* px, uint32_t width, uint32_t height) {
    if (!px || width < 64 || height < 64) return 1;
    constexpr uint32_t kGrid = 8;
    const auto emptyOutside = [&](uint32_t d) {
        const uint32_t cw = width / d, ch = height / d;
        uint32_t outside = 0, inside = 0;
        for (uint32_t gy = 0; gy < kGrid; ++gy) {
            for (uint32_t gx = 0; gx < kGrid; ++gx) {
                const uint32_t x = (2 * gx + 1) * width / (2 * kGrid), y = (2 * gy + 1) * height / (2 * kGrid);
                const uint16_t v = px[size_t(y) * width + x];
                if (x >= cw || y >= ch) {
                    if (v != 0) return false;
                    ++outside;
                } else {
                    inside += v != 0;
                }
            }
        }
        return outside > 0 && inside > 0;
    };
    const auto fits = [&](uint32_t d) {
        return width % d == 0 && height % d == 0 && (width / d) % 2 == 0 && (height / d) % 2 == 0;
    };
    if (!fits(2) || !emptyOutside(2)) return 1;
    return fits(4) && emptyOutside(4) ? 4 : 2;
}

// Keeps the top-left width/d x height/d of a packed RAW16 frame, in place.
inline void cropPackedRaw16ToCorner(std::vector<uint8_t>& raw16, uint32_t width, uint32_t height, uint32_t d) {
    if (d <= 1) return;
    const size_t srcStride = size_t(width) * 2, dstStride = size_t(width / d) * 2;
    for (uint32_t y = 0; y < height / d; ++y)
        std::memmove(raw16.data() + y * dstStride, raw16.data() + y * srcStride, dstStride);
    raw16.resize(dstStride * (height / d));
    raw16.shrink_to_fit();
}

struct BinnedCornerCheck {
    uint32_t divisor = 1;        // >1: keep only the top-left width/d x height/d
    const char* reason = "";     // "reported" (HAL result tag) or "empty" (zero samples)
};

// For a full-size buffer (width/height exactly 2x or 4x the binned pixel
// array; anything else is never checked). First the HAL's own report: vivo's
// seamlessMode is 768 when a still switched to the 200 MP mode and 259 for
// 50 MP, so any other value means the binned readout. Without the tag, or
// when it claims the switch, the zero samples decide.
inline BinnedCornerCheck fullSizeBinnedCorner(const std::vector<uint8_t>& raw16, uint32_t width, uint32_t height,
                                              uint32_t pixelArrayWidth, uint32_t pixelArrayHeight,
                                              std::optional<int64_t> seamlessMode) {
    if (!pixelArrayWidth || !pixelArrayHeight || raw16.size() < size_t(width) * height * 2) return {};
    uint32_t d = 0;
    for (const uint32_t k : {2u, 4u})
        if (width == pixelArrayWidth * k && height == pixelArrayHeight * k) d = k;
    if (!d) return {};
    if (seamlessMode && *seamlessMode != (d == 4 ? 768 : 259)) return {d, "reported"};
    if (const uint32_t empty = binnedCornerDivisor(reinterpret_cast<const uint16_t*>(raw16.data()), width, height);
        empty > 1)
        return {empty, "empty"};
    return {};
}

}  // namespace rawrcam::imaging
