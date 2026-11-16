#include <cassert>
#include <cstdint>
#include <vector>

#include "capture/multiframe/StripMerge.h"

using rawrcam::capture::multiframe::memoryFitStripPixels;
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
    // Memory plan: a Rexture-like cost of 99 bytes per pixel (arena + resident frames).
    {
        const auto need = [](std::uint32_t rows) { return std::uint64_t(rows) * 8160u * 99u; };
        const std::uint64_t whole = need(6144);
        // Fits: untouched, so stripWindows() leaves the 50 MP frame in one window.
        assert(memoryFitStripPixels(8160, 6144, 56'000'000, 16'000'000, whole, need) == 56'000'000);
        assert(stripWindows(8160, 6144, 56'000'000).size() == 1);
        // One byte short: split, into windows that fit the budget and are not needlessly small.
        for (const std::uint64_t budget : {whole - 1, whole * 3 / 4, whole / 2, whole / 3}) {
            const std::uint64_t px = memoryFitStripPixels(8160, 6144, 56'000'000, 16'000'000, budget, need);
            const std::uint32_t rows = std::uint32_t(px / 8160u);
            assert(px % 8160u == 0 && rows % 64u == 0 && need(rows) <= budget && need(rows + 64u) > budget);
            const auto w = stripWindows(8160, 6144, px);
            assert(w.size() > 1 && w[0].rows == rows && w.back().writeEnd == 6144);
        }
        // Nothing fits: the smallest window (16 MP) is still returned.
        assert(memoryFitStripPixels(8160, 6144, 56'000'000, 16'000'000, 1, need) / 8160u == 16'000'000 / 8160u / 64u * 64u);
        // 200 MP frame that "fits" nothing but the cap: limited by maxPixels, as before.
        assert(memoryFitStripPixels(16320, 12288, 56'000'000, 16'000'000, ~0ull, [](std::uint32_t) { return 0ull; }) < 56'000'001);
    }
    // Balanced windows: same count, shortest rows that cover the frame with the minimum overlap, still within the limit.
    {
        const auto tall = stripWindows(8160, 6144, 31'300'000);
        const auto bal = stripWindows(8160, 6144, 31'300'000, 256, 128, true);
        assert(tall.size() == 2 && bal.size() == 2 && bal[0].rows < tall[0].rows);
        assert(bal[0].rows % 64 == 0 && std::uint64_t(bal[0].rows) * 8160 <= 31'300'000);
        assert(bal[0].y0 == 0 && bal.back().y0 + bal.back().rows == 6144 && bal.back().writeEnd == 6144);
        assert(bal[0].y0 + bal[0].rows - bal[1].y0 >= 256 && bal[0].y0 + bal[0].rows - bal[1].y0 < 256 + 64 + 64);
        // Off by default: the existing 200 MP geometry is unchanged.
        assert(stripWindows(16320, 12288, 56'000'000).size() == 4 && stripWindows(16320, 12288, 56'000'000)[0].rows == 3392);
        // One window stays one window.
        assert(stripWindows(8160, 6144, 56'000'000, 256, 128, true).size() == 1);
    }
    return 0;
}
