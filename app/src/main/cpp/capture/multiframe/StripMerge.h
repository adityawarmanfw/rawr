#pragma once
// Strip merge geometry: a burst too large to merge at once (vivo 200 MP) is
// merged in overlapping horizontal windows, each the size of a merge that
// fits, and stitched with a crossfade in the middle of each overlap.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rawrcam::capture::multiframe {

struct StripWindow {
    std::uint32_t y0 = 0;    // first source row of the window (even: CFA phase kept)
    std::uint32_t rows = 0;  // window height (all windows alike: one merge arena)
    // Output rows this window writes: [writeBegin, writeEnd). Rows below
    // blendEnd crossfade from the previous window's values to this one's.
    std::uint32_t writeBegin = 0, blendEnd = 0, writeEnd = 0;
};

// Windows of at most `maxPixels` (rows a multiple of 64) covering `height`,
// overlapping by at least `minOverlap` rows; one window when the frame fits.
inline std::vector<StripWindow> stripWindows(std::uint32_t width, std::uint32_t height, std::uint64_t maxPixels,
                                             std::uint32_t minOverlap = 256, std::uint32_t blendRows = 128) {
    std::vector<StripWindow> out;
    if (!width || !height) return out;
    // Fits test before the 64-row rounding: a frame whose height is not a
    // multiple of 64 (vivo H35: 6128 rows) must not split into strips.
    const std::uint64_t maxRows = maxPixels / width;
    const std::uint32_t rows = std::uint32_t(std::min<std::uint64_t>(height, maxRows)) / 64u * 64u;
    if (maxRows >= height || rows <= minOverlap) {
        out.push_back({0, height, 0, 0, height});
        return out;
    }
    const std::uint32_t count = (height - minOverlap + (rows - minOverlap) - 1) / (rows - minOverlap);
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint64_t start = std::uint64_t(k) * (height - rows) / (count - 1);
        out.push_back({std::uint32_t(start) & ~1u, rows, 0, 0, 0});
    }
    out.back().y0 = height - rows;  // even: height and rows are
    // Blend band centred in each overlap.
    for (std::size_t k = 1; k < out.size(); ++k) {
        const std::uint32_t overlapBegin = out[k].y0, overlapEnd = out[k - 1].y0 + rows;
        const std::uint32_t band = std::min(blendRows, (overlapEnd - overlapBegin) / 2u) & ~1u;
        const std::uint32_t mid = ((overlapBegin + overlapEnd) / 2u) & ~1u;
        out[k].writeBegin = mid - band / 2u;
        out[k].blendEnd = out[k].writeBegin + band;
    }
    for (std::size_t k = 0; k < out.size(); ++k)
        out[k].writeEnd = k + 1 < out.size() ? out[k + 1].blendEnd : height;
    return out;
}

// Writes window `w`'s merged rows (`strip`, w.rows x width) into `full`
// (height x width), crossfading [writeBegin, blendEnd) with what the previous
// window wrote there. Values are RAW16 codes at the same CFA sites.
inline void writeStrip(std::uint16_t* full, std::uint32_t width, const StripWindow& w, const std::uint16_t* strip) {
    for (std::uint32_t y = w.writeBegin; y < w.writeEnd; ++y) {
        const std::uint16_t* src = strip + std::size_t(y - w.y0) * width;
        std::uint16_t* dst = full + std::size_t(y) * width;
        if (y >= w.blendEnd) {
            std::memcpy(dst, src, std::size_t(width) * sizeof(std::uint16_t));
            continue;
        }
        const float t = (float(y - w.writeBegin) + 0.5f) / float(w.blendEnd - w.writeBegin);
        for (std::uint32_t x = 0; x < width; ++x)
            dst[x] = std::uint16_t(float(dst[x]) + (float(src[x]) - float(dst[x])) * t + 0.5f);
    }
}

}  // namespace rawrcam::capture::multiframe
