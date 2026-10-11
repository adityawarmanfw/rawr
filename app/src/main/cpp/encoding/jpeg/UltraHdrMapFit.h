#pragma once
// Legacy brightness fit: adaptive gainmap ceiling + shoulder + chroma
// compression, applied in the JPEG writer on the GPU wide-range map (no
// second render). Header-only so host tests can include it directly.
// Mirrors the validated offline prototype (tmp/final_stack.py::remux_final)
// brightness baseline: fit = ceil((p99.9(mean log-gain) + margin) / step) * step clamped
// to [floor, min(displayCap, wideMax)]; shoulder y = knee + W*(1-exp(-(x-knee)/W))
// above knee = fitted - kneeBelow; chroma d' = T*tanh(d/T) around the
// per-pixel mean of shouldered channels.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "color/UltraHdrParams.h"

namespace rawrcam::encoding::jpeg {

// Fast tanh for the chroma knee: 4097-entry LUT over [-3, 3] with linear
// interpolation (max error ~1e-7, far below 1 LSB). std::tanh costs
// 30-100ns/call and the knee evaluates it on most map texels (12.5M at
// full res): measured 4.3s per file on device. LUT brings it under ~150ms.
inline float fastTanh(float x) {
    static float lut[4097];
    static const bool init = [] {
        for (int i = 0; i <= 4096; ++i) lut[i] = std::tanh(-3.0f + 6.0f * i / 4096.0f);
        return true;
    }();
    (void)init;
    const float t = std::clamp((x + 3.0f) * (4096.0f / 6.0f), 0.0f, 4096.0f);
    const int i = static_cast<int>(t);
    const float f = t - static_cast<float>(i);
    const float a = lut[i];
    const float b = lut[i < 4096 ? i + 1 : 4096];
    return a + (b - a) * f;
}

// Returns false (leave pixels + metadata wide) when the map is degenerate
// or gamma != 1 (codes wouldn't decode linear). base (RGBA8 sRGB, baseW x
// baseH) drives the luma baseline floor; nullptr skips it. Map texels sample
// the base at the nearest scaled pixel (identity for full-res maps).
inline bool fitAndRemapMap(const unsigned char* src, std::uint32_t w, std::uint32_t h,
                           const color::UltraHdrParams& uh, std::vector<unsigned char>& dst, float& fittedMax,
                           const unsigned char* base = nullptr, std::uint32_t baseW = 0, std::uint32_t baseH = 0) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const float wideMax = uh.gainMapMaxLog2;
    if (n == 0 || wideMax <= 0.0f || uh.gamma != 1.0f) return false;
    // p99.9 of per-pixel channel-mean log-gain via a 2048-bin histogram
    // (~0.0023 EV bins at wide 4.7: exact enough, single pass, no 50MB sort).
    constexpr int kBins = 2048;
    std::vector<std::uint32_t> hist(kBins, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const float mean = (static_cast<float>(src[i * 4 + 0]) + static_cast<float>(src[i * 4 + 1]) +
                            static_cast<float>(src[i * 4 + 2])) /
                           765.0f * wideMax;
        int b = static_cast<int>(mean / wideMax * kBins);
        hist[std::clamp(b, 0, kBins - 1)]++;
    }
    const std::uint64_t target = static_cast<std::uint64_t>(static_cast<double>(n) * 0.999);
    std::uint64_t acc = 0;
    float p999 = 0.0f;
    for (int b = 0; b < kBins; ++b) {
        acc += hist[b];
        if (acc >= target) {
            p999 = (static_cast<float>(b) + 1.0f) / kBins * wideMax;
            break;
        }
    }
    if (!(p999 >= 0.0f) || !(p999 <= wideMax)) return false;
    float fitted = std::ceil((p999 + uh.gainMapFitMargin) / uh.gainMapFitStep) * uh.gainMapFitStep;
    fitted = std::clamp(fitted, uh.gainMapFitFloor, std::min(uh.gainMapDisplayCap, wideMax));
    fittedMax = fitted;
    const float knee = fitted - uh.shoulderKneeBelowMax;
    const float W = uh.shoulderWidth;
    const float T = uh.chromaKnee;
    // Luma baseline floor (UltraHdrParams::baseShare): sRGB EOTF via a
    // 256-entry LUT, Rec.709 luma, then fitted * share * Y^exp. Max with
    // the measured channels before the shoulder, so it never lowers a code.
    const bool useFloor = base != nullptr && baseW > 0 && baseH > 0 && uh.baseShare > 0.0f;
    float eotf[256];
    for (int i = 0; i < 256; ++i) {
        const float c = static_cast<float>(i) / 255.0f;
        eotf[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    const float floorScale = fitted * uh.baseShare;
    const float floorExp = uh.baseExponent;
    dst.resize(n * 4);
    const float inv255 = 1.0f / 255.0f;
    (void)fastTanh(0.0f);  // force LUT init once, outside the threaded loop
    const std::uint32_t h32 = h;
    unsigned workers = std::thread::hardware_concurrency();
    if (workers < 1) workers = 1;
    if (workers > 4) workers = 4;
    if (n < 4096) workers = 1;
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned t = 0; t < workers; ++t) {
        const std::uint32_t y0 = h32 * t / workers;
        const std::uint32_t y1 = h32 * (t + 1) / workers;
        threads.emplace_back([=, &src, &dst, &eotf] {
            for (std::uint32_t y = y0; y < y1; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * w;
                const std::uint32_t by = useFloor ? std::min(baseH - 1, static_cast<std::uint32_t>(
                                                                            std::uint64_t(y) * baseH / h))
                                                  : 0;
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::size_t i = row + x;
                    float c[3];
                    for (int k = 0; k < 3; ++k) c[k] = static_cast<float>(src[i * 4 + k]) * inv255 * wideMax;
                    if (useFloor) {
                        const std::uint32_t bx =
                            std::min(baseW - 1, static_cast<std::uint32_t>(std::uint64_t(x) * baseW / w));
                        const unsigned char* p = base + (static_cast<std::size_t>(by) * baseW + bx) * 4;
                        const float luma = 0.2126f * eotf[p[0]] + 0.7152f * eotf[p[1]] + 0.0722f * eotf[p[2]];
                        const float fl = luma > 0.0f ? floorScale * std::pow(luma, floorExp) : 0.0f;
                        for (int k = 0; k < 3; ++k) c[k] = std::max(c[k], fl);
                    }
                    // Shoulder each channel, then average: mean of shouldered
                    // channels, not shoulder of the mean.
                    float ms = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        c[k] = (c[k] > knee) ? knee + W * (1.0f - std::exp(-(c[k] - knee) / W)) : c[k];
                        ms += c[k];
                    }
                    ms /= 3.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float d = (c[k] - ms) / T;
                        float dk;
                        if (d > 3.0f) {
                            dk = T;
                        } else if (d < -3.0f) {
                            dk = -T;
                        } else if (d > -0.02f && d < 0.02f) {
                            dk = T * d;  // tanh(x) ~= x: skip the call, exact to 1e-3 LSB
                        } else {
                            dk = T * fastTanh(d);
                        }
                        const float yy = ms + dk;
                        const float q = std::round(std::clamp(yy / fitted, 0.0f, 1.0f) * 255.0f);
                        dst[i * 4 + k] = static_cast<unsigned char>(q);
                    }
                    dst[i * 4 + 3] = src[i * 4 + 3];
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    return true;
}

// Restore capture highlight chromaticity at the already-fitted luminance.
// The capture colour is highlight recovery's output (the HDR tap); this only
// carries it through an SDR base that rendered it white.
// The SDR base and the brightness fit are immutable. Negative per-channel
// gains are necessary to restore color through a pale/white SDR highlight;
// they do not mean that shadow or midtone exposure is being lowered.
inline bool restoreHighlightColor(std::vector<unsigned char>& map, const unsigned char* chromaticity,
                                  uint32_t w, uint32_t h, const unsigned char* base,
                                  uint32_t baseW, uint32_t baseH, const color::UltraHdrParams& uh,
                                  float fittedMax, float& encodedMin, float& encodedMax) {
    const size_t n = size_t(w) * h;
    if (!chromaticity || !base || !baseW || !baseH || !n || map.size() != n * 4 ||
        !(fittedMax > 0) || !(uh.offsetSdr > 0) || !(uh.offsetHdr > 0) || uh.gamma != 1) return false;
    float linear[256], boost[256];
    for (int i = 0; i < 256; ++i) {
        const float c = float(i) / 255;
        linear[i] = c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
        boost[i] = std::exp2(float(i) / 255 * fittedMax);
    }
    const auto gains = [&](uint32_t x, uint32_t y, float* g) {
        const size_t i = size_t(y) * w + x;
        const uint32_t bx = std::min(baseW - 1, uint32_t(uint64_t(x) * baseW / w));
        const uint32_t by = std::min(baseH - 1, uint32_t(uint64_t(y) * baseH / h));
        const auto* b = base + (size_t(by) * baseW + bx) * 4;
        float s[3], original[3];
        for (int k = 0; k < 3; ++k) {
            s[k] = linear[b[k]];
            g[k] = float(map[i * 4 + k]) / 255 * fittedMax;
            original[k] = (s[k] + uh.offsetSdr) * boost[map[i * 4 + k]] - uh.offsetHdr;
        }
        // Restore only where the SDR lost colour: channels pressed against
        // white. A brightness ramp would also recolour well-exposed SDR
        // highlights and draw a ring at the ramp's onset.
        const float smax = std::max(s[0], std::max(s[1], s[2]));
        float t = std::clamp((smax - .75f) / .20f, 0.0f, 1.0f);
        t = t * t * (3 - 2 * t);
        if (t == 0) return;
        const auto* c = chromaticity + i * 4;
        const unsigned r = unsigned(c[0]) | (unsigned(c[1]) << 8);
        const unsigned green = unsigned(c[2]) | (unsigned(c[3]) << 8);
        if (r == 65535 && green == 65535) return;  // invalid/black tap
        float color[3] = {float(r) / 65535, float(green) / 65535,
                          std::max(0.0f, 1 - float(r + green) / 65535)};
        const float cy = .2126f * color[0] + .7152f * color[1] + .0722f * color[2];
        const float oy = .2126f * original[0] + .7152f * original[1] + .0722f * original[2];
        if (!(cy > 1e-8f) || !(oy > 0)) return;
        for (int k = 0; k < 3; ++k) {
            const float target = original[k] * (1 - t) + color[k] * (oy / cy) * t;
            g[k] = std::log2((std::max(target, 0.0f) + uh.offsetHdr) / (s[k] + uh.offsetSdr));
        }
    };
    // Two bounded-memory passes. First find the signed encoding range;
    // then quantize. Each worker only reads/writes its own rows.
    const unsigned workers = n < 4096 ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    std::vector<float> low(workers, 0), high(workers, fittedMax);
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            for (uint32_t y = h * worker / workers; y < h * (worker + 1) / workers; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    float g[3]; gains(x, y, g);
                    for (float v : g) { low[worker] = std::min(low[worker], v); high[worker] = std::max(high[worker], v); }
                }
        });
    }
    for (auto& thread : threads) thread.join();
    encodedMin = std::floor(*std::min_element(low.begin(), low.end()) * 4) / 4;
    encodedMax = std::ceil(*std::max_element(high.begin(), high.end()) * 4) / 4;
    const float scale = 255 / (encodedMax - encodedMin);
    threads.clear();
    for (unsigned worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            for (uint32_t y = h * worker / workers; y < h * (worker + 1) / workers; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    float g[3]; gains(x, y, g);
                    const size_t i = size_t(y) * w + x;
                    for (int k = 0; k < 3; ++k)
                        map[i * 4 + k] = static_cast<unsigned char>(std::round(std::clamp((g[k] - encodedMin) * scale, 0.0f, 255.0f)));
                }
        });
    }
    for (auto& thread : threads) thread.join();
    return true;
}

}  // namespace rawrcam::encoding::jpeg
