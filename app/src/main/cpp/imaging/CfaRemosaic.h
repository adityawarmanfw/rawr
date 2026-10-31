#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
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

// Colour difference at a pixel from the four nearest blocks of the wanted
// colour: bilinear weights, each scaled by how close the block's mean green
// is to the pixel's green. Without that, a block across a strong edge (a
// bright leaf next to a dark wall) pushes R/B on the dark side to black: a
// dark green fringe. `tap(bx, by)` returns {diff, block green}.
template <typename Tap>
inline float cfaBlendDiff(const CfaBlockTap& t, float green, Tap&& tap) {
    const int32_t bxs[2] = {t.bx0, t.bx1}, bys[2] = {t.by0, t.by1};
    const float wx[2] = {1.0f - t.tx, t.tx}, wy[2] = {1.0f - t.ty, t.ty};
    float sum = 0.0f, weight = 0.0f;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const auto [d, g] = tap(bxs[i], bys[j]);
            // Similar within ~10% of the brighter green (plus a few DN of noise).
            const float sigma = 0.1f * std::max(green, g) + 4.0f;
            const float r = (g - green) / sigma;
            const float wgt = (wx[i] * wy[j] + 1e-4f) / (1.0f + r * r);
            sum += wgt * d;
            weight += wgt;
        }
    return sum / weight;
}

// Fast (live): no green interpolation. A pixel keeps its own sample where
// its block has the wanted colour; elsewhere the wanted colour is the
// block means of that colour interpolated linearly (binned colour, sharp
// luminance only where the pixel's own colour carries it). One mean pass.
// Standard (live, GPU and CPU alike): the passes above.
// High (stills, CPU): R/B colour difference from every 2x2 sub-block of the
// wanted colour within 2n pixels (the nearest blocks in all directions, a
// block straddling an edge no longer averaged into one value), weighted by
// distance and green similarity.
enum class CfaRemosaicQuality : uint32_t { Fast = 0, Standard = 1, High = 2 };

// Fast at (x, y) from block means. Mirrored by the shader's fast pass.
inline float cfaFastAt(const uint16_t* raw, const float* means, uint32_t w, uint32_t bw, uint32_t bh, uint32_t n,
                       uint32_t order, const uint32_t parity[3][2], uint32_t x, uint32_t y) {
    const uint32_t want = cfaChannel(order, x, y), bx = x / n, by = y / n;
    if (cfaChannel(order, bx, by) == want) return float(raw[size_t(y) * w + x]);
    const auto mean = [&](int32_t bx2, int32_t by2) { return means[size_t(by2) * bw + size_t(bx2)]; };
    if (want == 1u) {
        // The four edge neighbours are green; mirror at the frame edge.
        const int32_t bl = bx > 0 ? int32_t(bx) - 1 : int32_t(bx) + 1, br = bx + 1 < bw ? int32_t(bx) + 1 : bl;
        const int32_t bt = by > 0 ? int32_t(by) - 1 : int32_t(by) + 1, bb = by + 1 < bh ? int32_t(by) + 1 : bt;
        // Pixel within its block (0..1), neighbour centres at -0.5 and 1.5.
        const float tx = ((float(x % n) + 0.5f) / float(n) + 0.5f) * 0.5f;
        const float ty = ((float(y % n) + 0.5f) / float(n) + 0.5f) * 0.5f;
        const float gh = mean(bl, int32_t(by)) + (mean(br, int32_t(by)) - mean(bl, int32_t(by))) * tx;
        const float gv = mean(int32_t(bx), bt) + (mean(int32_t(bx), bb) - mean(int32_t(bx), bt)) * ty;
        return 0.5f * (gh + gv);
    }
    const auto t = cfaBlockTap(n, bw, bh, parity[want][0], parity[want][1], x, y);
    const float top = mean(t.bx0, t.by0) + (mean(t.bx1, t.by0) - mean(t.bx0, t.by0)) * t.tx;
    const float bottom = mean(t.bx0, t.by1) + (mean(t.bx1, t.by1) - mean(t.bx0, t.by1)) * t.tx;
    return top + (bottom - top) * t.ty;
}

// Remosaics `raw` (w x h packed RAW16) in place. n = block side (2 or 4);
// w and h are multiples of 2n on every sensor this serves (others still work,
// edge blocks are partial). Single-threaded unless `threads` > 1.
inline void remosaicBlockCfa(uint16_t* raw, uint32_t w, uint32_t h, uint32_t n, uint32_t order, uint32_t threads = 1,
                             CfaRemosaicQuality quality = CfaRemosaicQuality::Standard) {
    if (n < 2 || !raw || w < 2 * n || h < 2 * n) return;
    const uint32_t bw = (w + n - 1) / n, bh = (h + n - 1) / n;
    // Block parity of R and B.
    uint32_t parity[3][2] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t c = cfaChannel(order, i & 1u, i >> 1);
        parity[c][0] = i >> 1;
        parity[c][1] = i & 1u;
    }
    if (quality == CfaRemosaicQuality::Fast) {
        std::vector<float> means(size_t(bw) * bh);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                float sum = 0.0f;
                uint32_t count = 0;
                for (uint32_t y = by * n; y < std::min(h, by * n + n); ++y)
                    for (uint32_t x = bx * n; x < std::min(w, bx * n + n); ++x, ++count) sum += float(raw[size_t(y) * w + x]);
                means[size_t(by) * bw + bx] = sum / float(count);
            }
        // Own-colour sites stay, so writing in place is safe.
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                raw[size_t(y) * w + x] = uint16_t(std::clamp(
                    std::lround(cfaFastAt(raw, means.data(), w, bw, bh, n, order, parity, x, y)), 0l, 65535l));
        return;
    }
    // Whole-DN codes (the GPU keeps pass A as 16-bit too), so half the RAM of
    // a float plane at 50 MP: this is the largest allocation of the remosaic.
    std::vector<uint16_t> green(size_t(w) * h);
    std::vector<float> diff(size_t(bw) * bh, 0.0f), blockGreen(size_t(bw) * bh, 0.0f);
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
                    uint16_t(std::clamp(std::lround(CfaRemosaicGreen::at(raw, w, h, n, order, x, y)), 0l, 65535l));
    });
    parallelRows(bh, [&](uint32_t b0, uint32_t b1) {
        for (uint32_t by = b0; by < b1; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                if (cfaChannel(order, bx, by) == 1u) continue;
                float sum = 0.0f, greenSum = 0.0f;
                uint32_t count = 0;
                for (uint32_t y = by * n; y < std::min(h, by * n + n); ++y)
                    for (uint32_t x = bx * n; x < std::min(w, bx * n + n); ++x, ++count) {
                        sum += float(raw[size_t(y) * w + x]) - float(green[size_t(y) * w + x]);
                        greenSum += float(green[size_t(y) * w + x]);
                    }
                diff[size_t(by) * bw + bx] = count ? sum / float(count) : 0.0f;
                blockGreen[size_t(by) * bw + bx] = count ? greenSum / float(count) : 0.0f;
            }
    });
    // High: colour difference and green per 2x2 sub-block (= the block for quad).
    const uint32_t sw = (w + 1) / 2, sh = (h + 1) / 2;
    std::vector<float> subDiff, subGreen;
    if (quality == CfaRemosaicQuality::High) {
        subDiff.assign(size_t(sw) * sh, 0.0f);
        subGreen.assign(size_t(sw) * sh, 0.0f);
        parallelRows(sh, [&](uint32_t s0, uint32_t s1) {
            for (uint32_t sy = s0; sy < s1; ++sy)
                for (uint32_t sx = 0; sx < sw; ++sx) {
                    float d = 0.0f, g = 0.0f;
                    uint32_t count = 0;
                    for (uint32_t y = sy * 2; y < std::min(h, sy * 2 + 2); ++y)
                        for (uint32_t x = sx * 2; x < std::min(w, sx * 2 + 2); ++x, ++count) {
                            d += float(raw[size_t(y) * w + x]) - float(green[size_t(y) * w + x]);
                            g += float(green[size_t(y) * w + x]);
                        }
                    subDiff[size_t(sy) * sw + sx] = d / float(count);
                    subGreen[size_t(sy) * sw + sx] = g / float(count);
                }
        });
    }
    const int32_t radius = int32_t(n);  // in sub-blocks: 2n pixels
    const float spatial = 1.0f / (float(n) * float(n));
    // The taps repeat: sub-block colours have period n (in sub-blocks), so
    // the wanted-colour offsets and their distance weights depend only on the
    // pixel's sub-block phase, its position inside the sub-block and the colour.
    struct HighTap {
        int32_t ox, oy;
        float spatialWeight;
    };
    std::vector<HighTap> highTaps[4][4][4][2];  // [cy % n][cx % n][(y & 1) * 2 + (x & 1)][want == 2]
    if (quality == CfaRemosaicQuality::High) {
        for (uint32_t phy = 0; phy < n; ++phy)
            for (uint32_t phx = 0; phx < n; ++phx)
                for (uint32_t sub = 0; sub < 4; ++sub)
                    for (uint32_t blue = 0; blue < 2; ++blue) {
                        const uint32_t want = blue ? 2u : 0u;
                        // Pixel position in sub-block units relative to its sub-block, centres at +0.5.
                        const float px = float(sub & 1u) * 0.5f + 0.25f, py = float(sub >> 1) * 0.5f + 0.25f;
                        auto& taps = highTaps[phy][phx][sub][blue];
                        for (int32_t oy = -radius; oy <= radius; ++oy)
                            for (int32_t ox = -radius; ox <= radius; ++ox) {
                                // Colour of the sub-block at phase + offset (period n in both axes).
                                const uint32_t sx = uint32_t(int32_t(phx) + ox + 4 * int32_t(n)) % n;
                                const uint32_t sy = uint32_t(int32_t(phy) + oy + 4 * int32_t(n)) % n;
                                if (cfaChannel(order, sx * 2 / n, sy * 2 / n) != want) continue;
                                const float dx = float(ox) + 0.5f - px, dy = float(oy) + 0.5f - py;
                                taps.push_back({ox, oy, std::exp(-(dx * dx + dy * dy) * spatial)});
                            }
                    }
    }
    const auto highDiff = [&](uint32_t want, uint32_t x, uint32_t y, float g0) {
        const int32_t cx = int32_t(x / 2), cy = int32_t(y / 2);
        const auto& taps = highTaps[uint32_t(cy) % n][uint32_t(cx) % n][(y & 1u) * 2u + (x & 1u)][want == 2u];
        float sum = 0.0f, weight = 0.0f;
        for (const auto& t : taps) {
            const int32_t sx = cx + t.ox, sy = cy + t.oy;
            if (sx < 0 || sy < 0 || sx >= int32_t(sw) || sy >= int32_t(sh)) continue;
            const size_t b = size_t(sy) * sw + size_t(sx);
            const float sigma = 0.1f * std::max(g0, subGreen[b]) + 4.0f;
            const float r = (subGreen[b] - g0) / sigma;
            const float wgt = t.spatialWeight / (1.0f + r * r * r * r);
            sum += wgt * subDiff[b];
            weight += wgt;
        }
        return weight > 0.0f ? sum / weight : 0.0f;
    };
    parallelRows(h, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const uint32_t want = cfaChannel(order, x, y);
                float v;
                const float greenHere = float(green[i]);
                if (want == 1u) {
                    v = greenHere;
                } else if (cfaChannel(order, x / n, y / n) == want) {
                    continue;  // own site: raw sample stays
                } else {
                    if (quality == CfaRemosaicQuality::High) {
                        v = greenHere + highDiff(want, x, y, greenHere);
                    } else {
                        const auto t = cfaBlockTap(n, bw, bh, parity[want][0], parity[want][1], x, y);
                        v = greenHere + cfaBlendDiff(t, greenHere, [&](int32_t bx, int32_t by) {
                                const size_t b = size_t(by) * bw + size_t(bx);
                                return std::pair{diff[b], blockGreen[b]};
                            });
                    }
                }
                raw[i] = uint16_t(std::clamp(std::lround(v), 0l, 65535l));
            }
    });
}

// Packed RAW16 bytes (little-endian, as the snapshots hold them).
inline void remosaicBlockCfa(std::vector<uint8_t>& raw16, uint32_t w, uint32_t h, uint32_t n, uint32_t order,
                             uint32_t threads = 1, CfaRemosaicQuality quality = CfaRemosaicQuality::Standard) {
    if (raw16.size() < size_t(w) * h * 2) return;
    remosaicBlockCfa(reinterpret_cast<uint16_t*>(raw16.data()), w, h, n, order, threads, quality);
}

}  // namespace rawrcam::imaging
