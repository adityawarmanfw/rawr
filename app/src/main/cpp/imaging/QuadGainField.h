#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace rawrcam::imaging {

// Per-position response of a quad (2x2 same-colour blocks) sensor across the
// frame. The four pixels under one colour block do not see the same light: the
// microlens angle changes over the field, so one diagonal pair reads brighter
// than the other towards the corners (vivo X200 Ultra main: +-5.8% in the
// corners, 0 at the centre, sign by quadrant) plus row/column tilts. A
// remosaic keeps two of the four as Bayer samples and rebuilds the other two
// from neighbours, so any imbalance becomes a 4-px grid or diagonal stripes.
//
// The field is fitted per frame from the frame's own flat blocks: the ratio of
// each sample to its block mean, gathered per tile (256 px on 50 MP) on every
// seventh block row, then a robust weighted quadratic surface per (block phase, position)
// in field coordinates u, v in [-1, 1]. No calibration, no sensor constants.
// A frame whose fitted field is small (well matched sensors) or within the
// fit's own noise leaves `active` false and is not touched.
//
// Apply (CPU here, GPU raw_remosaic.comp pass 14, same expressions): each
// sample becomes black + (v - black) / g(u, v), rounded to the stored code.
struct QuadGainField {
    static constexpr uint32_t kSurfaces = 16;  // block phase (2x2) x in-block position (2x2)
    static constexpr uint32_t kTerms = 6;      // 1, u, v, u*v, u^2, v^2 (of g - 1)
    bool active = false;
    std::array<float, kSurfaces * kTerms> coef{};
    float deviation = 0.0f;  // largest RMS of g - 1 over the frame among the surfaces
    float noise = 0.0f;      // the fit's standard error at that surface
};

// Surface index of the sample at (x, y): block parity, then position inside the block.
inline uint32_t quadGainSurface(uint32_t x, uint32_t y) {
    return ((((y >> 1) & 1u) * 2u + ((x >> 1) & 1u)) * 4u) + (y & 1u) * 2u + (x & 1u);
}
// Field coordinate of row/column i of n (centre of the pixel). Mirrored by the shader.
inline float quadGainCoord(uint32_t i, uint32_t n) { return (float(i) + 0.5f) / float(n) * 2.0f - 1.0f; }
// 1 / g at (u, v) for coefficients k (kTerms floats). Mirrored by the shader.
inline float quadGainInverse(const float* k, float u, float v) {
    const float g = 1.0f + k[0] + u * (k[1] + k[3] * v + k[4] * u) + v * (k[2] + k[5] * v);
    return 1.0f / g;
}
// Mirrors the shader's roundDn.
inline uint16_t quadGainApplyOne(uint16_t sample, float black, float inverse) {
    const float v = (float(sample) - black) * inverse + black;
    return uint16_t(std::clamp(std::floor(std::fabs(v) + 0.5f) * (v < 0.0f ? -1.0f : 1.0f), 0.0f, 65535.0f));
}

inline QuadGainField fitQuadGainField(const uint16_t* raw, uint32_t width, uint32_t height, float black, float white,
                                      uint32_t threads = 1) {
    QuadGainField field;
    constexpr uint32_t kRowStride = 7;  // odd: both block parities
    // 256 px tiles on 50 MP, smaller on small frames: at least ~16 x 12 tiles for the surface fit.
    const uint32_t kTile = std::clamp(std::min(width / 16, height / 12) / 64 * 64, 64u, 256u);
    const uint32_t bw = width / 2, bh = height / 2;
    if (bw < 16 || bh < 16 || !(white > black)) return field;
    const uint32_t tw = (width + kTile - 1) / kTile, th = (height + kTile - 1) / kTile;
    const float range = white - black, wCap = 0.4f * range, mMin = std::max(8.0f, 0.02f * range), mMax = 0.9f * range;
    const size_t cells = size_t(tw) * th * QuadGainField::kSurfaces;
    threads = std::clamp(threads, 1u, 16u);
    std::vector<std::vector<double>> acc(threads, std::vector<double>(cells * 2, 0.0));
    auto mean = [&](uint32_t bx, uint32_t by) {
        const uint16_t* p = raw + size_t(2 * by) * width + 2 * bx;
        return 0.25f * ((float(p[0]) + float(p[1])) + (float(p[width]) + float(p[width + 1]))) - black;
    };
    auto work = [&](uint32_t t) {
        double* a = acc[t].data();
        uint32_t row = 0;
        for (uint32_t by = 2; by + 2 < bh; by += kRowStride, ++row) {
            if (row % threads != t) continue;
            const uint32_t ty = (2 * by) / kTile;
            for (uint32_t bx = 2; bx + 2 < bw; ++bx) {
                const float m = mean(bx, by);
                if (m < mMin || m > mMax) continue;
                // Flat: the same colour's neighbouring blocks agree with this one.
                const float nb = 0.25f * ((mean(bx - 2, by) + mean(bx + 2, by)) + (mean(bx, by - 2) + mean(bx, by + 2)));
                if (std::fabs(m - nb) >= 0.03f * m + 1.0f) continue;
                const uint16_t* p = raw + size_t(2 * by) * width + 2 * bx;
                const float w = std::min(m, wCap), scale = w / m;
                const uint32_t surface0 = ((by & 1u) * 2u + (bx & 1u)) * 4u;
                double* cell = a + ((size_t(ty) * tw + (2 * bx) / kTile) * QuadGainField::kSurfaces + surface0) * 2;
                const float s[4] = {float(p[0]) - black, float(p[1]) - black, float(p[width]) - black,
                                    float(p[width + 1]) - black};
                for (int k = 0; k < 4; ++k) {
                    cell[2 * k] += double(s[k] * scale);
                    cell[2 * k + 1] += double(w);
                }
            }
        }
    };
    {
        std::vector<std::thread> pool;
        for (uint32_t t = 1; t < threads; ++t) pool.emplace_back(work, t);
        work(0);
        for (auto& p : pool) p.join();
    }
    for (uint32_t t = 1; t < threads; ++t)
        for (size_t i = 0; i < acc[0].size(); ++i) acc[0][i] += acc[t][i];
    const std::vector<double>& sums = acc[0];

    struct Point {
        double u, v, r, w;
    };
    std::array<std::array<double, QuadGainField::kTerms>, QuadGainField::kSurfaces> fitted{};
    float worstDeviation = 0.0f, worstNoise = 0.0f;
    bool enough = true;
    for (uint32_t s = 0; s < QuadGainField::kSurfaces; ++s) {
        std::vector<Point> pts;
        for (uint32_t ty = 0; ty < th; ++ty)
            for (uint32_t tx = 0; tx < tw; ++tx) {
                const double* c = &sums[((size_t(ty) * tw + tx) * QuadGainField::kSurfaces + s) * 2];
                if (!(c[1] > 0.0)) continue;
                const double cx = std::min(double(width), (tx + 0.5) * kTile), cy = std::min(double(height), (ty + 0.5) * kTile);
                pts.push_back({cx / width * 2.0 - 1.0, cy / height * 2.0 - 1.0, c[0] / c[1] - 1.0, c[1]});
            }
        if (pts.size() < 24) {
            enough = false;
            break;
        }
        // Tile weights shrink with the tile's residual (Cauchy, 3 x robust scale), three rounds.
        std::vector<double> robust(pts.size(), 1.0);
        std::array<double, QuadGainField::kTerms> c{};
        double scaleEst = 0.0;
        for (int round = 0; round < 4; ++round) {
            double ata[6][7] = {};
            for (size_t i = 0; i < pts.size(); ++i) {
                const Point& p = pts[i];
                const double b[6] = {1.0, p.u, p.v, p.u * p.v, p.u * p.u, p.v * p.v};
                const double w = p.w * robust[i];
                for (int r = 0; r < 6; ++r) {
                    for (int q = 0; q < 6; ++q) ata[r][q] += w * b[r] * b[q];
                    ata[r][6] += w * b[r] * p.r;
                }
            }
            // Gaussian elimination with partial pivoting.
            for (int col = 0; col < 6; ++col) {
                int piv = col;
                for (int r = col + 1; r < 6; ++r)
                    if (std::fabs(ata[r][col]) > std::fabs(ata[piv][col])) piv = r;
                for (int q = 0; q < 7; ++q) std::swap(ata[col][q], ata[piv][q]);
                if (std::fabs(ata[col][col]) < 1e-12) return field;
                for (int r = 0; r < 6; ++r) {
                    if (r == col) continue;
                    const double f = ata[r][col] / ata[col][col];
                    for (int q = col; q < 7; ++q) ata[r][q] -= f * ata[col][q];
                }
            }
            for (int r = 0; r < 6; ++r) c[r] = ata[r][6] / ata[r][r];
            std::vector<double> res(pts.size());
            for (size_t i = 0; i < pts.size(); ++i) {
                const Point& p = pts[i];
                res[i] = p.r - (c[0] + p.u * (c[1] + c[3] * p.v + c[4] * p.u) + p.v * (c[2] + c[5] * p.v));
            }
            std::vector<double> absRes(res.size());
            for (size_t i = 0; i < res.size(); ++i) absRes[i] = std::fabs(res[i]);
            std::nth_element(absRes.begin(), absRes.begin() + absRes.size() / 2, absRes.end());
            scaleEst = 1.4826 * absRes[absRes.size() / 2] + 1e-6;
            for (size_t i = 0; i < pts.size(); ++i) {
                const double z = res[i] / (3.0 * scaleEst);
                robust[i] = 1.0 / (1.0 + z * z);
            }
        }
        fitted[s] = c;
        // Size of the field: RMS of g - 1 over the tile centres; its noise: the tile scatter over the
        // effective number of tiles (weights uneven), per fitted term.
        double rms = 0.0, w2 = 0.0, w1 = 0.0;
        for (const Point& p : pts) {
            const double g = c[0] + p.u * (c[1] + c[3] * p.v + c[4] * p.u) + p.v * (c[2] + c[5] * p.v);
            rms += g * g;
            w1 += p.w;
            w2 += p.w * p.w;
        }
        rms = std::sqrt(rms / pts.size());
        const double effective = w1 * w1 / w2;
        const double se = scaleEst * std::sqrt(double(QuadGainField::kTerms) / std::max(effective, 1.0));
        if (rms > worstDeviation) {
            worstDeviation = float(rms);
            worstNoise = float(se);
        }
    }
    if (!enough) return field;
    // The four positions of a block phase average to the block mean: remove their common part.
    for (uint32_t phase = 0; phase < 4; ++phase)
        for (uint32_t t = 0; t < QuadGainField::kTerms; ++t) {
            double m = 0.0;
            for (uint32_t k = 0; k < 4; ++k) m += fitted[phase * 4 + k][t];
            m *= 0.25;
            for (uint32_t k = 0; k < 4; ++k)
                field.coef[(phase * 4 + k) * QuadGainField::kTerms + t] = float(fitted[phase * 4 + k][t] - m);
        }
    field.deviation = worstDeviation;
    field.noise = worstNoise;
    // Correct only a field clearly above the fit's noise and above 0.75%: well matched quads measure
    // 0.2-0.5% per position (invisible), the X200 Ultra main about 5%.
    field.active = worstDeviation > std::max(0.0075f, 4.0f * worstNoise);
    return field;
}

// Divides the field out of a quad frame in place (CPU path; the GPU applies it while loading the raw).
inline void applyQuadGainField(uint16_t* raw, uint32_t width, uint32_t height, float black, const QuadGainField& field,
                               uint32_t threads = 1) {
    if (!field.active) return;
    threads = std::clamp(threads, 1u, 16u);
    auto work = [&](uint32_t t) {
        for (uint32_t y = t; y < height; y += threads) {
            const float v = quadGainCoord(y, height);
            uint16_t* row = raw + size_t(y) * width;
            for (uint32_t x = 0; x < width; ++x) {
                const float* k = &field.coef[quadGainSurface(x, y) * QuadGainField::kTerms];
                row[x] = quadGainApplyOne(row[x], black, quadGainInverse(k, quadGainCoord(x, width), v));
            }
        }
    };
    std::vector<std::thread> pool;
    for (uint32_t t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& p : pool) p.join();
}

}  // namespace rawrcam::imaging
