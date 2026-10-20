#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace rawrcam::imaging {

// Remosaic of a block CFA (quad: 2x2 same-colour blocks, or 4x4) to Bayer at
// full resolution, in the same block-level colour order (CfaPattern code,
// 0=RGGB 1=GRBG 2=GBRG 3=BGGR). Some sensor modes (in-sensor crops) hand the
// app the block mosaic while reporting Bayer.
//
// Three passes, mirrored by native/raw_ingress/shaders/raw_remosaic_*.comp
// (keep both in step; the Vulkan host test compares them):
//  A. green everywhere: at R/B pixels, the two nearest green samples along
//     each axis (the neighbouring blocks) interpolated linearly, the axes
//     blended by inverse squared gradient; rounded to whole DN (the GPU
//     keeps it as 16-bit);
//  B. per R/B block, the mean of raw - green;
//  C. Bayer out: green from A; R/B at their own sites the raw sample, at
//     foreign sites green plus the colour difference interpolated bilinearly
//     between the nearest blocks of that colour.
// Colour differences cancel the black level, so raw values go in unchanged.

// Colour at block level: 0 R, 1 G, 2 B.
inline uint32_t cfaChannel(uint32_t order, uint32_t x, uint32_t y) {
    static constexpr uint8_t kChannels[4][4] = {{0, 1, 1, 2}, {1, 0, 2, 1}, {1, 2, 0, 1}, {2, 1, 1, 0}};
    return kChannels[order & 3u][(y & 1u) * 2u + (x & 1u)];
}

struct CfaRemosaicGreen {
    // Pass A at (x, y): raw sample when green, else the directional estimate.
    static float at(const uint16_t* raw, uint32_t w, uint32_t h, uint32_t n, uint32_t order, uint32_t x, uint32_t y) {
        const auto px = [&](int32_t xx, int32_t yy) {
            xx = std::clamp<int32_t>(xx, 0, int32_t(w) - 1);
            yy = std::clamp<int32_t>(yy, 0, int32_t(h) - 1);
            return float(raw[size_t(yy) * w + size_t(xx)]);
        };
        const uint32_t c = cfaChannel(order, x / n, y / n);
        if (c == 1u) return px(int32_t(x), int32_t(y));
        const int32_t N = int32_t(n), X = int32_t(x), Y = int32_t(y);
        const int32_t x0 = X / N * N, y0 = Y / N * N;
        // Nearest greens: the last column/row of the previous block, the first of the next.
        // At the frame edge only one side exists; use it for both.
        const int32_t xl = x0 - 1 >= 0 ? x0 - 1 : x0 + N, xr = x0 + N < int32_t(w) ? x0 + N : xl;
        const int32_t yt = y0 - 1 >= 0 ? y0 - 1 : y0 + N, yb = y0 + N < int32_t(h) ? y0 + N : yt;
        const float gl = px(xl, Y), gr = px(xr, Y), gt = px(X, yt), gb = px(X, yb);
        const float th = xr != xl ? float(X - xl) / float(xr - xl) : 0.5f;
        const float tv = yb != yt ? float(Y - yt) / float(yb - yt) : 0.5f;
        const float gh = gl + (gr - gl) * th, gv = gt + (gb - gt) * tv;
        // Gradients: across the greens, across this block, across the same-colour blocks beyond the greens.
        const float ah = std::abs(gl - gr) + std::abs(px(x0, Y) - px(x0 + N - 1, Y)) +
                         0.5f * std::abs(px(x0 - 2 * N + N - 1, Y) - px(x0 + 2 * N, Y));
        const float av = std::abs(gt - gb) + std::abs(px(X, y0) - px(X, y0 + N - 1)) +
                         0.5f * std::abs(px(X, y0 - 2 * N + N - 1) - px(X, y0 + 2 * N));
        constexpr float kEps = 1.0f;  // DN; keeps flat areas an even blend
        const float wh = 1.0f / ((ah + kEps) * (ah + kEps)), wv = 1.0f / ((av + kEps) * (av + kEps));
        return (wh * gh + wv * gv) / (wh + wv);
    }
};

// Block-centre bilinear weights of colour `c`'s lattice at pixel (x, y): the
// four block indices (clamped) and weights. Blocks of one colour repeat every
// 2 blocks; (py, px) is that colour's block parity.
struct CfaBlockTap {
    int32_t bx0, by0, bx1, by1;
    float tx, ty;
};
inline CfaBlockTap cfaBlockTap(uint32_t n, uint32_t bw, uint32_t bh, uint32_t py, uint32_t px, uint32_t x, uint32_t y) {
    const float half = float(n - 1) * 0.5f;
    const float fx = (float(x) - float(px * n) - half) / float(2 * n);
    const float fy = (float(y) - float(py * n) - half) / float(2 * n);
    const int32_t kxMax = int32_t((bw - 1 - px) / 2), kyMax = int32_t((bh - 1 - py) / 2);
    const int32_t kx = std::clamp(int32_t(std::floor(fx)), 0, kxMax), ky = std::clamp(int32_t(std::floor(fy)), 0, kyMax);
    const int32_t kx1 = std::min(kx + 1, kxMax), ky1 = std::min(ky + 1, kyMax);
    CfaBlockTap t;
    t.bx0 = kx * 2 + int32_t(px);
    t.bx1 = kx1 * 2 + int32_t(px);
    t.by0 = ky * 2 + int32_t(py);
    t.by1 = ky1 * 2 + int32_t(py);
    t.tx = std::clamp(fx - float(kx), 0.0f, 1.0f);
    t.ty = std::clamp(fy - float(ky), 0.0f, 1.0f);
    return t;
}

// Remosaics `raw` (w x h packed RAW16) in place. n = block side (2 or 4);
// w and h are multiples of 2n on every sensor this serves (others still work,
// edge blocks are partial). Single-threaded unless `threads` > 1.
inline void remosaicBlockCfa(uint16_t* raw, uint32_t w, uint32_t h, uint32_t n, uint32_t order, uint32_t threads = 1) {
    if (n < 2 || !raw || w < 2 * n || h < 2 * n) return;
    const uint32_t bw = (w + n - 1) / n, bh = (h + n - 1) / n;
    std::vector<float> green(size_t(w) * h);
    std::vector<float> diff(size_t(bw) * bh, 0.0f);
    const auto parallelRows = [&](uint32_t rows, auto&& body) {
        threads = std::max(1u, std::min(threads, rows));
        if (threads == 1) {
            body(0u, rows);
            return;
        }
        std::vector<std::thread> pool;
        for (uint32_t t = 0; t < threads; ++t)
            pool.emplace_back([&, t] { body(rows * t / threads, rows * (t + 1) / threads); });
        for (auto& th : pool) th.join();
    };
    parallelRows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = 0; x < w; ++x)
                green[size_t(y) * w + x] =
                    float(std::clamp(std::lround(CfaRemosaicGreen::at(raw, w, h, n, order, x, y)), 0l, 65535l));
    });
    parallelRows(bh, [&](uint32_t b0, uint32_t b1) {
        for (uint32_t by = b0; by < b1; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                if (cfaChannel(order, bx, by) == 1u) continue;
                float sum = 0.0f;
                uint32_t count = 0;
                for (uint32_t y = by * n; y < std::min(h, by * n + n); ++y)
                    for (uint32_t x = bx * n; x < std::min(w, bx * n + n); ++x, ++count)
                        sum += float(raw[size_t(y) * w + x]) - green[size_t(y) * w + x];
                diff[size_t(by) * bw + bx] = count ? sum / float(count) : 0.0f;
            }
    });
    // Block parity of R and B.
    uint32_t parity[3][2] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t c = cfaChannel(order, i & 1u, i >> 1);
        parity[c][0] = i >> 1;
        parity[c][1] = i & 1u;
    }
    parallelRows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const uint32_t want = cfaChannel(order, x, y);
                float v;
                if (want == 1u) {
                    v = green[i];
                } else if (cfaChannel(order, x / n, y / n) == want) {
                    continue;  // own site: raw sample stays
                } else {
                    const auto t = cfaBlockTap(n, bw, bh, parity[want][0], parity[want][1], x, y);
                    const auto d = [&](int32_t bx, int32_t by) { return diff[size_t(by) * bw + size_t(bx)]; };
                    const float top = d(t.bx0, t.by0) + (d(t.bx1, t.by0) - d(t.bx0, t.by0)) * t.tx;
                    const float bottom = d(t.bx0, t.by1) + (d(t.bx1, t.by1) - d(t.bx0, t.by1)) * t.tx;
                    v = green[i] + top + (bottom - top) * t.ty;
                }
                raw[i] = uint16_t(std::clamp(std::lround(v), 0l, 65535l));
            }
    });
}

// Packed RAW16 bytes (little-endian, as the snapshots hold them).
inline void remosaicBlockCfa(std::vector<uint8_t>& raw16, uint32_t w, uint32_t h, uint32_t n, uint32_t order,
                             uint32_t threads = 1) {
    if (raw16.size() < size_t(w) * h * 2) return;
    remosaicBlockCfa(reinterpret_cast<uint16_t*>(raw16.data()), w, h, n, order, threads);
}

}  // namespace rawrcam::imaging
