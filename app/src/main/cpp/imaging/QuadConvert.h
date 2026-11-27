#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace rawrcam::imaging {

// Thin-feature stage of the quad (2x2 same-colour blocks) remosaic, run on Detail's output.
//
// A quad sensor measures the scene at every one of its pixels; only the colour is subsampled (one same-colour
// block per 4 px). Detail rebuilds the Bayer sites whose colour differs from the pixel's own by interpolating
// neighbours, which discards that pixel's measurement: thin lines at diagonal angles come out jagged, dashed and
// colour-fringed (a 2 px wire is seen at different positions by the R, G and B blocks), up to 33 DN RMS on a
// synthetic 2 px line against 0 for lines on the pixel axes.
//
// Convert keeps the pixel's own value and only changes its colour: out = v * L_wanted / L_own, where L_c is the
// smooth colour plane (Gaussian, sigma 2 cells = 8 px) of the block means. It is exact where chroma is smooth
// (neutral wires, lines on a wall) and wrong at colour edges, so it is gated per cell:
//  * colour: the fine (sigma 1.25 cells) and coarse colour-ratio maps must agree (|rf / rs - 1| < delta);
//  * orientation: lines on the pixel axes are exact in Detail and beaded by Convert, so the weight falls with
//    |sin 2 theta| of the cell-luminance structure tensor (0.2 .. 0.5);
//  * structure: only near luminance gradients clearly above the noise (flat areas keep Detail's smoother
//    foreign sites: Convert there would add 40-60% high-frequency noise);
//  * not near clipped samples, and a signal ramp (10 .. 40 DN) against noise.
// Near structure where Convert is off, R and B sites are pulled to G_hat * L_c / L_G (fine ratio) wherever the
// colour ratios are consistent (|rf / rs - 1| < delta_c): the green/cyan/yellow dots of aliased thin lines.
//
// A cell is 4 x 4 pixels (2 x 2 blocks: one R, two G, one B), so every smoothing runs on a grid 16x smaller
// than the pixels. Windows start on a multiple of 4 rows (strips: 8) and have width, height multiple of 4.
// Mirrors native/raw_ingress/shaders/raw_convert.comp; prototype and measurements in tmp/ocl (chain_cell.py).
struct QuadConvert {
    static constexpr float kSigmaCoarse = 2.0f, kSigmaFine = 1.25f, kSigmaTensor = 1.0f;
    static constexpr int kRadCoarse = 6, kRadFine = 4, kRadTensor = 3;  // int(3 sigma + 0.5)
    static constexpr float kDelta = 0.08f, kDeltaChroma = 0.15f;
    static constexpr float kOrientLow = 0.2f, kOrientHigh = 0.5f, kCoherence = 0.4f;
    static constexpr float kStructureThreshold = 3.0f;
    static constexpr float kNoiseSlope = 0.2f, kNoiseFloor = 0.25f;  // pixel variance model for the cell mean
    static constexpr float kSnrLow = 10.0f, kSnrHigh = 40.0f;        // coarse green level ramp (DN above black)
    static constexpr float kDarkRatioFloor = 3.0f;
    // Reach of one output pixel in cells (coarse blur 6 + ratio dilation 1 + bilinear 1); strips need a margin of
    // 4 * this + 2 rows on top of the stages before it.
    static constexpr uint32_t kSupportCells = 8;
};

namespace detail {
// Round to IEEE half (nearest even) and back: the GPU keeps its cell grids in rgba16f.
inline float quadConvertHalf(float v) {
    if (!(std::fabs(v) < 65504.0f)) return v;
    int e;
    const float m = std::frexp(v, &e);  // v = m 2^e, 0.5 <= |m| < 1
    if (v == 0.0f) return 0.0f;
    const int bits = e < -13 ? 11 - (-13 - e) : 11;  // subnormals lose bits below 2^-14
    if (bits <= 0) return 0.0f;
    const float scale = std::ldexp(1.0f, bits);
    return std::ldexp(std::nearbyint(m * scale) / scale, e);
}
inline void quadConvertQuantize(std::vector<float>& a) {
    for (float& v : a) v = quadConvertHalf(v);
}
inline void quadConvertKernel(float sigma, int radius, std::vector<float>& k) {
    k.assign(size_t(2 * radius + 1), 0.0f);
    double sum = 0.0;
    std::vector<double> w(k.size());
    for (int i = -radius; i <= radius; ++i) sum += (w[size_t(i + radius)] = std::exp(-0.5 * double(i) * i / (double(sigma) * sigma)));
    for (size_t i = 0; i < k.size(); ++i) k[i] = float(w[i] / sum);
}
template <class F>
inline void quadConvertParallel(uint32_t count, uint32_t threads, F&& f) {
    threads = std::clamp(threads, 1u, 16u);
    if (threads == 1u || count < threads * 4u) {
        f(0u, count);
        return;
    }
    std::vector<std::thread> pool;
    const uint32_t chunk = (count + threads - 1) / threads;
    for (uint32_t t = 1; t < threads; ++t)
        if (t * chunk < count) pool.emplace_back([&, t] { f(t * chunk, std::min(count, (t + 1) * chunk)); });
    f(0u, std::min(count, chunk));
    for (auto& p : pool) p.join();
}
// Separable Gaussian with replicated borders over a w x h float plane.
inline void quadConvertBlur(const std::vector<float>& in, std::vector<float>& out, std::vector<float>& tmp, int w, int h,
                            float sigma, int radius, uint32_t threads) {
    std::vector<float> k;
    quadConvertKernel(sigma, radius, k);
    tmp.resize(in.size());
    out.resize(in.size());
    quadConvertParallel(uint32_t(h), threads, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float s = 0.0f;
                for (int i = -radius; i <= radius; ++i) s += k[size_t(i + radius)] * in[size_t(y) * w + size_t(std::clamp(x + i, 0, w - 1))];
                tmp[size_t(y) * w + x] = s;
            }
    });
    quadConvertParallel(uint32_t(h), threads, [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float s = 0.0f;
                for (int i = -radius; i <= radius; ++i) s += k[size_t(i + radius)] * tmp[size_t(std::clamp(int(y) + i, 0, h - 1)) * w + x];
                out[size_t(y) * w + x] = s;
            }
    });
}
inline void quadConvertFilter3(const std::vector<float>& in, std::vector<float>& out, int w, int h, bool maximum, bool minimum) {
    out.resize(in.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float mx = -1e30f, mn = 1e30f, sum = 0.0f;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const float v = in[size_t(std::clamp(y + j, 0, h - 1)) * w + size_t(std::clamp(x + i, 0, w - 1))];
                    mx = std::max(mx, v);
                    mn = std::min(mn, v);
                    sum += v;
                }
            out[size_t(y) * w + x] = maximum ? mx : minimum ? mn : sum * (1.0f / 9.0f);  // neither: the mean
        }
}
}  // namespace detail

// quad: RAW16 window after the gain field and disparity correction (what Detail read); detail: Detail's Bayer
// output of the same window; both width x height; out may alias neither. order: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR
// (the quad's block order and the output's Bayer order). black: raw black level in codes.
inline void quadConvert(const uint16_t* quad, const uint16_t* detailIn, uint16_t* out, uint32_t width, uint32_t height,
                        uint32_t order, float black, uint32_t threads = 1) {
    using Q = QuadConvert;
    if (!quad || !detailIn || !out || width % 4 || height % 4 || width < 16 || height < 16 || order > 3) return;
    const int cw = int(width / 4), ch = int(height / 4);
    const size_t cells = size_t(cw) * ch;
    // Block parity ((y & 1) << 1 | (x & 1)) of the R and B blocks; green is the other two.
    static constexpr int kRy[4] = {0, 0, 1, 1}, kRx[4] = {0, 1, 0, 1}, kBy[4] = {1, 1, 0, 0}, kBx[4] = {1, 0, 1, 0};
    const int ry = kRy[order], rx = kRx[order], by = kBy[order], bx = kBx[order];
    const auto sample = [&](uint32_t x, uint32_t y) { return float(quad[size_t(y) * width + x]) - black; };
    // ---- cell statistics
    std::vector<float> R(cells), G(cells), B(cells), MX(cells);
    detail::quadConvertParallel(uint32_t(ch), threads, [&](uint32_t j0, uint32_t j1) {
        for (uint32_t j = j0; j < j1; ++j)
            for (uint32_t i = 0; i < uint32_t(cw); ++i) {
                float mean[2][2], mx = -1e30f;
                for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b) {
                        const uint32_t x = 4 * i + 2 * b, y = 4 * j + 2 * a;
                        const float s0 = sample(x, y), s1 = sample(x + 1, y), s2 = sample(x, y + 1), s3 = sample(x + 1, y + 1);
                        mean[a][b] = 0.25f * ((s0 + s1) + (s2 + s3));
                        mx = std::max(std::max(mx, std::max(s0, s1)), std::max(s2, s3));
                    }
                // The two blocks that are neither R nor B.
                float g0, g1;
                if (ry == rx) g0 = mean[0][1], g1 = mean[1][0];  // R, B on the main diagonal
                else g0 = mean[0][0], g1 = mean[1][1];
                const size_t c = size_t(j) * cw + i;
                R[c] = mean[ry][rx];
                B[c] = mean[by][bx];
                G[c] = 0.5f * (g0 + g1);
                MX[c] = mx;
            }
    });
    for (auto* v : {&R, &G, &B, &MX}) detail::quadConvertQuantize(*v);
    // ---- smooth colour planes (coarse and fine)
    std::vector<float> tmp, Ls[3], Lf[3];
    const std::vector<float>* planes[3] = {&R, &G, &B};
    for (int c = 0; c < 3; ++c) {
        detail::quadConvertBlur(*planes[c], Ls[c], tmp, cw, ch, Q::kSigmaCoarse, Q::kRadCoarse, threads);
        detail::quadConvertBlur(*planes[c], Lf[c], tmp, cw, ch, Q::kSigmaFine, Q::kRadFine, threads);
    }
    // ---- colour-ratio disagreement, luminance structure
    std::vector<float> dis(cells), Y(cells);
    for (size_t c = 0; c < cells; ++c) {
        const float eps = Q::kDarkRatioFloor;
        const float rsR = Ls[0][c] / std::max(Ls[1][c], eps), rsB = Ls[2][c] / std::max(Ls[1][c], eps);
        const float rfR = Lf[0][c] / std::max(Lf[1][c], eps), rfB = Lf[2][c] / std::max(Lf[1][c], eps);
        dis[c] = std::max(std::fabs(rfR / std::max(rsR, 1e-3f) - 1.0f), std::fabs(rfB / std::max(rsB, 1e-3f) - 1.0f));
        Y[c] = (R[c] + 2.0f * G[c] + B[c]) * 0.25f;
    }
    for (int c = 0; c < 3; ++c) detail::quadConvertQuantize(Ls[c]), detail::quadConvertQuantize(Lf[c]);
    detail::quadConvertQuantize(dis);
    std::vector<float> dis3;
    detail::quadConvertFilter3(dis, dis3, cw, ch, true, false);
    std::vector<float> gx(cells), gy(cells);
    for (int j = 0; j < ch; ++j)
        for (int i = 0; i < cw; ++i) {
            const size_t c = size_t(j) * cw + i;
            gx[c] = i == 0 ? Y[c + 1] - Y[c] : i == cw - 1 ? Y[c] - Y[c - 1] : 0.5f * (Y[c + 1] - Y[c - 1]);
            gy[c] = j == 0 ? Y[c + cw] - Y[c] : j == ch - 1 ? Y[c] - Y[c - cw] : 0.5f * (Y[c + cw] - Y[c - cw]);
        }
    // Structure tensor: the gradient products smoothed separably (sigma 1 cell), replicated borders.
    std::vector<float> gxx(cells), gyy(cells), gxy(cells), sxx, syy, sxy;
    for (size_t c = 0; c < cells; ++c) gxx[c] = gx[c] * gx[c], gyy[c] = gy[c] * gy[c], gxy[c] = gx[c] * gy[c];
    detail::quadConvertBlur(gxx, sxx, tmp, cw, ch, Q::kSigmaTensor, Q::kRadTensor, threads);
    detail::quadConvertBlur(gyy, syy, tmp, cw, ch, Q::kSigmaTensor, Q::kRadTensor, threads);
    detail::quadConvertBlur(gxy, sxy, tmp, cw, ch, Q::kSigmaTensor, Q::kRadTensor, threads);
    std::vector<float> orient(cells), structure(cells), clipMask(cells);
    for (size_t c = 0; c < cells; ++c) {
        const float den = std::sqrt((sxx[c] - syy[c]) * (sxx[c] - syy[c]) + 4.0f * sxy[c] * sxy[c]);
        const float coh = den / (sxx[c] + syy[c] + 1e-9f);
        const float s2 = std::fabs(2.0f * sxy[c]) / std::max(den, 1e-9f);
        float f = std::clamp((s2 - Q::kOrientLow) / (Q::kOrientHigh - Q::kOrientLow), 0.0f, 1.0f);
        orient[c] = coh > Q::kCoherence ? f : 1.0f;
        const float gm = std::hypot(gx[c], gy[c]) * 2.0f;
        const float sg = std::sqrt(Q::kNoiseSlope * std::max(Y[c], 0.0f) + Q::kNoiseFloor) * 0.25f;
        structure[c] = gm / std::max(sg, 0.15f) > Q::kStructureThreshold ? 1.0f : 0.0f;
    }
    // Clipped: a sample code of 1020 or more (10-bit sensors; the cell maximum is black-subtracted).
    for (size_t c = 0; c < cells; ++c) clipMask[c] = MX[c] + black >= 1020.0f ? 1.0f : 0.0f;
    std::vector<float> orient3, struct3, struct33, clip3;
    detail::quadConvertFilter3(orient, orient3, cw, ch, false, true);
    detail::quadConvertFilter3(structure, struct3, cw, ch, true, false);
    detail::quadConvertFilter3(struct3, struct33, cw, ch, false, false);
    detail::quadConvertFilter3(clipMask, clip3, cw, ch, true, false);
    // Per-cell maps for the pixel pass: convert weight, structure, chroma consistency.
    std::vector<float> wMap(cells), sMap(cells), dcMap(cells);
    for (size_t c = 0; c < cells; ++c) {
        float w = std::clamp(1.0f - dis3[c] / Q::kDelta, 0.0f, 1.0f);
        w = w * orient3[c];
        const float sm = std::clamp(struct33[c], 0.0f, 1.0f);
        w = w * sm;
        if (clip3[c] > 0.0f) w = 0.0f;
        w *= std::clamp((Ls[1][c] - Q::kSnrLow) / (Q::kSnrHigh - Q::kSnrLow), 0.0f, 1.0f);
        wMap[c] = w;
        sMap[c] = sm;
        dcMap[c] = std::clamp(1.0f - dis3[c] / Q::kDeltaChroma, 0.0f, 1.0f);
    }
    for (auto* v : {&wMap, &sMap, &dcMap}) detail::quadConvertQuantize(*v);
    // ---- pixel pass
    const auto up = [&](const std::vector<float>& a, int x0, int y0, float fx, float fy) {
        const float* p = a.data();
        const float top = p[size_t(y0) * cw + x0] * (1.0f - fx) + p[size_t(y0) * cw + x0 + 1] * fx;
        const float bot = p[size_t(y0 + 1) * cw + x0] * (1.0f - fx) + p[size_t(y0 + 1) * cw + x0 + 1] * fx;
        return top * (1.0f - fy) + bot * fy;
    };
    const auto det = [&](int x, int y) {  // Detail output, black removed, reflected at the window border
        const auto rf = [](int v, int n) { return v < 0 ? -v : v >= n ? 2 * n - 2 - v : v; };
        return float(detailIn[size_t(rf(y, int(height))) * width + size_t(rf(x, int(width)))]) - black;
    };
    detail::quadConvertParallel(height, threads, [&](uint32_t y0r, uint32_t y1r) {
        for (uint32_t y = y0r; y < y1r; ++y) {
            const float yy = (float(y) + 0.5f) * 0.25f - 0.5f;
            const int cy = std::clamp(int(std::floor(yy)), 0, ch - 2);
            const float fy = yy - float(cy);
            for (uint32_t x = 0; x < width; ++x) {
                const float xx = (float(x) + 0.5f) * 0.25f - 0.5f;
                const int cx = std::clamp(int(std::floor(xx)), 0, cw - 2);
                const float fx = xx - float(cx);
                const int bpy = int((y >> 1) & 1u), bpx = int((x >> 1) & 1u);
                const int cq = (bpy == ry && bpx == rx) ? 0 : (bpy == by && bpx == bx) ? 2 : 1;
                const int oy = int(y & 1u), ox = int(x & 1u);
                const int cb = (oy == ry && ox == rx) ? 0 : (oy == by && ox == bx) ? 2 : 1;
                const float v = sample(x, y);
                const float wp = up(wMap, cx, cy, fx, fy);
                const float d = det(int(x), int(y));
                float result = d;
                if (wp > 0.0f) {
                    float conv = v;
                    if (cb != cq) {
                        const float num = up(cb == 0 ? Ls[0] : cb == 2 ? Ls[2] : Ls[1], cx, cy, fx, fy);
                        const float dn = up(cq == 0 ? Ls[0] : cq == 2 ? Ls[2] : Ls[1], cx, cy, fx, fy);
                        conv = v * num / std::max(dn, 1.0f);
                    }
                    result = wp * conv + (1.0f - wp) * d;
                }
                if (cb != 1) {
                    const float smp = up(sMap, cx, cy, fx, fy), dcp = up(dcMap, cx, cy, fx, fy);
                    const float wc = smp * dcp * (1.0f - wp);
                    if (wc > 0.0f) {
                        const float l = det(int(x) - 1, int(y)), r = det(int(x) + 1, int(y)), u = det(int(x), int(y) - 1),
                                    dn = det(int(x), int(y) + 1);
                        const float ghat = std::fabs(l - r) <= std::fabs(u - dn) ? 0.5f * (l + r) : 0.5f * (u + dn);
                        const float lfc = up(Lf[cb == 0 ? 0 : 2], cx, cy, fx, fy), lfg = up(Lf[1], cx, cy, fx, fy);
                        const float fix = ghat * (lfc / std::max(lfg, Q::kDarkRatioFloor));
                        result = result * (1.0f - wc) + fix * wc;
                    }
                }
                out[size_t(y) * width + x] = uint16_t(std::clamp(std::floor(result + black + 0.5f), 0.0f, 65535.0f));
            }
        }
    });
}

}  // namespace rawrcam::imaging
