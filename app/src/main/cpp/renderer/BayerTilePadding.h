#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rawrcam::renderer {
// Extend the last sample of the same CFA parity; never mix Bayer colors.
inline std::vector<float> padBayerTile(const std::vector<float>& source, uint32_t width, uint32_t height,
                                     uint32_t paddedWidth, uint32_t paddedHeight) {
    if (width < 2 || height < 2 || paddedWidth < width || paddedHeight < height ||
        source.size() != size_t(width) * height)
        throw std::invalid_argument("Invalid Bayer tile padding dimensions");
    std::vector<float> result(size_t(paddedWidth) * paddedHeight);
    auto edge = [](uint32_t position, uint32_t length) {
        if (position < length) return position;
        const uint32_t last = length - 1;
        return last - ((last ^ position) & 1u);
    };
    for (uint32_t y = 0; y < paddedHeight; ++y)
        for (uint32_t x = 0; x < paddedWidth; ++x)
            result[size_t(y) * paddedWidth + x] = source[size_t(edge(y, height)) * width + edge(x, width)];
    return result;
}
}  // namespace rawrcam::renderer
