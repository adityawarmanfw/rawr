#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "renderer/BayerBin.h"
#include "renderer/BayerTilePadding.h"
#include "renderer/NaturalBayerBin.h"

int main() {
    // Non-aligned sensor borders must retain each Bayer site's color and all
    // original pixels when padded for Quad Filter's 16-pixel workgroups.
    for (const auto [w, h] : {std::pair{18u, 24u}, std::pair{17u, 23u}, std::pair{64u, 64u}}) {
        std::vector<float> source(size_t(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) source[size_t(y) * w + x] = float(y * 1000 + x);
        const auto padded = rawrcam::renderer::padBayerTile(source, w, h, 64, 64);
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x) {
                const auto value = uint32_t(padded[size_t(y) * 64 + x]);
                const auto sy = value / 1000, sx = value % 1000;
                if (sx >= w || sy >= h || (sx & 1) != (x & 1) || (sy & 1) != (y & 1)) return 20;
                if (x < w && y < h && value != y * 1000 + x) return 21;
            }
    }
    using rawrcam::renderer::binSameColor2x;
    constexpr uint32_t width = 128, height = 96;
    auto constant = [](uint32_t x, uint32_t y) { return float((x & 1u) + 2u * (y & 1u)); };
    for (uint32_t y = 0; y < height / 2; ++y)
        for (uint32_t x = 0; x < width / 2; ++x)
            if (std::abs(binSameColor2x(x, y, width, height, constant) - constant(x, y)) > 1e-6f) return 1;

    // Every parity must have the same edge center. A constant-center filter
    // also suppresses a source-period-4 checkerboard on each color lattice.
    for (uint32_t parityY = 0; parityY < 2; ++parityY)
        for (uint32_t parityX = 0; parityX < 2; ++parityX) {
            auto edge = [](uint32_t x, uint32_t) { return x >= 64 ? 1.0f : 0.0f; };
            const float a = binSameColor2x(30u + parityX, 20u + parityY, width, height, edge);
            const float b = binSameColor2x(32u + parityX, 20u + parityY, width, height, edge);
            if (a < 0.0f || a > 1.0f || b < 0.0f || b > 1.0f || a > b) return 2;
        }
    // At a vertical edge the sharp kernel places 85% of the bright side
    // into its centered even site, matching rawr-dng's selected filter.
    auto edge = [](uint32_t x, uint32_t) { return x >= 64 ? 1.0f : 0.0f; };
    if (std::abs(binSameColor2x(32, 20, width, height, edge) - 0.85f) > 1e-6f) return 4;
    auto ramp = [](uint32_t x, uint32_t y) { return float(x + 2u * y); };
    for (uint32_t y = 3; y < height / 2 - 3; ++y)
        for (uint32_t x = 3; x < width / 2 - 3; ++x)
            if (std::abs(binSameColor2x(x, y, width, height, ramp) - ramp(2u * x, 2u * y)) > 1e-4f) return 5;
    auto neutral = [](uint32_t x, uint32_t y) { return float((x / 2u + y / 2u) & 1u); };
    for (uint32_t y = 3; y < height / 2 - 3; ++y)
        for (uint32_t x = 3; x < width / 2 - 3; ++x)
            if (std::abs(binSameColor2x(x, y, width, height, neutral) - .5f) > .001f) return 3;
    // Golden output from the supplied Rust reconstruction, with integer
    // rounding and no dither. Input is synthetic and contains no photo data.
    {
        constexpr int w = 96, h = 80;
        float black[] = {64, 128, 128, 96};
        auto sample = [&](int x, int y) {
            return float(1024 + ((x * 173 + y * 317 + (x * y % 23) * 71) % 12000)) - black[2 * (y & 1) + (x & 1)];
        };
        std::vector<float> out(w * h / 4);
        rawrcam::renderer::NaturalBayerTile tile;
        tile.run(0, 0, w / 2, h / 2, w, h, {1, .5, .5, .75}, 0, .0625f, sample, [] {}, out.data(), w / 2);
        std::ifstream file(std::filesystem::path(__FILE__).parent_path() / "../fixtures/renderer_natural_reference.u16",
                           std::ios::binary);
        if (!file) return 10;
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w / 2; ++x) {
                unsigned char bytes[2];
                file.read(reinterpret_cast<char*>(bytes), 2);
                const int ref = bytes[0] + 256 * bytes[1];
                if (!file || std::abs(out[y * w / 2 + x] + black[2 * (y & 1) + (x & 1)] - ref) > .6f) return 11;
            }
    }
    for (int red = 0; red < 4; ++red) {
        constexpr int w = 320, h = 96;
        std::array<float, 4> gains{1, .5f, .75f, .6f};
        auto sample = [&](int x, int y) {
            return (20.f + 3.f * std::sin(x * .17f) + 2.f * std::cos(y * .11f)) / gains[2 * (y & 1) + (x & 1)];
        };
        rawrcam::renderer::NaturalBayerTile tile;
        std::vector<float> full(w * h / 4), tiled(full.size());
        tile.run(0, 0, w / 2, h / 2, w, h, gains, red, 1e-8f, sample, [] {}, full.data(), w / 2);
        for (int y = 0; y < h / 2; y += 32)
            for (int x = 0; x < w / 2; x += 128)
                tile.run(
                    x, y, std::min(128, w / 2 - x), std::min(32, h / 2 - y), w, h, gains, red, 1e-8f, sample, [] {},
                    tiled.data() + y * w / 2 + x, w / 2);
        for (size_t i = 0; i < full.size(); ++i)
            if (std::abs(full[i] - tiled[i]) > 1e-5f) return 12;
        auto flat = [&](int x, int y) { return float(2 * (y & 1) + (x & 1) + 1); };
        tile.run(0, 0, w / 2, h / 2, w, h, gains, red, 1e-8f, flat, [] {}, full.data(), w / 2);
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w / 2; ++x)
                if (std::abs(full[y * w / 2 + x] - flat(x, y)) > 1e-5f) return 13;
        int checks = 0;
        bool stopped = false;
        try {
            tile.run(
                0, 0, w / 2, h / 2, w, h, gains, red, 1e-8f, sample,
                [&] {
                    if (++checks == 3) throw std::runtime_error("cancel");
                },
                full.data(), w / 2);
        } catch (const std::runtime_error&) {
            stopped = true;
        }
        if (!stopped) return 14;
    }
    puts("RENDERER_BAYER_BIN_PASS (Natural reference, phases, seams, cancellation)");
}
