#include <cassert>
#include <cstdint>
#include <vector>

#include "capture/multiframe/StripMerge.h"

using rawrcam::capture::multiframe::stripWindows;
using rawrcam::capture::multiframe::writeStrip;

int main() {
    // Fits: one window, writes everything.
    {
        const auto w = stripWindows(8160, 6144, 56'000'000);
        assert(w.size() == 1 && w[0].y0 == 0 && w[0].rows == 6144 && w[0].writeEnd == 6144);
    }
    // Fits with a height off the 64-row grid (vivo H35 50 MP: 6128 rows): still one window.
    for (const std::uint32_t height : {6128u, 3064u}) {
        const auto w = stripWindows(8192, height, 56'000'000);
        assert(w.size() == 1 && w[0].rows == height && w[0].writeEnd == height);
    }
    // vivo 200 MP tele: four ~55 MP windows, even starts, full coverage, bands inside overlaps.
    for (const std::uint32_t height : {12288u, 12256u}) {
        const auto w = stripWindows(16320, height, 56'000'000);
        assert(w.size() == 4);
        assert(w[0].y0 == 0 && w[0].writeBegin == 0 && w.back().y0 + w.back().rows == height);
        assert(w.back().writeEnd == height);
        for (std::size_t k = 0; k < w.size(); ++k) {
            assert(w[k].rows % 64 == 0 && w[k].rows == w[0].rows && w[k].y0 % 2 == 0);
            assert(std::uint64_t(w[k].rows) * 16320 <= 56'000'000);
            assert(w[k].writeBegin >= w[k].y0 && w[k].writeEnd <= w[k].y0 + w[k].rows);
            assert(w[k].writeBegin % 2 == 0 && w[k].blendEnd % 2 == 0);
            if (k) {
                // Contiguous: the band starts where the previous window wrote up to it.
                assert(w[k - 1].writeEnd == w[k].blendEnd && w[k].blendEnd - w[k].writeBegin == 128);
                // Band well inside the overlap: away from both windows' edges.
                assert(w[k].writeBegin >= w[k].y0 + 64 && w[k].blendEnd + 64 <= w[k - 1].y0 + w[k - 1].rows);
            }
        }
    }
    // Stitching: constant windows reproduce the constant; a step crossfades monotonically.
    {
        const std::uint32_t width = 4, height = 1024;
        const auto w = stripWindows(width, height, 4 * 448, 128, 32);
        assert(w.size() > 1);
        std::vector<std::uint16_t> full(std::size_t(width) * height, 0);
        for (std::size_t k = 0; k < w.size(); ++k) {
            std::vector<std::uint16_t> strip(std::size_t(width) * w[k].rows, std::uint16_t(1000 + 100 * k));
            writeStrip(full.data(), width, w[k], strip.data());
        }
        assert(full[0] == 1000 && full[full.size() - 1] == 1000 + 100 * (w.size() - 1));
        for (std::uint32_t y = 1; y < height; ++y) assert(full[y * width] >= full[(y - 1) * width]);
        // Outside the bands each row holds exactly one window's value.
        assert(full[std::size_t(w[1].blendEnd) * width] == 1100 && full[std::size_t(w[1].writeBegin - 1) * width] == 1000);
    }
    return 0;
}
