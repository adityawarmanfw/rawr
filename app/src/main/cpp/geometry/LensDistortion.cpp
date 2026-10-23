#include "geometry/LensDistortion.h"

#include <algorithm>
#include <cmath>

namespace rawrcam::geometry {

bool isValid(const LensDistortionModel& m) noexcept {
    if (m.width == 0 || m.height == 0) return false;
    if (!(m.fx > 0.0) || !(m.fy > 0.0) || !(m.outputScale > 0.0)) return false;
    if (!std::isfinite(m.cx) || !std::isfinite(m.cy) || !std::isfinite(m.fx) || !std::isfinite(m.fy)) return false;
    return std::all_of(m.k.begin(), m.k.end(), [](double v) { return std::isfinite(v); });
}

std::optional<LensDistortionModel> scaledTo(const LensDistortionModel& m, uint32_t width, uint32_t height) noexcept {
    if (!isValid(m) || width == 0 || height == 0) return std::nullopt;
    const double sx = double(width) / m.width, sy = double(height) / m.height;
    // Same aspect to within half a pixel at the target size.
    if (std::abs(sx - sy) * std::max(m.width, m.height) > 0.5 * std::max(sx, sy)) return std::nullopt;
    LensDistortionModel out = m;
    out.width = width;
    out.height = height;
    out.fx *= sx;
    out.fy *= sy;
    // Pixel indices: the continuous edge (index - 0.5) scales, not the index.
    out.cx = (m.cx + 0.5) * sx - 0.5;
    out.cy = (m.cy + 0.5) * sy - 0.5;
    return out;
}

std::array<double, 2> sourcePixel(const LensDistortionModel& m, double x, double y) noexcept {
    const double u = (x - m.cx) / (m.fx * m.outputScale), v = (y - m.cy) / (m.fy * m.outputScale);
    const double r2 = u * u + v * v;
    const auto& k = m.k;
    const double radial = (1.0 + r2 * (k[0] + r2 * (k[1] + r2 * k[4]))) / (1.0 + r2 * (k[5] + r2 * (k[6] + r2 * k[7])));
    const double xd = u * radial + 2.0 * k[2] * u * v + k[3] * (r2 + 2.0 * u * u);
    const double yd = v * radial + k[2] * (r2 + 2.0 * v * v) + 2.0 * k[3] * u * v;
    return {m.cx + xd * m.fx, m.cy + yd * m.fy};
}

std::array<double, 2> sourceUv(const LensDistortionModel& m, double u, double v) noexcept {
    const auto p = sourcePixel(m, u * m.width - 0.5, v * m.height - 0.5);
    return {(p[0] + 0.5) / m.width, (p[1] + 0.5) / m.height};
}

std::array<double, 2> correctedUv(const LensDistortionModel& m, double u, double v) noexcept {
    std::array<double, 2> x{u, v};
    for (int i = 0; i < 20; ++i) {
        const auto s = sourceUv(m, x[0], x[1]);
        x[0] += u - s[0];
        x[1] += v - s[1];
    }
    return x;
}

static double containedOutputScaleUncached(const LensDistortionModel& model, uint32_t x, uint32_t y, uint32_t width,
                                    uint32_t height);

double containedOutputScale(const LensDistortionModel& model, uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height) {
    if (!isValid(model) || width < 2 || height < 2) return model.outputScale;
    // Callers ask per frame with the same lens and crop: keep the last answer.
    struct Last {
        LensDistortionModel model;
        uint32_t x = 0, y = 0, width = 0, height = 0;
        double scale = 0.0;
    };
    thread_local std::optional<Last> last;
    if (last && last->x == x && last->y == y && last->width == width && last->height == height &&
        last->model.width == model.width && last->model.height == model.height && last->model.fx == model.fx &&
        last->model.fy == model.fy && last->model.cx == model.cx && last->model.cy == model.cy &&
        last->model.k == model.k && last->model.outputScale == model.outputScale)
        return last->scale;
    const double scale = containedOutputScaleUncached(model, x, y, width, height);
    last = Last{model, x, y, width, height, scale};
    return scale;
}

static double containedOutputScaleUncached(const LensDistortionModel& model, uint32_t x, uint32_t y, uint32_t width,
                                    uint32_t height) {
    const double x0 = x, y0 = y, x1 = x + width - 1.0, y1 = y + height - 1.0;
    const auto inside = [&](double scale) {
        LensDistortionModel m = model;
        m.outputScale = scale;
        // Edges and a coarse interior grid: the source extremes sit on the edges.
        constexpr int kSteps = 64;
        for (int i = 0; i <= kSteps; ++i)
            for (int j = 0; j <= kSteps; j += (i == 0 || i == kSteps) ? 1 : kSteps) {
                for (int swap = 0; swap < 2; ++swap) {
                    const double a = double(swap ? j : i) / kSteps, b = double(swap ? i : j) / kSteps;
                    const auto s = sourcePixel(m, x0 + a * (x1 - x0), y0 + b * (y1 - y0));
                    if (s[0] < x0 || s[0] > x1 || s[1] < y0 || s[1] > y1) return false;
                }
            }
        return true;
    };
    double lo = model.outputScale;
    if (inside(lo)) return lo;
    double hi = lo;
    for (int i = 0; i < 16 && !inside(hi); ++i) hi *= 1.25;
    if (!inside(hi)) return model.outputScale;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        (inside(mid) ? hi : lo) = mid;
    }
    return hi;
}

std::optional<WarpRectilinearFit> fitWarpRectilinear(const LensDistortionModel& model, uint32_t width,
                                                     uint32_t height) {
    const auto m = scaledTo(model, width, height);
    if (!m) return std::nullopt;
    // DNG continuous coordinates: pixel index i covers [i, i + 1).
    const double cx = m->cx + 0.5, cy = m->cy + 0.5;
    const double norm = std::hypot(std::max(cx, width - cx), std::max(cy, height - cy));
    // Least squares on a grid over the whole area (edges included), x and y
    // residuals stacked: 6 unknowns via the normal equations.
    constexpr int kCols = 64, kRows = 48;
    double ata[6][7]{};
    const auto rows = [&](double X, double Y, double out[2][6], double target[2]) {
        const double u = (X - cx) / norm, v = (Y - cy) / norm, r2 = u * u + v * v;
        const auto s = sourcePixel(*m, X - 0.5, Y - 0.5);
        target[0] = (s[0] + 0.5 - cx) / norm;
        target[1] = (s[1] + 0.5 - cy) / norm;
        const double ax[6] = {u, u * r2, u * r2 * r2, u * r2 * r2 * r2, 2.0 * u * v, r2 + 2.0 * u * u};
        const double ay[6] = {v, v * r2, v * r2 * r2, v * r2 * r2 * r2, r2 + 2.0 * v * v, 2.0 * u * v};
        std::copy(ax, ax + 6, out[0]);
        std::copy(ay, ay + 6, out[1]);
    };
    for (int j = 0; j <= kRows; ++j)
        for (int i = 0; i <= kCols; ++i) {
            double a[2][6], t[2];
            rows(double(width) * i / kCols, double(height) * j / kRows, a, t);
            for (int e = 0; e < 2; ++e)
                for (int r = 0; r < 6; ++r) {
                    for (int c = 0; c < 6; ++c) ata[r][c] += a[e][r] * a[e][c];
                    ata[r][6] += a[e][r] * t[e];
                }
        }
    // Gaussian elimination with partial pivoting.
    for (int c = 0; c < 6; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 6; ++r)
            if (std::abs(ata[r][c]) > std::abs(ata[pivot][c])) pivot = r;
        if (std::abs(ata[pivot][c]) < 1e-12) return std::nullopt;
        std::swap(ata[c], ata[pivot]);
        for (int r = 0; r < 6; ++r) {
            if (r == c) continue;
            const double f = ata[r][c] / ata[c][c];
            for (int k = c; k < 7; ++k) ata[r][k] -= f * ata[c][k];
        }
    }
    WarpRectilinearFit fit;
    for (int i = 0; i < 4; ++i) fit.kr[size_t(i)] = ata[i][6] / ata[i][i];
    for (int i = 0; i < 2; ++i) fit.kt[size_t(i)] = ata[4 + i][6] / ata[4 + i][4 + i];
    fit.center = {cx / width, cy / height};
    double sum = 0.0;
    int count = 0;
    for (int j = 0; j <= kRows; ++j)
        for (int i = 0; i <= kCols; ++i) {
            double a[2][6], t[2];
            rows(double(width) * i / kCols, double(height) * j / kRows, a, t);
            double p[2]{};
            for (int e = 0; e < 2; ++e) {
                for (int r = 0; r < 4; ++r) p[e] += a[e][r] * fit.kr[size_t(r)];
                p[e] += a[e][4] * fit.kt[0] + a[e][5] * fit.kt[1];
            }
            const double err = std::hypot(p[0] - t[0], p[1] - t[1]) * norm;
            sum += err;
            fit.maxErrorPx = std::max(fit.maxErrorPx, err);
            ++count;
        }
    fit.meanErrorPx = sum / count;
    return fit;
}

namespace {
// Display UV -> stored UV for an EXIF orientation (as present.frag samples).
std::array<double, 2> orientedSource(uint16_t o, double x, double y) noexcept {
    switch (o) {
        case 2: return {1.0 - x, y};
        case 3: return {1.0 - x, 1.0 - y};
        case 4: return {x, 1.0 - y};
        case 5: return {y, x};
        case 6: return {y, 1.0 - x};
        case 7: return {1.0 - y, 1.0 - x};
        case 8: return {1.0 - y, x};
        default: return {x, y};
    }
}
}  // namespace

uint16_t mirroredOrientation(uint16_t orientation, uint32_t mirror) noexcept {
    if (orientation < 1 || orientation > 8) orientation = 1;
    mirror &= kLensMirrorMask;
    if (!mirror) return orientation;
    // Stored-unmirrored source for display q is mirror(source_o(q)); find the
    // orientation that maps the same (three corners pin an axis-aligned map).
    constexpr double corners[3][2] = {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};
    for (uint16_t candidate = 1; candidate <= 8; ++candidate) {
        bool same = true;
        for (const auto& c : corners) {
            const auto s = orientedSource(orientation, c[0], c[1]);
            const auto want = mirroredUv(mirror, s[0], s[1]);
            const auto got = orientedSource(candidate, c[0], c[1]);
            same = same && want == got;
        }
        if (same) return candidate;
    }
    return orientation;
}

}  // namespace rawrcam::geometry
