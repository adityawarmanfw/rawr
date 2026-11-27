#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace rawrcam::imaging {

// Phase-detect disparity of a quad sensor with one on-chip lens per 2x2 block
// (vivo X200 Ultra main). The four pixels under a lens see different halves of
// the aperture: in focus they agree, out of focus each sees the image shifted
// along its side (the PDAF signal). On a blurred edge one half of every block
// then reads brighter than the other, by far more than the half-pixel offset
// explains (X200 Ultra clock edge: the top pair 7-14 DN above the bottom pair
// where a sharp image puts it 2 DN below, about a 6 px shift). A remosaic
// keeps two of the four as Bayer samples and rebuilds the other two, so the
// split becomes a blocky 4-px band along every soft edge.
//
// Per block of samples s0..s3 (s0 top left, s3 bottom right), mean m, same-
// colour gradient g measured between the blocks two away (zero at the frame
// edge), sample offsets o = (+-1, +-1):
//  1. The split beyond the in-focus half pixel: rx = (s1 + s3) - (s0 + s2) - 2 gx
//     (likewise ry). A local shift s gives rx = 4 s gx.
//  2. s is regressed on the gradient, separately per axis, over 3 x 3 cells of
//     4 x 4 blocks around each cell: sx = sum(w rx gx) / (sum(w 4 gx^2) +
//     N max(m - black, 1) / 64), |s| <= 4 (N the window's frame blocks, m the
//     block's own mean), then interpolated bilinearly between the four cells
//     around the block (cell centres at block 4 c + 1.5): smooth, and cheap on
//     the GPU (the per-block terms never leave the gate pass).
//     w only admits blocks where the image is smooth at block scale: fine
//     texture aliases on the 4-px same-colour lattice into a gradient that
//     correlates with the split exactly like a shift does, so blocks whose
//     green plane (green blocks, the others averaged from their four green
//     neighbours: a 2-px quincunx that does not alias the same way) is rough
//     next to its gradient, or whose diagonal split (s0 + s3 - s1 - s2: no
//     first-order shift signal) is large, get no say. Their energies, 3 x 3
//     block means, are taken net of the frame's noise (QuadGainField
//     noiseSlope/noiseFloor, from flat blocks): rho = G2 / (16 L2 + D2 + G2 noise / 4),
//     w = clamp((rho - 2) / 6, 0, 1); zero within three blocks of the edge.
//     Texture and flat noise therefore give s = 0.
//  3. Where the shift predicts a split well above the noise, the block's own
//     measured split goes: cx = c rx / 4, c = p^2 / (p^2 + 0.02 var), p = 4 sx gx
//     (the measured split, not s gx: the gradient between blocks 8 px apart
//     underestimates a narrow edge's slope). sample_k -= cx ox_k + cy oy_k,
//     clamped to the block's original range. The block mean is kept exactly.
//
// Mirrored expression for expression by native/raw_ingress/shaders/raw_disparity.comp
// (passes 15 and 16).
struct QuadDisparity {
    static constexpr int kCell = 4;              // regression terms summed per 4 x 4 blocks
    static constexpr int kRadius = 6;            // about the window half-size in blocks (3 x 3 cells, interpolated)
    static constexpr int kGateMargin = 3;        // blocks next to the edge with no gate weight
    static constexpr float kMaxShift = 4.0f;     // |s| clamp
    static constexpr float kRegScale = 1.0f / 64.0f;
    // Expected noise in the gate energies per unit pixel variance (white noise through these operators).
    static constexpr float kNoiseG2 = 0.149f, kNoiseL2 = 0.081f, kNoiseD2 = 4.0f;
    // The diagonal split counts a tenth in the gate's denominator: a strongly defocused highlight has a large,
    // genuinely shift-driven diagonal term (multiplicative, not first order), and at full weight it closed the
    // gate exactly where the shift is biggest (coloured patches with a 4-px lattice in a bokeh halo).
    static constexpr float kDiagonalWeight = 0.1f;
    static constexpr float kRhoLow = 2.0f, kRhoHigh = 8.0f;
    // The gradient energy must also clear a quarter of its own noise level: flat noise has every energy
    // near zero after the noise is taken off, and a tiny leftover gradient would otherwise pass.
    static constexpr float kGateNoise = 0.25f;
    static constexpr float kConfidenceNoise = 0.02f;  // c = p^2 / (p^2 + 0.02 var): near-full removal where a shift is found
};

// Green blocks of a quad mosaic with Bayer order `order` (0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR).
inline bool quadGreenBlock(uint32_t order, int bx, int by) {
    const bool diagonal = ((bx ^ by) & 1) == 0;
    return order == 1u || order == 2u ? diagonal : !diagonal;
}

// In place on a RAW16 quad frame (width, height even). `black` is the raw black level; the noise model
// gives a pixel's variance as noiseSlope * max(level - black, 0) + noiseFloor (0, 0: no noise allowance).
inline void correctQuadDisparity(uint16_t* raw, uint32_t width, uint32_t height, uint32_t order, float black,
                                 float noiseSlope, float noiseFloor, uint32_t threads = 1) {
    using D = QuadDisparity;
    if (!raw || width < 16 || height < 16 || width % 2 || height % 2) return;
    constexpr int C = D::kCell, M = D::kGateMargin;
    const int bw = int(width / 2), bh = int(height / 2);
    const int cw = (bw + C - 1) / C, ch = (bh + C - 1) / C;
    const std::vector<uint16_t> src(raw, raw + size_t(width) * height);
    const auto at = [&](int x, int y) { return float(src[size_t(y) * width + size_t(x)]); };
    const auto variance = [&](float m) { return noiseSlope * std::max(m - black, 0.0f) + noiseFloor; };
    constexpr int kBand = 32;
    const int bands = (bh + kBand - 1) / kBand;
    const auto work = [&](int band) {
        const int b0 = band * kBand, b1 = std::min(bh, b0 + kBand);
        // Cell rows the band's shifts read (its cells +- 1 for the bilinear, +- 1 for the window), their block
        // rows, and the block rows those need (gate reach 3).
        const int cr0 = std::max(0, b0 / C - 2), cr1 = std::min(ch, (b1 - 1) / C + 3);
        const int t0 = cr0 * C, t1 = std::min(bh, cr1 * C);
        const int r0 = std::max(0, t0 - M), r1 = std::min(bh, t1 + M);
        const int rows = r1 - r0;
        std::vector<float> mean(size_t(rows) * bw), dx(size_t(rows) * bw), dy(size_t(rows) * bw),
            dxy(size_t(rows) * bw), green(size_t(rows) * bw, 0.0f);
        const auto idx = [&](int bx, int by) { return size_t(by - r0) * bw + size_t(bx); };
        for (int by = r0; by < r1; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const int x = 2 * bx, y = 2 * by;
                const float s0 = at(x, y), s1 = at(x + 1, y), s2 = at(x, y + 1), s3 = at(x + 1, y + 1);
                const size_t i = idx(bx, by);
                mean[i] = 0.25f * ((s0 + s1) + (s2 + s3));
                dx[i] = (s1 + s3) - (s0 + s2);
                dy[i] = (s2 + s3) - (s0 + s1);
                dxy[i] = (s0 + s3) - (s1 + s2);
            }
        // Green plane where the gate reads it (blocks with all four neighbours inside the frame).
        for (int by = std::max(r0 + 1, 1); by < std::min(r1 - 1, bh - 1); ++by)
            for (int bx = 1; bx < bw - 1; ++bx)
                green[idx(bx, by)] = quadGreenBlock(order, bx, by)
                                         ? mean[idx(bx, by)]
                                         : 0.25f * ((mean[idx(bx + 1, by)] + mean[idx(bx - 1, by)]) +
                                                    (mean[idx(bx, by + 1)] + mean[idx(bx, by - 1)]));
        const auto gradX = [&](int bx, int by) {
            return bx - 2 >= 0 && bx + 2 < bw ? (mean[idx(bx + 2, by)] - mean[idx(bx - 2, by)]) * 0.125f : 0.0f;
        };
        const auto gradY = [&](int bx, int by) {
            return by - 2 >= 0 && by + 2 < bh ? (mean[idx(bx, by + 2)] - mean[idx(bx, by - 2)]) * 0.125f : 0.0f;
        };
        const auto gate = [&](int bx, int by) {
            if (bx < M || by < M || bx >= bw - M || by >= bh - M) return 0.0f;
            float g2 = 0.0f, l2 = 0.0f, d2 = 0.0f;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int cx = bx + i, cy = by + j;
                    const float e = green[idx(cx + 1, cy)], wv = green[idx(cx - 1, cy)];
                    const float s = green[idx(cx, cy + 1)], n = green[idx(cx, cy - 1)];
                    const float lap = green[idx(cx, cy)] - 0.25f * ((e + wv) + (s + n));
                    const float hx = 0.5f * (e - wv), hy = 0.5f * (s - n);
                    g2 += hx * hx + hy * hy;
                    l2 += lap * lap;
                    const float q = dxy[idx(cx, cy)];
                    d2 += q * q;
                }
            const float var = variance(mean[idx(bx, by)]);
            const float g2s = std::max(g2 * (1.0f / 9.0f) - D::kNoiseG2 * var, 0.0f);
            const float l2s = std::max(l2 * (1.0f / 9.0f) - D::kNoiseL2 * var, 0.0f);
            const float d2s = std::max(d2 * (1.0f / 9.0f) - D::kNoiseD2 * var, 0.0f);
            const float rho = g2s / (16.0f * l2s + D::kDiagonalWeight * d2s + D::kGateNoise * D::kNoiseG2 * var + 1e-6f);
            return std::clamp((rho - D::kRhoLow) / (D::kRhoHigh - D::kRhoLow), 0.0f, 1.0f);
        };
        // Regression terms per cell (sums over its blocks in raster order) for cell rows [cr0, cr1).
        struct Terms {
            float px, qx, py, qy;
        };
        std::vector<Terms> cells(size_t(cr1 - cr0) * cw, Terms{0.0f, 0.0f, 0.0f, 0.0f});
        for (int cy = cr0; cy < cr1; ++cy)
            for (int cx = 0; cx < cw; ++cx) {
                Terms acc{0.0f, 0.0f, 0.0f, 0.0f};
                for (int by = cy * C; by < std::min(bh, cy * C + C); ++by)
                    for (int bx = cx * C; bx < std::min(bw, cx * C + C); ++bx) {
                        const float w = gate(bx, by), gx = gradX(bx, by), gy = gradY(bx, by);
                        const float rx = dx[idx(bx, by)] - 2.0f * gx, ry = dy[idx(bx, by)] - 2.0f * gy;
                        acc.px += w * (rx * gx);
                        acc.qx += w * (4.0f * gx * gx);
                        acc.py += w * (ry * gy);
                        acc.qy += w * (4.0f * gy * gy);
                    }
                cells[size_t(cy - cr0) * cw + cx] = acc;
            }
        // 3 x 3 cell window sums and the number of frame blocks they cover.
        const auto window = [&](int cx, int cy, int& n) {
            Terms acc{0.0f, 0.0f, 0.0f, 0.0f};
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int qx = cx + i, qy = cy + j;
                    if (qx < 0 || qy < 0 || qx >= cw || qy >= ch) continue;
                    const Terms& t = cells[size_t(qy - cr0) * cw + qx];
                    acc.px += t.px;
                    acc.qx += t.qx;
                    acc.py += t.py;
                    acc.qy += t.qy;
                }
            n = (std::min(C * (cx + 2), bw) - std::max(C * (cx - 1), 0)) *
                (std::min(C * (cy + 2), bh) - std::max(C * (cy - 1), 0));
            return acc;
        };
        for (int by = b0; by < b1; ++by) {
            const float fy = (float(by) + 0.5f) * (1.0f / C) - 0.5f;
            const int cy0 = std::clamp(int(std::floor(fy)), 0, ch - 1), cy1 = std::min(cy0 + 1, ch - 1);
            const float ty = std::clamp(fy - float(cy0), 0.0f, 1.0f);
            for (int bx = 0; bx < bw; ++bx) {
                const float fx = (float(bx) + 0.5f) * (1.0f / C) - 0.5f;
                const int cx0 = std::clamp(int(std::floor(fx)), 0, cw - 1), cx1 = std::min(cx0 + 1, cw - 1);
                const float tx = std::clamp(fx - float(cx0), 0.0f, 1.0f);
                const float m = mean[idx(bx, by)];
                const float level = std::max(m - black, 1.0f) * D::kRegScale;
                float sxc[4], syc[4];
                const int cxs[4] = {cx0, cx1, cx0, cx1}, cys[4] = {cy0, cy0, cy1, cy1};
                for (int k = 0; k < 4; ++k) {
                    int n = 0;
                    const Terms t = window(cxs[k], cys[k], n);
                    const float reg = float(n) * level;
                    sxc[k] = std::clamp(t.px / (t.qx + reg), -D::kMaxShift, D::kMaxShift);
                    syc[k] = std::clamp(t.py / (t.qy + reg), -D::kMaxShift, D::kMaxShift);
                }
                const float topX = sxc[0] + (sxc[1] - sxc[0]) * tx, bottomX = sxc[2] + (sxc[3] - sxc[2]) * tx;
                const float topY = syc[0] + (syc[1] - syc[0]) * tx, bottomY = syc[2] + (syc[3] - syc[2]) * tx;
                const float sx = topX + (bottomX - topX) * ty, sy = topY + (bottomY - topY) * ty;
                const float gx = gradX(bx, by), gy = gradY(bx, by);
                const float rx = dx[idx(bx, by)] - 2.0f * gx, ry = dy[idx(bx, by)] - 2.0f * gy;
                const float var = variance(m);
                const float px = 4.0f * sx * gx, py = 4.0f * sy * gy;
                const float cwx = px * px / (px * px + D::kConfidenceNoise * var + 1e-6f);
                const float cwy = py * py / (py * py + D::kConfidenceNoise * var + 1e-6f);
                const float cx = cwx * rx * 0.25f, cy = cwy * ry * 0.25f;
                const int x = 2 * bx, y = 2 * by;
                const float s[4] = {at(x, y), at(x + 1, y), at(x, y + 1), at(x + 1, y + 1)};
                // The diagonal split goes with the shift: where one was found it is a defocus artefact (second order
                // in the shift, so the per-axis terms above never see it).
                const float cd = std::max(cwx, cwy) * ((s[0] + s[3]) - (s[1] + s[2])) * 0.25f;
                const float lo = std::min(std::min(s[0], s[1]), std::min(s[2], s[3]));
                const float hi = std::max(std::max(s[0], s[1]), std::max(s[2], s[3]));
                for (int k = 0; k < 4; ++k) {
                    const float ox = (k & 1) ? 1.0f : -1.0f, oy = (k & 2) ? 1.0f : -1.0f;
                    const float v = std::clamp(s[k] - (cx * ox + cy * oy + cd * ox * oy), lo, hi);
                    raw[size_t(y + (k >> 1)) * width + size_t(x + (k & 1))] =
                        uint16_t(std::clamp(std::floor(v + 0.5f), 0.0f, 65535.0f));
                }
            }
        }
    };
    threads = std::clamp(threads, 1u, 16u);
    std::vector<std::thread> pool;
    for (uint32_t t = 1; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (int b = int(t); b < bands; b += int(threads)) work(b);
        });
    for (int b = 0; b < bands; b += int(threads)) work(b);
    for (auto& p : pool) p.join();
}

}  // namespace rawrcam::imaging
