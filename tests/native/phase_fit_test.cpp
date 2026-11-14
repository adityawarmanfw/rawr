// Phase-response fit (imaging/PhaseFit.h): a smooth scene sampled through a
// 4x4 block mosaic whose 16 positions respond differently (gain steps with
// brightness) must come back with a much smaller period-4 residual after the
// fit; a mosaic without any pattern must stay (nearly) unchanged; a frame too
// small or flat-poor for statistics must pass through as the exact 2x2 sum.
#include <cmath>
#include <cstdio>
#include <vector>

#include "imaging/PhaseFit.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
float smoothScene(uint32_t x, uint32_t y) {
    return 60.0f + 500.0f * (0.5f + 0.5f * std::sin(float(x) * 0.0021f)) * (0.6f + 0.4f * std::cos(float(y) * 0.0017f));
}
// Position response: +-7%, and a different pattern above 30 DN (dual-gain-like).
float response(uint32_t type, uint32_t p, float signal) {
    const float a = std::sin(1.3f * float(type * 16 + p)), b = std::cos(2.1f * float(type * 16 + p));
    return 1.0f + 0.07f * (signal < 30.0f ? a : b);
}
// Peak-to-peak of the mean over the 4x4 phases of (sum / local mean - 1) on interior 2x2 sums.
float phaseResidual(const std::vector<uint16_t>& q, uint32_t w, uint32_t h) {
    double acc[4][4] = {};
    uint32_t count[4][4] = {};
    for (uint32_t y = 16; y + 16 < h; ++y)
        for (uint32_t x = 16; x + 16 < w; ++x) {
            double mean = 0.0;
            for (int j = -8; j < 8; ++j)
                for (int i = -8; i < 8; ++i) mean += q[size_t(y + j) * w + x + i];
            mean /= 256.0;
            acc[y & 3][x & 3] += q[size_t(y) * w + x] / mean - 1.0;
            ++count[y & 3][x & 3];
        }
    double lo = 1e9, hi = -1e9;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            const double v = acc[j][i] / count[j][i];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    return float(hi - lo);
}
}  // namespace

int main() {
    using namespace rawrcam::imaging;
    constexpr uint32_t w = 1024, h = 768;
    std::vector<uint16_t> patterned(size_t(w) * h), clean(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float s = smoothScene(x, y);
            const uint32_t type = (((y >> 2) & 1u) << 1) | ((x >> 2) & 1u), p = ((y & 3u) << 2) | (x & 3u);
            clean[size_t(y) * w + x] = uint16_t(std::lround(64.0f + s));
            patterned[size_t(y) * w + x] = uint16_t(std::lround(64.0f + s * response(type, p, s)));
        }
    std::vector<uint16_t> plainSum(size_t(w / 2) * (h / 2)), fitted(plainSum.size()), cleanFit(plainSum.size());
    for (uint32_t y = 0; y < h / 2; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) {
            const size_t i = size_t(2 * y) * w + 2 * x;
            plainSum[size_t(y) * (w / 2) + x] =
                uint16_t(patterned[i] + patterned[i + 1] + patterned[i + w] + patterned[i + w + 1]);
        }
    hexadecaSumWithFit(patterned.data(), w, h, 64.0f, 3, fitted.data(), 2);
    hexadecaSumWithFit(clean.data(), w, h, 64.0f, 3, cleanFit.data(), 2);
    const float before = phaseResidual(plainSum, w / 2, h / 2), after = phaseResidual(fitted, w / 2, h / 2);
    std::printf("period-4 residual before %.5f after %.5f\n", before, after);
    check(before > 0.004f, "synthetic pattern is visible before the fit");
    check(after < before * 0.4f, "fit removes most of the period-4 pattern");
    // No pattern: the correction must not move the data by more than a few codes.
    int worst = 0;
    for (uint32_t y = 0; y < h / 2; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) {
            const size_t i = size_t(2 * y) * w + 2 * x;
            const int plain = clean[i] + clean[i + 1] + clean[i + w] + clean[i + w + 1];
            worst = std::max(worst, std::abs(plain - int(cleanFit[size_t(y) * (w / 2) + x])));
        }
    std::printf("clean mosaic: worst change %d quarter-DN codes\n", worst);
    check(worst <= 8, "fit leaves a pattern-free mosaic alone");
    // Too small for statistics: exact sum.
    std::vector<uint16_t> tiny(64 * 64, 200), tinyOut(32 * 32, 0);
    hexadecaSumWithFit(tiny.data(), 64, 64, 64.0f, 3, tinyOut.data(), 1);
    bool exact = true;
    for (auto v : tinyOut) exact &= v == 800;
    check(exact, "no statistics -> exact 2x2 sum");
    if (failures == 0) std::printf("phase_fit_test: ok\n");
    return failures == 0 ? 0 : 1;
}
