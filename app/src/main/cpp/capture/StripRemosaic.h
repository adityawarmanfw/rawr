#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace rawrcam::capture {
// Runs an in-place per-window pass over a RAW16 frame in horizontal strips,
// so the pass only needs window-sized memory. Each window is a strip's own
// rows ("core") plus kMargin rows above and below, more than the block-CFA
// remosaic reads (16 rows suffice for every quality on 2x2 and 4x4 blocks),
// so the result is bit-identical to one whole-frame pass. Cores and windows
// are multiples of 8 rows: block grid and CFA phase survive. The last window
// is pulled up to end at the frame (same edge clamping as the whole frame),
// so it can reach rows above its margin; `saved` original rows of each
// strip's bottom are kept because the frame is overwritten in place.
struct StripLayout {
    static constexpr uint32_t kMargin = 32;
    uint32_t core = 0, window = 0, saved = 0;
    // One strip (window == height) unless the height is a multiple of 8 and a
    // strip layout fits.
    static StripLayout make(uint32_t height, uint32_t strips) {
        StripLayout l{height, height, 0};
        strips = std::max(1u, strips);
        if (height % 8 != 0 || strips == 1) return l;
        const uint32_t core = (height / strips + 7) / 8 * 8, saved = 2 * kMargin + 8 * strips;
        if (core >= saved && core + 2 * kMargin < height) l = {core, core + 2 * kMargin, saved};
        return l;
    }
    uint32_t count(uint32_t height) const { return (height + core - 1) / core; }
};

// `window` holds layout.window rows of `width` samples (caller-owned, e.g.
// mapped GPU memory); pass(window, stripIndex) remosaics it in place.
template <class Pass>
void remosaicInStrips(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, const StripLayout& layout,
                      uint8_t* window, Pass&& pass) {
    const size_t rowBytes = size_t(width) * 2;
    if (pixels.size() != rowBytes * height) throw std::invalid_argument("strip_remosaic_size");
    std::vector<uint8_t> tail(size_t(layout.saved) * rowBytes);
    const uint32_t strips = layout.count(height);
    for (uint32_t i = 0; i < strips; ++i) {
        const uint32_t coreStart = i * layout.core, coreEnd = std::min(height, coreStart + layout.core);
        const uint32_t start =
            layout.window >= height
                ? 0
                : std::min(coreStart < StripLayout::kMargin ? 0 : coreStart - StripLayout::kMargin,
                           height - layout.window);
        // Rows above this core were already remosaiced in `pixels`: the window takes their originals.
        const uint32_t fromTail = coreStart > start ? coreStart - start : 0;
        if (fromTail > layout.saved) throw std::logic_error("strip_remosaic_layout");
        if (fromTail)
            std::memcpy(window, tail.data() + size_t(layout.saved - fromTail) * rowBytes, size_t(fromTail) * rowBytes);
        std::memcpy(window + size_t(fromTail) * rowBytes, pixels.data() + size_t(start + fromTail) * rowBytes,
                    size_t(layout.window - fromTail) * rowBytes);
        pass(window, i);
        if (layout.saved && coreEnd < height)
            std::memcpy(tail.data(), pixels.data() + size_t(coreEnd - layout.saved) * rowBytes,
                        size_t(layout.saved) * rowBytes);
        std::memcpy(pixels.data() + size_t(coreStart) * rowBytes, window + size_t(coreStart - start) * rowBytes,
                    size_t(coreEnd - coreStart) * rowBytes);
    }
}
}  // namespace rawrcam::capture
