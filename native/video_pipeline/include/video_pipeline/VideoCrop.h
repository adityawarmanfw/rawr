#pragma once

#include <cstdint>

namespace rawrcam::video {

// Recording source rect shared by the recording RAW stage and the idle
// viewfinder preview crop, so preview framing matches the recorded frame
// exactly. Single owner of the center-crop rule previously inline in
// VideoDemosaic: optional 2x Bayer reduction, then an even-aligned centered
// crop of the full RAW frame.
struct VideoSourceRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool reduceCfa = false;
};

// Source rect in full-RAW pixels for a recording output size. Returns an
// empty rect (width/height 0) when the output does not fit the sensor.
[[nodiscard]] inline VideoSourceRect videoSourceRect(uint32_t rawWidth, uint32_t rawHeight,
                                                     uint32_t outWidth, uint32_t outHeight) noexcept {
    VideoSourceRect rect{};
    const bool reduce =
        uint64_t(outWidth) * 2u <= rawWidth && uint64_t(outHeight) * 2u <= rawHeight;
    const uint32_t sourceWidth = reduce ? outWidth * 2u : outWidth;
    const uint32_t sourceHeight = reduce ? outHeight * 2u : outHeight;
    if (sourceWidth == 0 || sourceHeight == 0 || sourceWidth > rawWidth || sourceHeight > rawHeight)
        return rect;
    rect.x = ((rawWidth - sourceWidth) / 2u) & ~1u;
    rect.y = ((rawHeight - sourceHeight) / 2u) & ~1u;
    rect.width = sourceWidth;
    rect.height = sourceHeight;
    rect.reduceCfa = reduce;
    return rect;
}

// Image area of the RAW once padded edges are dropped (lens profile
// image.cropEdges: rows or columns a vendor mode delivers with no image).
// Edges are CFA-period multiples, so the origin keeps the colour order.
// Empty (width/height 0) when nothing is dropped or the edges are invalid.
struct VideoKeptArea {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    [[nodiscard]] bool valid() const noexcept { return width != 0 && height != 0; }
    bool operator==(const VideoKeptArea& o) const noexcept {
        return x == o.x && y == o.y && width == o.width && height == o.height;
    }
};

[[nodiscard]] inline VideoKeptArea videoKeptArea(uint32_t rawWidth, uint32_t rawHeight, uint32_t left, uint32_t top,
                                                 uint32_t right, uint32_t bottom) noexcept {
    if (!(left | top | right | bottom)) return {};
    if (uint64_t(left) + right >= rawWidth || uint64_t(top) + bottom >= rawHeight) return {};
    return {left, top, rawWidth - left - right, rawHeight - top - bottom};
}

// Same rule, centred on the kept area instead of the whole RAW, so padded
// edges never reach a recording or its preview. An empty kept area is the
// whole RAW (the rule above, unchanged).
[[nodiscard]] inline VideoSourceRect videoSourceRect(uint32_t rawWidth, uint32_t rawHeight, uint32_t outWidth,
                                                     uint32_t outHeight, const VideoKeptArea& kept) noexcept {
    if (!kept.valid() || uint64_t(kept.x) + kept.width > rawWidth || uint64_t(kept.y) + kept.height > rawHeight)
        return videoSourceRect(rawWidth, rawHeight, outWidth, outHeight);
    VideoSourceRect rect = videoSourceRect(kept.width, kept.height, outWidth, outHeight);
    if (rect.width == 0) return rect;
    rect.x += kept.x;
    rect.y += kept.y;
    return rect;
}

// The same window in the frame mirrored horizontally and/or vertically. A sampler that mirrors the
// whole frame after mapping a window into it (the live preview's lens mirror) needs this window to
// show the original one's pixels; a window centred on the frame is its own mirror. A window that
// does not fit the frame is returned unchanged.
[[nodiscard]] inline VideoSourceRect mirrorVideoSourceRect(VideoSourceRect rect, uint32_t width, uint32_t height,
                                                           bool horizontal, bool vertical) noexcept {
    if (rect.width == 0 || rect.height == 0 || uint64_t(rect.x) + rect.width > width ||
        uint64_t(rect.y) + rect.height > height)
        return rect;
    if (horizontal) rect.x = width - rect.x - rect.width;
    if (vertical) rect.y = height - rect.y - rect.height;
    return rect;
}

// Scales a full-RAW-space rect into another source space (e.g. the half-res
// preview image). Exact for even halvings; otherwise truncates with even
// alignment and clamps inside the target. Returns empty when degenerate.
[[nodiscard]] inline VideoSourceRect scaleVideoSourceRect(const VideoSourceRect& rect, uint32_t fromWidth,
                                                          uint32_t fromHeight, uint32_t toWidth,
                                                          uint32_t toHeight) noexcept {
    VideoSourceRect out{};
    out.reduceCfa = rect.reduceCfa;
    if (fromWidth == 0 || fromHeight == 0 || toWidth == 0 || toHeight == 0 || rect.width == 0 ||
        rect.height == 0)
        return VideoSourceRect{};
    const uint64_t x = (uint64_t(rect.x) * toWidth / fromWidth) & ~1u;
    const uint64_t y = (uint64_t(rect.y) * toHeight / fromHeight) & ~1u;
    uint64_t w = uint64_t(rect.width) * toWidth / fromWidth;
    uint64_t h = uint64_t(rect.height) * toHeight / fromHeight;
    if (x >= toWidth || y >= toHeight) return VideoSourceRect{};
    if (x + w > toWidth) w = toWidth - x;
    if (y + h > toHeight) h = toHeight - y;
    if (w == 0 || h == 0) return VideoSourceRect{};
    out.x = static_cast<uint32_t>(x);
    out.y = static_cast<uint32_t>(y);
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    return out;
}

}  // namespace rawrcam::video
