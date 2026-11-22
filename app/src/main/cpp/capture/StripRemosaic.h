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

// Same strips, two windows in flight: strip i is copied in and submitted
// before strip i-1 is waited for and copied out, so the host copies and the
// gaps between submissions overlap the GPU work. submit(window, slot, strip)
// starts the pass on windows[slot] (slot = strip & 1); wait(slot) blocks until
// that slot's pass is done (its result is then in windows[slot]). Strip i's
// window may reach into strip i-2's core, which is already written back; those
// rows come from the originals saved before that write-back (`saved` rows).
// Bit-identical to remosaicInStrips.
template <class Submit, class Wait>
void remosaicInStripsOverlapped(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                                const StripLayout& layout, uint8_t* const windows[2], Submit&& submit, Wait&& wait) {
    const size_t rowBytes = size_t(width) * 2;
    if (pixels.size() != rowBytes * height) throw std::invalid_argument("strip_remosaic_size");
    const uint32_t strips = layout.count(height);
    const auto coreStart = [&](uint32_t i) { return i * layout.core; };
    const auto coreEnd = [&](uint32_t i) { return std::min(height, coreStart(i) + layout.core); };
    const auto windowStart = [&](uint32_t i) {
        return layout.window >= height ? 0u
                                       : std::min(coreStart(i) < StripLayout::kMargin ? 0u : coreStart(i) - StripLayout::kMargin,
                                                  height - layout.window);
    };
    std::vector<uint8_t> tail(size_t(layout.saved) * rowBytes);
    uint32_t written = 0;  // rows [0, written) of `pixels` already hold Bayer
    const auto copyOut = [&](uint32_t j) {
        wait(j & 1u);
        // Keep the originals of this core's last rows for a later window that reaches back into it.
        if (layout.saved && coreEnd(j) < height)
            std::memcpy(tail.data(), pixels.data() + size_t(coreEnd(j) - layout.saved) * rowBytes,
                        size_t(layout.saved) * rowBytes);
        std::memcpy(pixels.data() + size_t(coreStart(j)) * rowBytes,
                    windows[j & 1u] + size_t(coreStart(j) - windowStart(j)) * rowBytes,
                    size_t(coreEnd(j) - coreStart(j)) * rowBytes);
        written = coreEnd(j);
    };
    for (uint32_t i = 0; i < strips; ++i) {
        if (i >= 2) copyOut(i - 2);  // frees windows[i & 1]
        const uint32_t start = windowStart(i);
        uint8_t* window = windows[i & 1u];
        const uint32_t fromTail = written > start ? written - start : 0;
        if (fromTail > layout.saved) throw std::logic_error("strip_remosaic_layout");
        if (fromTail)
            std::memcpy(window, tail.data() + size_t(layout.saved - fromTail) * rowBytes, size_t(fromTail) * rowBytes);
        std::memcpy(window + size_t(fromTail) * rowBytes, pixels.data() + size_t(start + fromTail) * rowBytes,
                    size_t(layout.window - fromTail) * rowBytes);
        submit(window, i & 1u, i);
    }
    for (uint32_t j = strips >= 2 ? strips - 2 : 0; j < strips; ++j) copyOut(j);
}
}  // namespace rawrcam::capture
