// Block CFA remosaic (imaging/CfaRemosaic.h): a smooth colour scene sampled as
// a quad / 4x4 mosaic must come back close to the same scene sampled as
// Bayer, in every colour order, and Bayer sites of the block's own colour
// must keep their raw value.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "capture/StripRemosaic.h"
#include "imaging/CfaRemosaic.h"
#include "imaging/QuadDisparity.h"
#include "imaging/QuadGainField.h"

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
    // A flat color field with quarter-DN channel values must retain its
    // fractional signal on measured and reconstructed sites, in every order.
    for (uint32_t order = 0; order < 4; ++order) {
        constexpr uint32_t side = 64;
        const uint16_t levels[3] = {401, 802, 603};
        std::vector<uint16_t> input(side * side);
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x)
                input[y * side + x] = levels[cfaChannel(order, x / 2, y / 2)];
        for (auto q : {CfaRemosaicQuality::Standard, CfaRemosaicQuality::High, CfaRemosaicQuality::Detail}) {
            auto out = input;
            remosaicBlockCfa(out.data(), side, side, 2, order, 2, q, 4.0f);
            bool exact = true;
            for (uint32_t y = 12; y < side - 12; ++y)
                for (uint32_t x = 12; x < side - 12; ++x)
                    exact &= out[y * side + x] == levels[cfaChannel(order, x, y)];
            check(exact, "quarter-DN constant-color field reconstructed exactly");
        }
        // Detail: a linear ramp is reproduced by the cubic green estimate (its
        // end slopes equal the ramp slope), and the clamp keeps a step inside
        // its end values.
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x) input[y * side + x] = uint16_t(400 + 6 * x + 3 * y);
        for (uint32_t y = 8; y < side - 8; y += 3)
            for (uint32_t x = 8; x < side - 8; x += 3) {
                const float g = CfaRemosaicGreen::at(input.data(), side, side, 2, order, x, y, 1.0f, true);
                check(std::abs(g - float(input[y * side + x])) < 0.01f, "cubic green reproduces a ramp");
            }
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x) input[y * side + x] = x < 33 ? 200 : 800;
        for (uint32_t y = 8; y < side - 8; y += 3)
            for (uint32_t x = 8; x < side - 8; x += 3) {
                const float g = CfaRemosaicGreen::at(input.data(), side, side, 2, order, x, y, 1.0f, true);
                check(g >= 199.99f && g <= 800.01f, "cubic green does not overshoot a step");
            }
        // Green estimate is homogeneous when DN constants scale with input.
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x)
                input[y * side + x] = uint16_t(64 + (x * 37 + y * 19) % 900);
        auto scaled = input;
        for (auto& v : scaled) v *= 4;
        for (uint32_t y = 8; y < side - 8; y += 3)
            for (uint32_t x = 8; x < side - 8; x += 3) {
                float a = CfaRemosaicGreen::at(input.data(), side, side, 2, order, x, y);
                float b = CfaRemosaicGreen::at(scaled.data(), side, side, 2, order, x, y, 4.0f);
                check(std::abs(a - b / 4) < 0.001f, "fixed-point green weights retain DN scale");
            }
    }
    // Detail's colour line: a two-colour edge with blended pixels. The red/blue of a blended pixel lie on the line
    // between the two colours (slope != 1 against green), which a colour difference cannot follow.
    {
        constexpr uint32_t es = 96;
        const float yel[3] = {330.0f, 300.0f, 40.0f}, drk[3] = {12.0f, 12.0f, 14.0f};
        const auto colour = [&](uint32_t c, uint32_t x, uint32_t y) {
            const float edgeAt = 30.0f + 0.31f * float(y);  // slanted edge, 2 px soft transition
            const float a = std::clamp((float(x) - edgeAt) / 2.0f + 0.5f, 0.0f, 1.0f);
            return 64.0f + yel[c] * (1.0f - a) + drk[c] * a;
        };
        for (uint32_t order : {0u, 3u}) {
            std::vector<uint16_t> mosaic(es * es), truth(es * es);
            for (uint32_t y = 0; y < es; ++y)
                for (uint32_t x = 0; x < es; ++x) {
                    mosaic[y * es + x] = uint16_t(std::lround(colour(cfaChannel(order, x / 2, y / 2), x, y)));
                    truth[y * es + x] = uint16_t(std::lround(colour(cfaChannel(order, x, y), x, y)));
                }
            auto out = mosaic;
            remosaicBlockCfa(out.data(), es, es, 2, order, 2, CfaRemosaicQuality::Detail, 1.0f, 64.0f);
            double e = 0.0, worst = 0.0;
            for (uint32_t y = 8; y < es - 8; ++y)
                for (uint32_t x = 8; x < es - 8; ++x) {
                    const double d = std::abs(double(out[y * es + x]) - double(truth[y * es + x]));
                    e += d;
                    worst = std::max(worst, d);
                }
            e /= double((es - 16) * (es - 16));
            char what[96];
            std::snprintf(what, sizeof what, "two-colour edge order=%u: mean error %.2f DN (max %.1f)", order, e,
                          worst);
            check(e < 1.0, what);
        }
    }
    // Detail fits red/blue to block moments over a wide window: threads change nothing, a flat colour field stays
    // flat, a noisy flat field has less chroma speckle than High, and a hard colour edge keeps its colour.
    {
        constexpr uint32_t side = 192;
        constexpr float kSlope = 0.2f, kOffset = 0.5f;
        const float level[3] = {180.0f, 420.0f, 260.0f};
        uint32_t state = 12345u;
        const auto noise = [&] {  // roughly unit Gaussian from four uniform bytes
            state = state * 1664525u + 1013904223u;
            return (float((state >> 24) + ((state >> 16) & 255u) + ((state >> 8) & 255u) + (state & 255u)) - 510.0f) / 147.8f;
        };
        for (uint32_t order : {0u, 3u}) {
            std::vector<uint16_t> flat(side * side), noisy(side * side);
            for (uint32_t y = 0; y < side; ++y)
                for (uint32_t x = 0; x < side; ++x) {
                    const float v = level[cfaChannel(order, x / 2, y / 2)];
                    flat[y * side + x] = uint16_t(std::lround(64.0f + v));
                    noisy[y * side + x] = uint16_t(std::lround(64.0f + v + noise() * std::sqrt(kSlope * v + kOffset)));
                }
            auto one = noisy, four = noisy, plain = noisy, field = flat;
            remosaicBlockCfa(one.data(), side, side, 2, order, 1, CfaRemosaicQuality::Detail, 1.0f, 64.0f);
            remosaicBlockCfa(four.data(), side, side, 2, order, 4, CfaRemosaicQuality::Detail, 1.0f, 64.0f);
            remosaicBlockCfa(plain.data(), side, side, 2, order, 4, CfaRemosaicQuality::High, 1.0f, 64.0f);
            remosaicBlockCfa(field.data(), side, side, 2, order, 2, CfaRemosaicQuality::Detail, 1.0f, 64.0f);
            check(one == four, "Detail: threaded result matches single-threaded");
            bool exact = true;
            for (uint32_t y = 16; y < side - 16; ++y)
                for (uint32_t x = 16; x < side - 16; ++x)
                    exact &= std::abs(int(field[y * side + x]) - int(std::lround(64.0f + level[cfaChannel(order, x, y)]))) <= 1;
            check(exact, "Detail keeps a flat colour field");
            // Chroma speckle: red minus the mean of its four green neighbours, spread over the interior.
            const auto chromaSpread = [&](const std::vector<uint16_t>& m) {
                double sum = 0.0, sum2 = 0.0;
                int n = 0;
                for (uint32_t y = 16; y < side - 16; ++y)
                    for (uint32_t x = 16; x < side - 16; ++x) {
                        if (cfaChannel(order, x, y) != 0u) continue;
                        const double g = 0.25 * (m[y * side + x - 1] + m[y * side + x + 1] + m[(y - 1) * side + x] + m[(y + 1) * side + x]);
                        const double d = double(m[y * side + x]) - g;
                        sum += d;
                        sum2 += d * d;
                        ++n;
                    }
                return std::sqrt(sum2 / n - (sum / n) * (sum / n));
            };
            char what[96];
            std::snprintf(what, sizeof what, "order=%u chroma speckle High %.2f, Detail %.2f DN", order, chromaSpread(plain), chromaSpread(one));
            std::printf("%s\n", what);
            check(chromaSpread(one) < chromaSpread(plain), what);
        }
        // A hard two-colour edge: Detail stays at least as close to the clean scene as High.
        for (uint32_t order : {0u, 3u}) {
            const auto edge = [&](uint32_t c, uint32_t x) {
                const float a[3] = {380.f, 900.f, 300.f}, b[3] = {80.f, 120.f, 700.f};
                return 64.0f + (x < side / 2 ? a[c] : b[c]);
            };
            std::vector<uint16_t> mosaic(side * side), truth(side * side);
            for (uint32_t y = 0; y < side; ++y)
                for (uint32_t x = 0; x < side; ++x) {
                    mosaic[y * side + x] = uint16_t(edge(cfaChannel(order, x / 2, y / 2), x));
                    truth[y * side + x] = uint16_t(edge(cfaChannel(order, x, y), x));
                }
            auto smoothed = mosaic, plain = mosaic;
            remosaicBlockCfa(smoothed.data(), side, side, 2, order, 2, CfaRemosaicQuality::Detail, 1.0f, 64.0f);
            remosaicBlockCfa(plain.data(), side, side, 2, order, 2, CfaRemosaicQuality::High, 1.0f, 64.0f);
            double es = 0.0, ep = 0.0;
            for (uint32_t y = 16; y < side - 16; ++y)
                for (uint32_t x = 16; x < side - 16; ++x) {
                    es += std::abs(double(smoothed[y * side + x]) - truth[y * side + x]);
                    ep += std::abs(double(plain[y * side + x]) - truth[y * side + x]);
                }
            char what[96];
            std::snprintf(what, sizeof what, "order=%u colour edge error Detail %.0f vs High %.0f", order, es, ep);
            std::printf("%s\n", what);
            check(es <= ep, what);
        }
    }
    // Strip loops (capture/StripRemosaic.h): the overlapped loop (two windows in flight, as the GPU runs it)
    // and the sequential one both match a whole-frame Detail run bit for bit.
    {
        constexpr uint32_t sw = 96;
        for (uint32_t sh : {1024u, 1000u, 520u, 136u})
            for (uint32_t strips : {2u, 3u, 4u, 6u}) {
                std::vector<uint8_t> frame(size_t(sw) * sh * 2);
                auto* px = reinterpret_cast<uint16_t*>(frame.data());
                uint32_t st = 777u + sh + strips;
                for (uint32_t y = 0; y < sh; ++y)
                    for (uint32_t x = 0; x < sw; ++x) {
                        st = st * 1664525u + 1013904223u;
                        px[size_t(y) * sw + x] = uint16_t(64 + (cfaChannel(0, x / 2, y / 2) == 1u ? 400 : 200) +
                                                          ((x / 9 + y / 7) % 2 ? 120 : 0) + (st >> 27));
                    }
                auto whole = frame;
                remosaicBlockCfa(whole, sw, sh, 2, 0, 1, CfaRemosaicQuality::Detail, 1.0f, 64.0f);
                const auto layout = rawrcam::capture::StripLayout::make(sh, strips);
                const auto windowPass = [&](uint8_t* window) {
                    remosaicBlockCfa(reinterpret_cast<uint16_t*>(window), sw, layout.window, 2, 0, 1,
                                     CfaRemosaicQuality::Detail, 1.0f, 64.0f);
                };
                std::vector<uint8_t> a(size_t(sw) * layout.window * 2), b(a.size());
                auto seq = frame;
                rawrcam::capture::remosaicInStrips(seq, sw, sh, layout, a.data(),
                                                   [&](uint8_t* w, uint32_t, uint32_t) { windowPass(w); });
                auto ovl = frame;
                uint8_t* const windows[2] = {a.data(), b.data()};
                bool pending[2] = {false, false};
                rawrcam::capture::remosaicInStripsOverlapped(
                    ovl, sw, sh, layout, windows,
                    [&](uint8_t*, uint32_t slot, uint32_t, uint32_t) {
                        check(!pending[slot], "overlapped strips: a window is reused only after its wait");
                        pending[slot] = true;
                    },
                    [&](uint32_t slot) {  // the "GPU" runs at wait time: the window must still hold its input
                        windowPass(windows[slot]);
                        pending[slot] = false;
                    });
                char what[96];
                std::snprintf(what, sizeof what, "strip loops match whole frame h=%u strips=%u", sh, strips);
                check(seq == whole && ovl == whole, what);
            }
    }
    {
        // Quad gain field: a smooth scene seen through a known per-position field (diagonal pairs opposite by
        // quadrant up to 5%, a row offset, a column tilt) is recognised, and dividing the fit out brings every
        // sample back to the field-free scene within a few codes. Without a field nothing is touched.
        constexpr uint32_t QW = 1536, QH = 1024;
        std::vector<uint16_t> clean(QW * QH), seen(QW * QH);
        for (uint32_t y = 0; y < QH; ++y)
            for (uint32_t x = 0; x < QW; ++x) {
                const float v = scene(cfaChannel(3, x / 2, y / 2), x / 3, y / 3) - 64.0f;
                const uint32_t p = (y & 1u) * 2u + (x & 1u);
                const float u = quadGainCoord(x, QW), w = quadGainCoord(y, QH);
                const float g = 1.0f + 0.05f * u * w * (p == 0u || p == 3u ? 1.0f : -1.0f) + (p < 2u ? 0.012f : -0.012f) +
                                0.02f * u * (p & 1u ? 1.0f : -1.0f);
                clean[y * QW + x] = uint16_t(std::lround(64.0f + v));
                seen[y * QW + x] = uint16_t(std::lround(64.0f + v * g));
            }
        const QuadGainField field = fitQuadGainField(seen.data(), QW, QH, 64.0f, 1023.0f, 4);
        check(field.active, "quad gain field: a 5% field is corrected");
        auto fixed = seen;
        applyQuadGainField(fixed.data(), QW, QH, 64.0f, field, 4);
        int before = 0, after = 0;
        for (size_t i = 0; i < clean.size(); ++i) {
            before = std::max(before, std::abs(int(seen[i]) - int(clean[i])));
            after = std::max(after, std::abs(int(fixed[i]) - int(clean[i])));
        }
        std::printf("quad gain field: deviation %.2f%%, max error %d -> %d codes\n", 100.0f * field.deviation, before,
                    after);
        // The local estimator is non-parametric (tile means, 32 x 24 nodes): within 4 codes (0.4%) here, where
        // the old quadratic fit, solving for the very form this scene was built from, reached 2.
        check(before > 15 && after <= 4, "quad gain field: dividing the fit out restores the scene");
        const QuadGainField none = fitQuadGainField(clean.data(), QW, QH, 64.0f, 1023.0f, 4);
        check(!none.active, "quad gain field: a matched sensor is left alone");
        auto untouched = clean;
        applyQuadGainField(untouched.data(), QW, QH, 64.0f, none, 4);
        check(untouched == clean, "quad gain field: inactive field changes nothing");
    }
    {
        // Quad gain field residual grid: a field the quadratic cannot bend to (the diagonal split grows with u^3,
        // up to 6% in the corners) is still restored within a few codes; without the grid it is not.
        constexpr uint32_t QW = 1536, QH = 1024;
        std::vector<uint16_t> clean(QW * QH), seen(QW * QH);
        for (uint32_t y = 0; y < QH; ++y)
            for (uint32_t x = 0; x < QW; ++x) {
                const float v = scene(cfaChannel(3, x / 2, y / 2), x / 3, y / 3) - 64.0f;
                const uint32_t p = (y & 1u) * 2u + (x & 1u);
                const float u = quadGainCoord(x, QW), w = quadGainCoord(y, QH);
                const float g = 1.0f + 0.06f * u * u * u * w * (p == 0u || p == 3u ? 1.0f : -1.0f);
                clean[y * QW + x] = uint16_t(std::lround(64.0f + v));
                seen[y * QW + x] = uint16_t(std::lround(64.0f + v * g));
            }
        QuadGainField field = fitQuadGainField(seen.data(), QW, QH, 64.0f, 1023.0f, 4);
        check(field.active, "quad gain grid: the field is corrected");
        auto fixed = seen, quadraticOnly = seen;
        applyQuadGainField(fixed.data(), QW, QH, 64.0f, field, 4);
        QuadGainField noGrid = field;
        noGrid.grid.fill(0.0f);
        applyQuadGainField(quadraticOnly.data(), QW, QH, 64.0f, noGrid, 4);
        double withGrid = 0.0, without = 0.0;
        for (size_t i = 0; i < clean.size(); ++i) {
            withGrid += std::abs(int(fixed[i]) - int(clean[i]));
            without += std::abs(int(quadraticOnly[i]) - int(clean[i]));
        }
        withGrid /= double(clean.size());
        without /= double(clean.size());
        std::printf("quad gain grid: mean error quadratic only %.2f, with grid %.2f codes\n", without, withGrid);
        check(withGrid < 0.5 * without, "quad gain grid: the grid takes what the quadratic leaves");
    }
    {
        // Quad phase-detect disparity: a soft edge seen by a defocused 2x2 on-chip lens (each in-block position
        // shifted 2.5 px along its side) splits every block on the edge; the correction brings the samples back
        // towards the in-focus image. An in-focus scene (no shift) with fine texture is left nearly untouched.
        constexpr uint32_t DW = 512, DH = 512;
        const auto edge = [](float x, float y) {
            return 100.0f + 500.0f * (0.5f + 0.5f * std::tanh((y - 256.0f + 0.3f * (x - 256.0f)) / 10.0f));
        };
        std::vector<uint16_t> truth(DW * DH), seen(DW * DH);
        for (uint32_t y = 0; y < DH; ++y)
            for (uint32_t x = 0; x < DW; ++x) {
                const float gain = cfaChannel(3, x / 2, y / 2) == 1u ? 1.0f : 0.6f;
                const float sx = float(x) + 0.5f + ((x & 1u) ? 2.5f : -2.5f), sy = float(y) + 0.5f + ((y & 1u) ? 2.5f : -2.5f);
                truth[y * DW + x] = uint16_t(std::lround(64.0f + gain * edge(float(x) + 0.5f, float(y) + 0.5f)));
                seen[y * DW + x] = uint16_t(std::lround(64.0f + gain * edge(sx, sy)));
            }
        auto fixed = seen;
        correctQuadDisparity(fixed.data(), DW, DH, 3, 64.0f, 0.0f, 0.0f, 4);
        double before = 0.0, after = 0.0;
        for (uint32_t y = 16; y < DH - 16; ++y)
            for (uint32_t x = 16; x < DW - 16; ++x) {
                before += std::abs(int(seen[y * DW + x]) - int(truth[y * DW + x]));
                after += std::abs(int(fixed[y * DW + x]) - int(truth[y * DW + x]));
            }
        // Fine in-focus textures alias on the 4-px same-colour lattice into gradients that correlate with the
        // split like a shift does; the texture gate must leave them alone.
        double changed = 0.0;
        for (int pattern = 0; pattern < 3; ++pattern) {
            std::vector<uint16_t> tex(DW * DH);
            for (uint32_t y = 0; y < DH; ++y)
                for (uint32_t x = 0; x < DW; ++x) {
                    const float gain = cfaChannel(3, x / 2, y / 2) == 1u ? 1.0f : 0.6f;
                    const float v = pattern == 0   ? std::sin(x * 1.9f) * std::cos(y * 1.3f)
                                    : pattern == 1 ? std::sin(x * 1.1f)
                                                   : std::sin(x * 0.7f + y * 0.4f);
                    tex[y * DW + x] = uint16_t(std::lround(64.0f + gain * (300.0f + 120.0f * v)));
                }
            auto out = tex;
            correctQuadDisparity(out.data(), DW, DH, 3, 64.0f, 0.0f, 0.25f, 4);
            double c = 0.0;
            for (size_t i = 0; i < out.size(); ++i) c += std::abs(int(out[i]) - int(tex[i]));
            changed = std::max(changed, c / double(out.size()));
        }
        std::printf("quad disparity: edge error %.0f -> %.0f, in-focus textures mean change %.3f codes (worst)\n", before,
                    after, changed);
        check(after * 3.0 < before, "quad disparity: a defocused edge's block split is corrected");
        check(changed < 0.05, "quad disparity: in-focus texture is kept");
    }
    if (failures == 0) std::printf("cfa_remosaic_test: ok\n");
    return failures == 0 ? 0 : 1;
}
