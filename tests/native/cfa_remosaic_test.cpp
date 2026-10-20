// Block CFA remosaic (imaging/CfaRemosaic.h): a smooth colour scene sampled as
// a quad / 4x4 mosaic must come back close to the same scene sampled as
// Bayer, in every colour order, and Bayer sites of the block's own colour
// must keep their raw value.
#include <cmath>
#include <cstdio>
#include <vector>

#include "imaging/CfaRemosaic.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
// Smooth scene with gentle edges: per channel 0 R, 1 G, 2 B, in DN above a 64 black.
float scene(uint32_t c, uint32_t x, uint32_t y) {
    const float fx = float(x), fy = float(y);
    const float base = 300.0f + 200.0f * std::sin(fx * 0.031f) * std::cos(fy * 0.027f);
    const float tint[3] = {0.55f, 1.0f, 0.7f};
    return 64.0f + base * tint[c] + 40.0f * std::sin((fx + fy) * 0.05f + float(c));
}
}  // namespace

int main() {
    using namespace rawrcam::imaging;
    constexpr uint32_t W = 256, H = 192;
    for (const uint32_t n : {2u, 4u}) {
        for (uint32_t order = 0; order < 4; ++order) {
            std::vector<uint16_t> mosaic(W * H), truth(W * H);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x) {
                    mosaic[y * W + x] = uint16_t(std::lround(scene(cfaChannel(order, x / n, y / n), x, y)));
                    truth[y * W + x] = uint16_t(std::lround(scene(cfaChannel(order, x, y), x, y)));
                }
          for (const auto quality : {CfaRemosaicQuality::Fast, CfaRemosaicQuality::Standard, CfaRemosaicQuality::High}) {
            auto out = mosaic;
            remosaicBlockCfa(out.data(), W, H, n, order, 2, quality);
            double err = 0.0, maxErr = 0.0;
            bool ownKept = true;
            // Interior only: edges clamp.
            for (uint32_t y = 4 * n; y < H - 4 * n; ++y)
                for (uint32_t x = 4 * n; x < W - 4 * n; ++x) {
                    const double e = std::abs(double(out[y * W + x]) - double(truth[y * W + x]));
                    err += e;
                    maxErr = std::max(maxErr, e);
                    if (cfaChannel(order, x, y) == cfaChannel(order, x / n, y / n) && cfaChannel(order, x, y) != 1u)
                        ownKept = ownKept && out[y * W + x] == mosaic[y * W + x];
                }
            err /= double((W - 8 * n) * (H - 8 * n));
            char what[96];
            std::snprintf(what, sizeof what, "n=%u order=%u quality=%u mean error %.2f DN (max %.1f)", n, order,
                          unsigned(quality), err, maxErr);
            // Fast interpolates block means: softer by design.
            const double meanLimit = quality == CfaRemosaicQuality::Fast ? (n == 2 ? 6.0 : 14.0) : (n == 2 ? 2.0 : 5.0);
            const double maxLimit = quality == CfaRemosaicQuality::Fast ? (n == 2 ? 40.0 : 80.0) : (n == 2 ? 20.0 : 45.0);
            check(err < meanLimit && maxErr < maxLimit, what);
            check(ownKept, "R/B at their own sites keep the raw value");
            // Threads change nothing.
            auto single = mosaic;
            remosaicBlockCfa(single.data(), W, H, n, order, 1, quality);
            check(single == out, "threaded result matches single-threaded");
          }
        }
    }
    // Dark fringe: a bright saturated green leaf beside a dark grey wall. R/B
    // on the wall next to the edge must not be dragged towards black by the
    // leaf's colour difference (the dark green outline).
    for (const uint32_t n : {2u, 4u}) {
        const auto edge = [&](uint32_t c, uint32_t x, uint32_t y) {
            const bool leaf = (float(x) + 0.37f * float(y)) < 140.0f;
            const float leafRgb[3] = {380.f, 900.f, 300.f}, wallRgb[3] = {70.f, 75.f, 72.f};
            return 64.f + (leaf ? leafRgb[c] : wallRgb[c]);
        };
        std::vector<uint16_t> mosaic(W * H), truth(W * H);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                mosaic[y * W + x] = uint16_t(edge(cfaChannel(0, x / n, y / n), x, y));
                truth[y * W + x] = uint16_t(edge(cfaChannel(0, x, y), x, y));
            }
        for (const auto quality : {CfaRemosaicQuality::Standard, CfaRemosaicQuality::High}) {
            auto out = mosaic;
            remosaicBlockCfa(out.data(), W, H, n, 0, 1, quality);
            double sum = 0.0;
            int count = 0;
            for (uint32_t y = 16; y < H - 16; ++y)
                for (uint32_t x = 16; x < W - 16; ++x) {
                    const float d = float(x) + 0.37f * float(y) - 140.0f;
                    if (d < 1.0f || d > 6.0f || cfaChannel(0, x, y) == 1u) continue;
                    sum += std::max(0.0, double(truth[y * W + x]) - double(out[y * W + x]));
                    ++count;
                }
            char what[96];
            std::snprintf(what, sizeof what, "n=%u quality=%u dark fringe %.1f DN", n, unsigned(quality), sum / count);
            // Without green-similarity weights: 41 DN (quad), 75 DN (4x4).
            check(sum / count < (quality == CfaRemosaicQuality::High ? 1.0 : (n == 2 ? 3.0 : 6.0)), what);
        }
    }
    if (failures == 0) std::printf("cfa_remosaic_test: ok\n");
    return failures == 0 ? 0 : 1;
}
