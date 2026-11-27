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
// in field coordinates u, v in [-1, 1], plus a coarse residual grid (32 x 24
// nodes, bilinear) for what the quadratic cannot bend to: a strong field
// (+-11% on a close-focus X200 Ultra shot) leaves 1-3% in the corners. Grid
// nodes average the tiles' residuals around them and shrink towards zero
// where the tiles are few or noisy, so content and empty areas stay on the
// quadratic. No calibration, no sensor constants.
// A frame whose fitted field is small (well matched sensors) or within the
// fit's own noise leaves `active` false and is not touched.
//
// Apply (CPU here, GPU raw_remosaic.comp pass 14, same expressions): each
// sample becomes black + (v - black) / g(u, v), rounded to the stored code.
struct QuadGainField {
    static constexpr uint32_t kSurfaces = 16;  // block phase (2x2) x in-block position (2x2)
    static constexpr uint32_t kTerms = 6;      // 1, u, v, u*v, u^2, v^2 (of g - 1)
    static constexpr uint32_t kGridX = 32, kGridY = 24, kGridCells = kGridX * kGridY;
    // Upload layout (GPU): coef, then grid.
    static constexpr uint32_t kFloats = kSurfaces * kTerms + kSurfaces * kGridCells;
    bool active = false;
    std::array<float, kSurfaces * kTerms> coef{};
    std::array<float, kSurfaces * kGridCells> grid{};  // per surface, row-major nodes; added to g - 1
    float deviation = 0.0f;  // largest RMS of g - 1 over the frame among the surfaces
    float noise = 0.0f;      // the fit's standard error at that surface
    // Pixel noise of the frame, variance = noiseSlope * (level - black) + noiseFloor (DN^2), from the
    // diagonal split of flat blocks (s0 + s3 - s1 - s2: variance 4 sigma^2, no first-order image signal)
    // less the same-colour neighbour's (the gain field cancels). Set whether or not the field is active; 0, 0 when the frame has too few
    // flat blocks. Used by the quad disparity correction (QuadDisparity.h).
    float noiseSlope = 0.0f, noiseFloor = 0.0f;
};

// Surface index of the sample at (x, y): block parity, then position inside the block.
inline uint32_t quadGainSurface(uint32_t x, uint32_t y) {
    return ((((y >> 1) & 1u) * 2u + ((x >> 1) & 1u)) * 4u) + (y & 1u) * 2u + (x & 1u);
}
// Field coordinate of row/column i of n (centre of the pixel). Mirrored by the shader.
inline float quadGainCoord(uint32_t i, uint32_t n) { return (float(i) + 0.5f) / float(n) * 2.0f - 1.0f; }
// Bilinear residual grid of one surface at (u, v); nodes span the frame
// edge to edge (node i at u = 2 i / (GX - 1) - 1). Mirrored by the shader.
inline float quadGainGrid(const float* grid, float u, float v) {
    constexpr uint32_t GX = QuadGainField::kGridX, GY = QuadGainField::kGridY;
    const float fx = std::clamp((u + 1.0f) * 0.5f * float(GX - 1), 0.0f, float(GX - 1));
    const float fy = std::clamp((v + 1.0f) * 0.5f * float(GY - 1), 0.0f, float(GY - 1));
    const uint32_t x0 = std::min(uint32_t(fx), GX - 2), y0 = std::min(uint32_t(fy), GY - 2);
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float* r0 = grid + y0 * GX + x0;
    const float* r1 = r0 + GX;
    const float top = r0[0] + (r0[1] - r0[0]) * tx, bottom = r1[0] + (r1[1] - r1[0]) * tx;
    return top + (bottom - top) * ty;
}
// 1 / g at (u, v) for coefficients k (kTerms floats) and the surface's grid. Mirrored by the shader.
// g is bounded to 0.7..1.3: where no tile had enough signal (dark corners) the polynomial and grid
// extrapolate freely (seen reaching -0.03 and 1.8 on a 14 mm ultrawide), and 1 / g must not follow.
inline float quadGainInverse(const float* k, const float* grid, float u, float v) {
    const float g = 1.0f + k[0] + u * (k[1] + k[3] * v + k[4] * u) + v * (k[2] + k[5] * v) + quadGainGrid(grid, u, v);
    return 1.0f / std::clamp(g, 0.7f, 1.3f);
}
// Mirrors the shader's roundDn.
inline uint16_t quadGainApplyOne(uint16_t sample, float black, float inverse) {
    const float v = (float(sample) - black) * inverse + black;
    return uint16_t(std::clamp(std::floor(std::fabs(v) + 0.5f) * (v < 0.0f ? -1.0f : 1.0f), 0.0f, 65535.0f));
}

namespace detail {
struct QuadGainPoint {
    double u, v, r, w;  // tile centre (field coordinates), mean ratio - 1, weight
    uint32_t tx, ty;    // tile index
};
// Residual grid of one surface: at each node a Gaussian-weighted local plane
// (sigma one node spacing, at least one tile) through the tiles' residuals
// from the quadratic (tile weight x its robust weight); the node takes the
// plane's value there, so border and corner nodes are not pulled towards the
// interior. A node shrinks towards zero when its tiles weigh little next to
// what its kernel would hold on a fully flat frame (content, empty areas)
// and by its own signal-to-noise (Wiener: v^2 / (v^2 + se^2)). The tile noise
// comes from neighbouring tiles' residual differences (a smooth residual
// cancels there, noise does not), scaled by each tile's weight, so only a
// real, measured residual survives.
inline bool solve3(double a[3][4]) {
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        for (int q = 0; q < 4; ++q) std::swap(a[col][q], a[piv][q]);
        if (std::fabs(a[col][col]) < 1e-30) return false;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double f = a[r][col] / a[col][col];
            for (int q = col; q < 4; ++q) a[r][q] -= f * a[col][q];
        }
    }
    for (int r = 0; r < 3; ++r) a[r][3] /= a[r][r];
    return true;
}
inline void fitResidualGrid(const std::vector<QuadGainPoint>& pts, const std::vector<double>& robust,
                            const std::array<double, 6>& c, uint32_t tilesX, uint32_t tilesY, uint32_t width,
                            uint32_t height, uint32_t tile, std::vector<double>& out) {
    constexpr uint32_t GX = QuadGainField::kGridX, GY = QuadGainField::kGridY;
    const double sx = std::max(double(width) / (GX - 1), double(tile)) / width * 2.0;  // sigma in u units
    const double sy = std::max(double(height) / (GY - 1), double(tile)) / height * 2.0;
    std::vector<double> res(pts.size()), wt(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const QuadGainPoint& p = pts[i];
        res[i] = p.r - (c[0] + p.u * (c[1] + c[3] * p.v + c[4] * p.u) + p.v * (c[2] + c[5] * p.v));
        wt[i] = p.w;  // not the quadratic's robust weights: they mark exactly where it misfits
    }
    (void)robust;
    out.assign(GX * GY, 0.0);
    if (pts.empty()) return;
    std::vector<int> index(size_t(tilesX) * tilesY, -1);
    for (size_t i = 0; i < pts.size(); ++i) index[size_t(pts[i].ty) * tilesX + pts[i].tx] = int(i);
    std::vector<double> tw(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) tw[i] = pts[i].w;
    std::nth_element(tw.begin(), tw.begin() + tw.size() / 2, tw.end());
    const double medianTile = tw[tw.size() / 2];
        // Noise of a tile of weight w: q / w, robustly (median, / 0.455 for a chi-square median).
    std::vector<double> qs;
    for (size_t i = 0; i < pts.size(); ++i)
        for (int dir = 0; dir < 2; ++dir) {
            // Second difference across the tile (a residual's slope cancels, curvature over two tiles is small):
            // var(d) = q (1/w + 1/(4 wa) + 1/(4 wb)).
            const uint32_t tx = pts[i].tx, ty = pts[i].ty;
            if (dir == 0 ? (tx == 0 || tx + 1 >= tilesX) : (ty == 0 || ty + 1 >= tilesY)) continue;
            const int a = index[size_t(ty - (dir == 1)) * tilesX + (tx - (dir == 0))];
            const int b = index[size_t(ty + (dir == 1)) * tilesX + (tx + (dir == 0))];
            if (a < 0 || b < 0 || !(pts[i].w > 0.0) || !(pts[size_t(a)].w > 0.0) || !(pts[size_t(b)].w > 0.0)) continue;
            const double d = res[i] - 0.5 * (res[size_t(a)] + res[size_t(b)]);
            qs.push_back(d * d / (1.0 / pts[i].w + 0.25 / pts[size_t(a)].w + 0.25 / pts[size_t(b)].w));
        }
    double noise = 0.0;
    if (!qs.empty()) {
        std::nth_element(qs.begin(), qs.begin() + qs.size() / 2, qs.end());
        noise = qs[qs.size() / 2] / 0.455;
    }
    // Separable kernel: per node column and tile column (rows likewise) the offset in sigmas and its Gaussian.
    std::vector<double> dxs(size_t(GX) * tilesX), gxs(size_t(GX) * tilesX), dys(size_t(GY) * tilesY), gys(size_t(GY) * tilesY);
    for (uint32_t nx = 0; nx < GX; ++nx)
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
            const double un = double(nx) / (GX - 1) * 2.0 - 1.0;
            const double tu = std::min(double(width), (tx + 0.5) * tile) / width * 2.0 - 1.0;
            const double d = (tu - un) / sx;
            dxs[size_t(nx) * tilesX + tx] = d;
            gxs[size_t(nx) * tilesX + tx] = std::exp(-0.5 * d * d);
        }
    for (uint32_t ny = 0; ny < GY; ++ny)
        for (uint32_t ty = 0; ty < tilesY; ++ty) {
            const double vn = double(ny) / (GY - 1) * 2.0 - 1.0;
            const double tv = std::min(double(height), (ty + 0.5) * tile) / height * 2.0 - 1.0;
            const double d = (tv - vn) / sy;
            dys[size_t(ny) * tilesY + ty] = d;
            gys[size_t(ny) * tilesY + ty] = std::exp(-0.5 * d * d);
        }
    struct Near {
        uint32_t i;
        double g, dx, dy;
    };
    constexpr size_t kMaxNear = 256;
    Near near[kMaxNear];
    double rw[kMaxNear], dev[kMaxNear], absDev[kMaxNear];
    for (uint32_t ny = 0; ny < GY; ++ny)
        for (uint32_t nx = 0; nx < GX; ++nx) {
            size_t count = 0;
            double mass = 0.0;
            for (uint32_t ty = 0; ty < tilesY; ++ty) {
                const double dy = dys[size_t(ny) * tilesY + ty];
                if (dy * dy > 6.25) continue;
                for (uint32_t tx = 0; tx < tilesX; ++tx) {
                    const double dx = dxs[size_t(nx) * tilesX + tx];
                    if (dx * dx + dy * dy > 6.25) continue;  // 2.5 sigma
                    const double g = gxs[size_t(nx) * tilesX + tx] * gys[size_t(ny) * tilesY + ty];
                    mass += g;  // what a fully flat frame would put here, in median tiles
                    const int i = index[size_t(ty) * tilesX + tx];
                    if (i >= 0 && count < kMaxNear) near[count++] = {uint32_t(i), g, dx, dy};
                }
            }
            if (count == 0) continue;
            // Shrink towards zero where the kernel holds little data: a prior of a quarter of the
            // node's own full-frame mass (slopes a quarter as strongly).
            const double w0 = 0.25 * medianTile * mass;
            for (size_t n = 0; n < count; ++n) rw[n] = 1.0;
            double v = 0.0, se2 = 0.0;
            bool ok = true;
            for (int round = 0; round < 2 && ok; ++round) {
                double A[3][3] = {}, B[3][3] = {}, rhs[3] = {};
                for (size_t n = 0; n < count; ++n) {
                    const Near& t = near[n];
                    const double k = t.g * wt[t.i] * rw[n], x[3] = {1.0, t.dx, t.dy};
                    for (int r = 0; r < 3; ++r) {
                        rhs[r] += k * x[r] * res[t.i];
                        for (int q = 0; q < 3; ++q) {
                            A[r][q] += k * x[r] * x[q];
                            B[r][q] += t.g * t.g * rw[n] * rw[n] * pts[t.i].w * x[r] * x[q];
                        }
                    }
                }
                double sys[3][4], col[3][4];
                for (int r = 0; r < 3; ++r) {
                    for (int q = 0; q < 3; ++q)
                        sys[r][q] = col[r][q] = A[r][q] + (r == q ? w0 * (r == 0 ? 1.0 : 0.25) : 0.0);
                    sys[r][3] = rhs[r];
                    col[r][3] = r == 0 ? 1.0 : 0.0;
                }
                ok = solve3(sys) && solve3(col);
                if (!ok) break;
                v = sys[0][3];
                // Variance of the intercept: e0' S^-1 B S^-1 e0 * noise, S = A + prior.
                const double e[3] = {col[0][3], col[1][3], col[2][3]};
                se2 = 0.0;
                for (int r = 0; r < 3; ++r)
                    for (int q = 0; q < 3; ++q) se2 += e[r] * B[r][q] * e[q];
                se2 *= noise;
                if (round == 1) break;  // the weights would only serve a third round
                // Content tiles: Cauchy weights on the residual from the plane, at 3 x the larger of the
                // tile's own noise and the kernel's robust spread around the plane.
                for (size_t n = 0; n < count; ++n) {
                    const Near& t = near[n];
                    dev[n] = res[t.i] - (sys[0][3] + sys[1][3] * t.dx + sys[2][3] * t.dy);
                    absDev[n] = std::fabs(dev[n]);
                }
                std::nth_element(absDev, absDev + count / 2, absDev + count);
                const double spread = 1.4826 * absDev[count / 2];
                for (size_t n = 0; n < count; ++n) {
                    const double scale = std::max(std::sqrt(std::max(noise, 0.0) / pts[near[n].i].w), spread) + 1e-9;
                    const double z = dev[n] / (3.0 * scale);
                    rw[n] = 1.0 / (1.0 + z * z);
                }
            }
            if (!ok) continue;
            out[ny * GX + nx] = v * v * v / (v * v + se2 + 1e-30);
        }
}
}  // namespace detail

namespace detail {
// Line through the per-bin medians of the flat blocks' pixel-variance samples (log level bins, each weighted by
// its count; a chi-square median is 0.455 of its mean).
inline void fitQuadNoise(std::vector<std::vector<std::vector<float>>>& perThread, float mMin, float mMax,
                         QuadGainField& field) {
    const size_t bins = perThread.empty() ? 0 : perThread[0].size();
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    size_t total = 0;
    for (size_t b = 0; b < bins; ++b) {
        std::vector<float> v;
        for (auto& t : perThread) v.insert(v.end(), t[b].begin(), t[b].end());
        if (v.size() < 64) continue;
        total += v.size();
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        const double x = mMin * std::pow(double(mMax / mMin), (b + 0.5) / bins), y = v[v.size() / 2] / 0.455;
        const double w = double(v.size());
        sw += w;
        sx += w * x;
        sy += w * y;
        sxx += w * x * x;
        sxy += w * x * y;
    }
    if (total < 2000) return;
    const double det = sw * sxx - sx * sx;
    const double slope = det > 0.0 ? (sw * sxy - sx * sy) / det : 0.0;
    const double floor = (sy - slope * sx) / sw;
    field.noiseSlope = float(std::max(slope, 0.0));
    field.noiseFloor = float(std::max(floor, 0.25));
}
}  // namespace detail

inline QuadGainField fitQuadGainFieldFlat(const uint16_t* raw, uint32_t width, uint32_t height, float black, float white,
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
    // Pixel variance samples per level bin (48 log bins over [mMin, mMax], capped per thread).
    constexpr uint32_t kNoiseBins = 48, kNoiseCap = 1024;
    std::vector<std::vector<std::vector<float>>> noiseSamples(threads, std::vector<std::vector<float>>(kNoiseBins));
    // Level -> noise bin, by whole DN (a log per block costs more than the rest of the scan on phones).
    std::vector<uint8_t> noiseBin(size_t(std::ceil(mMax)) + 2);
    for (size_t i = 0; i < noiseBin.size(); ++i)
        noiseBin[i] = uint8_t(std::min<double>(kNoiseBins - 1, std::max(0.0, std::log(std::max(double(i), double(mMin)) / mMin) /
                                                                              std::log(mMax / mMin) * kNoiseBins)));
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
                // Diagonal split minus the next same-colour block's: the smooth gain field cancels, the
                // noise adds (variance 8 sigma^2).
                const uint16_t* q = p + 4;
                const float diagonal = (float(p[0]) + float(p[width + 1])) - (float(p[1]) + float(p[width])) -
                                       ((float(q[0]) + float(q[width + 1])) - (float(q[1]) + float(q[width])));
                const uint32_t bin = noiseBin[size_t(m)];
                if (noiseSamples[t][bin].size() < kNoiseCap) noiseSamples[t][bin].push_back(0.125f * diagonal * diagonal);
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
    detail::fitQuadNoise(noiseSamples, mMin, mMax, field);

    std::array<std::array<double, QuadGainField::kTerms>, QuadGainField::kSurfaces> fitted{};
    std::vector<std::vector<double>> gridFitted(QuadGainField::kSurfaces,
                                                std::vector<double>(QuadGainField::kGridCells, 0.0));
    std::vector<std::vector<detail::QuadGainPoint>> surfacePts(QuadGainField::kSurfaces);
    float worstDeviation = 0.0f, worstNoise = 0.0f;
    bool enough = true;
    for (uint32_t s = 0; s < QuadGainField::kSurfaces; ++s) {
        std::vector<detail::QuadGainPoint> pts;
        for (uint32_t ty = 0; ty < th; ++ty)
            for (uint32_t tx = 0; tx < tw; ++tx) {
                const double* c = &sums[((size_t(ty) * tw + tx) * QuadGainField::kSurfaces + s) * 2];
                if (!(c[1] > 0.0)) continue;
                const double cx = std::min(double(width), (tx + 0.5) * kTile), cy = std::min(double(height), (ty + 0.5) * kTile);
                pts.push_back({cx / width * 2.0 - 1.0, cy / height * 2.0 - 1.0, c[0] / c[1] - 1.0, c[1], tx, ty});
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
                const detail::QuadGainPoint& p = pts[i];
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
                const detail::QuadGainPoint& p = pts[i];
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
        for (const detail::QuadGainPoint& p : pts) {
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
        surfacePts[s] = std::move(pts);
    }
    if (!enough) return field;
    {  // Residual grids, one surface per task.
        std::vector<std::thread> pool;
        const auto gridWork = [&](uint32_t t) {
            for (uint32_t s = t; s < QuadGainField::kSurfaces; s += threads)
                detail::fitResidualGrid(surfacePts[s], {}, fitted[s], tw, th, width, height, kTile, gridFitted[s]);
        };
        for (uint32_t t = 1; t < threads; ++t) pool.emplace_back(gridWork, t);
        gridWork(0);
        for (auto& p : pool) p.join();
    }
    // The four positions of a block phase average to the block mean: remove their common part.
    for (uint32_t phase = 0; phase < 4; ++phase)
        for (uint32_t t = 0; t < QuadGainField::kTerms; ++t) {
            double m = 0.0;
            for (uint32_t k = 0; k < 4; ++k) m += fitted[phase * 4 + k][t];
            m *= 0.25;
            for (uint32_t k = 0; k < 4; ++k)
                field.coef[(phase * 4 + k) * QuadGainField::kTerms + t] = float(fitted[phase * 4 + k][t] - m);
        }
    for (uint32_t phase = 0; phase < 4; ++phase)
        for (uint32_t n = 0; n < QuadGainField::kGridCells; ++n) {
            double m = 0.0;
            for (uint32_t k = 0; k < 4; ++k) m += gridFitted[phase * 4 + k][n];
            m *= 0.25;
            for (uint32_t k = 0; k < 4; ++k)
                field.grid[(phase * 4 + k) * QuadGainField::kGridCells + n] = float(gridFitted[phase * 4 + k][n] - m);
        }
    field.deviation = worstDeviation;
    field.noise = worstNoise;
    // Correct only a field clearly above the fit's noise and above 0.75%: well matched quads measure
    // 0.2-0.5% per position (invisible), the X200 Ultra main about 5%.
    field.active = worstDeviation > std::max(0.0075f, 4.0f * worstNoise);
    return field;
}

namespace detail {
// Local per-position response. Per tile and surface (block phase x position in the block) the clipped mean of
// sample / block mean over every block with signal: in focus a symmetric 2x2 sampling makes that unbiased even in
// textured areas, and it is constant over a 30x range of brightness (a gain, not an offset). Unlike the flat-block
// polynomial fit it follows steep ramps (14 mm ultrawide: 0 to 11% top / bottom split within 600 px at the frame
// edge) and does not extrapolate where a region has no data (tiles without blocks take their neighbours').
// Tile estimates are smoothed by a 3 x 3 median, each phase's four positions renormalised to mean 1, resampled to
// the 32 x 24 residual grid, and shrunk by their own standard error. Returns false when the frame is too small.
inline bool fitLocalQuadField(const uint16_t* raw, uint32_t width, uint32_t height, float black, float white,
                              uint32_t threads, QuadGainField& field) {
    constexpr uint32_t GX = QuadGainField::kGridX, GY = QuadGainField::kGridY, kCells = QuadGainField::kGridCells;
    const uint32_t bw = width / 2, bh = height / 2;
    const uint32_t tb = std::clamp(std::min(bw / 40, bh / 30) / 2 * 2, 16u, 48u);  // tile edge in blocks, even
    const uint32_t tx = (bw + tb - 1) / tb, ty = (bh + tb - 1) / tb;
    if (tx < 4 || ty < 4 || !(white > black)) return false;
    const uint32_t kStride = tb >= 32 ? 3u : 1u;  // odd block-row stride (both block-row parities); small tiles take every row
    const float minLevel = std::max(12.0f, 0.012f * (white - black));
    struct Cell {
        double n = 0.0, s[4] = {0, 0, 0, 0}, q[4] = {0, 0, 0, 0};
    };
    std::vector<Cell> cells(size_t(ty) * tx * 4);
    threads = std::clamp(threads, 1u, 16u);
    const auto work = [&](uint32_t t) {
        for (uint32_t iy = t; iy < ty; iy += threads) {
            std::vector<Cell> row(size_t(tx) * 4);
            const auto blockMean = [&](uint32_t bx, uint32_t by) {
                const uint16_t* p = raw + size_t(2 * by) * width + 2 * bx;
                return 0.25f * ((float(p[0]) + float(p[1])) + (float(p[width]) + float(p[width + 1]))) - black;
            };
            for (uint32_t by = std::max(iy * tb, 2u); by < std::min(bh - 2, (iy + 1) * tb); ++by) {
                if (by % kStride != 0) continue;
                const uint16_t* r0 = raw + size_t(2 * by) * width;
                const uint16_t* r1 = r0 + width;
                for (uint32_t bx = 2; bx + 2 < bw; ++bx) {
                    float v[4] = {float(r0[2 * bx]) - black, float(r0[2 * bx + 1]) - black,
                                  float(r1[2 * bx]) - black, float(r1[2 * bx + 1]) - black};
                    const float m = 0.25f * ((v[0] + v[1]) + (v[2] + v[3]));
                    if (!(m > minLevel)) continue;
                    // In focus the four sit half a pixel either side of the block centre: take the scene's own
                    // split out (same-colour block gradient, as the disparity stage measures it) so a smooth
                    // ramp or parallel edges do not leak into the response.
                    const float gx = (blockMean(bx + 2, by) - blockMean(bx - 2, by)) * 0.125f;
                    const float gy = (blockMean(bx, by + 2) - blockMean(bx, by - 2)) * 0.125f;
                    v[0] += 0.5f * (gx + gy);
                    v[1] += 0.5f * (gy - gx);
                    v[2] += 0.5f * (gx - gy);
                    v[3] -= 0.5f * (gx + gy);
                    const float inv = 1.0f / m;
                    Cell& c = row[size_t(bx / tb) * 4 + ((by & 1u) * 2u + (bx & 1u))];
                    c.n += 1.0;
                    for (int k = 0; k < 4; ++k) {
                        const float r = std::clamp(v[k] * inv, 0.5f, 1.5f);
                        c.s[k] += r;
                        c.q[k] += double(r) * r;
                    }
                }
            }
            for (uint32_t ix = 0; ix < tx; ++ix)
                for (uint32_t ph = 0; ph < 4; ++ph) cells[(size_t(iy) * tx + ix) * 4 + ph] = row[size_t(ix) * 4 + ph];
        }
    };
    {
        std::vector<std::thread> pool;
        for (uint32_t t = 1; t < threads; ++t) pool.emplace_back(work, t);
        work(0);
        for (auto& p : pool) p.join();
    }
    // Tile values [ty][tx][phase][k]; NaN where a tile holds too few blocks.
    const size_t planeSize = size_t(ty) * tx;
    std::vector<float> g(planeSize * 16, std::nanf("")), se(planeSize * 16, std::nanf(""));
    std::vector<float> seSamples;
    for (uint32_t iy = 0; iy < ty; ++iy)
        for (uint32_t ix = 0; ix < tx; ++ix)
            for (uint32_t ph = 0; ph < 4; ++ph) {
                const Cell& c = cells[(size_t(iy) * tx + ix) * 4 + ph];
                if (c.n < 24.0) continue;
                for (uint32_t k = 0; k < 4; ++k) {
                    const double mean = c.s[k] / c.n, var = std::max(c.q[k] / c.n - mean * mean, 0.0);
                    const size_t at = (size_t(ph) * 4 + k) * planeSize + size_t(iy) * tx + ix;
                    g[at] = float(mean);
                    se[at] = float(std::sqrt(var / c.n));
                    seSamples.push_back(se[at]);
                }
            }
    if (seSamples.size() < 16 * 8) return false;
    std::nth_element(seSamples.begin(), seSamples.begin() + seSamples.size() / 2, seSamples.end());
    const float seMedian = seSamples[seSamples.size() / 2];
    // Fill empty tiles from their neighbours, then a 3 x 3 median per plane.
    std::vector<float> smooth(planeSize);
    for (uint32_t pl = 0; pl < 16; ++pl) {
        float* a = g.data() + size_t(pl) * planeSize;
        for (int round = 0; round < 8; ++round) {
            bool any = false;
            std::vector<float> next(a, a + planeSize);
            for (uint32_t iy = 0; iy < ty; ++iy)
                for (uint32_t ix = 0; ix < tx; ++ix) {
                    if (!std::isnan(a[size_t(iy) * tx + ix])) continue;
                    float sum = 0.0f;
                    int n = 0;
                    const int dy[4] = {-1, 1, 0, 0}, dx[4] = {0, 0, -1, 1};
                    for (int d = 0; d < 4; ++d) {
                        const int yy = int(iy) + dy[d], xx = int(ix) + dx[d];
                        if (yy < 0 || xx < 0 || yy >= int(ty) || xx >= int(tx)) continue;
                        const float v = a[size_t(yy) * tx + size_t(xx)];
                        if (!std::isnan(v)) sum += v, ++n;
                    }
                    if (n) next[size_t(iy) * tx + ix] = sum / float(n);
                    else any = true;
                }
            std::copy(next.begin(), next.end(), a);
            if (!any) break;
        }
        for (size_t i = 0; i < planeSize; ++i)
            if (std::isnan(a[i])) a[i] = 1.0f;
        for (uint32_t iy = 0; iy < ty; ++iy)
            for (uint32_t ix = 0; ix < tx; ++ix) {
                float v[9];
                int n = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const int yy = std::clamp(int(iy) + j, 0, int(ty) - 1), xx = std::clamp(int(ix) + i, 0, int(tx) - 1);
                        v[n++] = a[size_t(yy) * tx + size_t(xx)];
                    }
                std::nth_element(v, v + 4, v + 9);
                smooth[size_t(iy) * tx + ix] = v[4];
            }
        std::copy(smooth.begin(), smooth.end(), a);
    }
    for (uint32_t ph = 0; ph < 4; ++ph)  // the four positions of a block phase average to 1
        for (size_t i = 0; i < planeSize; ++i) {
            float m = 0.0f;
            for (uint32_t k = 0; k < 4; ++k) m += g[(size_t(ph) * 4 + k) * planeSize + i];
            m *= 0.25f;
            for (uint32_t k = 0; k < 4; ++k) g[(size_t(ph) * 4 + k) * planeSize + i] /= m;
        }
    // Resample to the residual grid (nodes edge to edge, node i at x = i W / (GX - 1)), shrink by the standard
    // error of what a node averages (about a 3 x 3 tile median of tiles of standard error seMedian).
    const float seNode = seMedian * 0.25f;
    const float tilePx = float(2 * tb);
    double worst = 0.0;
    for (uint32_t ph = 0; ph < 4; ++ph) {
        for (uint32_t n = 0; n < kCells; ++n) {
            const uint32_t j = n / GX, i = n % GX;
            // The outermost nodes lie half a tile beyond the outermost tile centres: extrapolate the local slope
            // (linear in the last two tiles) instead of holding the edge tile's value.
            const float fx = float(i) * float(width) / float(GX - 1) / tilePx - 0.5f;
            const float fy = float(j) * float(height) / float(GY - 1) / tilePx - 0.5f;
            const uint32_t x0 = uint32_t(std::clamp(std::floor(fx), 0.0f, float(tx - 2))),
                           y0 = uint32_t(std::clamp(std::floor(fy), 0.0f, float(ty - 2)));
            const uint32_t x1 = x0 + 1, y1 = y0 + 1;
            const float ax = fx - float(x0), ay = fy - float(y0);
            float v[4], mean = 0.0f;
            for (uint32_t k = 0; k < 4; ++k) {
                const float* a = g.data() + (size_t(ph) * 4 + k) * planeSize;
                const float top = a[size_t(y0) * tx + x0] * (1 - ax) + a[size_t(y0) * tx + x1] * ax;
                const float bot = a[size_t(y1) * tx + x0] * (1 - ax) + a[size_t(y1) * tx + x1] * ax;
                v[k] = top * (1 - ay) + bot * ay - 1.0f;
                v[k] = v[k] * v[k] * v[k] / (v[k] * v[k] + seNode * seNode + 1e-30f);
                mean += 0.25f * v[k];
            }
            for (uint32_t k = 0; k < 4; ++k) field.grid[(ph * 4 + k) * kCells + n] = v[k] - mean;
        }
    }
    for (uint32_t sf = 0; sf < QuadGainField::kSurfaces; ++sf) {
        double rms = 0.0;
        for (uint32_t n = 0; n < kCells; ++n) rms += double(field.grid[sf * kCells + n]) * field.grid[sf * kCells + n];
        worst = std::max(worst, std::sqrt(rms / kCells));
    }
    field.coef.fill(0.0f);
    field.deviation = float(worst);
    field.noise = seNode;
    field.active = worst > std::max(0.0075, 3.0 * double(seNode));
    return true;
}
}  // namespace detail

// The frame's per-position response field: the local estimate (detail::fitLocalQuadField), with the flat-block
// polynomial + grid fit as the fallback on frames too small for tiles. Both fill the pixel noise model.
inline QuadGainField fitQuadGainField(const uint16_t* raw, uint32_t width, uint32_t height, float black, float white,
                                      uint32_t threads = 1) {
    QuadGainField flat = fitQuadGainFieldFlat(raw, width, height, black, white, threads);
    QuadGainField local = flat;
    local.grid.fill(0.0f);
    if (!detail::fitLocalQuadField(raw, width, height, black, white, threads, local)) return flat;
    return local;
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
                const uint32_t surface = quadGainSurface(x, y);
                const float* k = &field.coef[surface * QuadGainField::kTerms];
                const float* grid = &field.grid[surface * QuadGainField::kGridCells];
                row[x] = quadGainApplyOne(row[x], black, quadGainInverse(k, grid, quadGainCoord(x, width), v));
            }
        }
    };
    std::vector<std::thread> pool;
    for (uint32_t t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& p : pool) p.join();
}

}  // namespace rawrcam::imaging
