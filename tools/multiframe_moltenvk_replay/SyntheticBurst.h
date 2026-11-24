#pragma once
#include <rawr/zsl_container/ZslContainer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>

namespace synthetic {
inline float halton(unsigned n, unsigned base) {
    float f = 1.f, result = 0.f;
    while (n) { f /= float(base); result += f * float(n % base); n /= base; }
    return result;
}
inline std::array<float, 3> scene(float x, float y, bool flat, float motion = 0.f) {
    if (flat) return {.3f, .5f, .2f};
    // Bandlimited grayscale texture, both diagonal edge signs, and colored
    // regions. Values stay inside sensor range so clipping cannot hide errors.
    float v;
    if (y < 256.f) {
        const float edge = x < 256.f ? x - .7f * y - 40.f : x + .7f * y - 450.f;
        v = .2f + .5f / (1.f + std::exp(-edge * 1.5f));
    } else {
        const float frequency = x < 256.f ? .12f : .28f;
        v = .45f + .16f * std::sin(6.28318530718f * frequency * (x + .23f * y));
    }
    if (x > 205.f + motion && x < 235.f + motion && y > 200.f && y < 310.f) return {.7f, .2f, .12f};
    return {v * .8f, v, v * .6f};
}
inline std::array<float, 3> sample(float x, float y, bool flat, float motion = 0.f) {
    std::array<float, 3> rgb{};
    // Sensor-pixel aperture shared by input and ground truth. We test sampling
    // reconstruction here, not recovery of frequencies removed by optics.
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
        const auto v = scene(x + (float(i) + .5f) / 4.f - .5f,
                             y + (float(j) + .5f) / 4.f - .5f, flat, motion);
        for (int c = 0; c < 3; ++c) rgb[c] += v[c] / 16.f;
    }
    return rgb;
}
// synthetic:dark: a flat deep-shadow field (2048 x 1536, RGGB, 10-bit codes widened x16 like a merged 14-bit range)
// with shot + read noise at the X300U ISO 3200 burst fit (var = A x + B, normalized). Truth is the constant kDarkRgb,
// so everything in the merged output that is not kDarkRgb is noise: per-scale noise of the merge vs one frame.
inline constexpr std::array<float, 3> kDarkRgb{.0030f, .0045f, .0022f};
inline constexpr float kDarkShot = 1.5e-4f, kDarkRead = 1.0e-7f;
inline rawr::zsl_container::Bundle darkBurst() {
    constexpr unsigned W = 2048, H = 1536;
    constexpr float black = 1024.f, white = 16383.f;
    rawr::zsl_container::Bundle out{};
    for (unsigned n = 0; n < 30; ++n) {
        rawr::zsl_container::PackedFrame f{};
        f.width = W; f.height = H; f.frameId = n + 1; f.timestampNs = 1000000000ull + n * 33333333ull;
        // The true noise model as a resolved replay header (a flat field gives the burst fit only one level).
        std::ostringstream model;
        model.precision(9);
        model << "rawrReplayNoiseV1\t1 " << kDarkShot << ' ' << kDarkRead;
        for (int k = 0; k < 2; ++k) {  // low and high signal profiles alike
            for (int s = 0; s < 4; ++s) model << ' ' << kDarkShot;
            for (int s = 0; s < 4; ++s) model << ' ' << kDarkRead;
        }
        for (int s = 0; s < 4; ++s) model << " 0";
        f.metadata = "effectiveWhiteLevel\t16383\nblackLevelPhysicalRggb\t1024,1024,1024,1024\n" + model.str() + '\n';
        std::vector<uint16_t> raw(std::size_t(W) * H);
        std::mt19937 rng(7919u + n);
        std::normal_distribution<float> unit(0.f, 1.f);
        for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
            const int c = !(y & 1) && !(x & 1) ? 0 : (y & 1) && (x & 1) ? 2 : 1;
            const float v = kDarkRgb[c] + std::sqrt(kDarkShot * kDarkRgb[c] + kDarkRead) * unit(rng);
            raw[std::size_t(y) * W + x] = uint16_t(std::clamp(std::lround(black + v * (white - black)), 0l, 65535l));
        }
        f.gpuPacket.resize(raw.size() * 2); std::memcpy(f.gpuPacket.data(), raw.data(), f.gpuPacket.size());
        out.frames.push_back(std::move(f));
    }
    return out;
}
inline rawr::zsl_container::Bundle burst(const std::string& name) {
    if (name == "synthetic:dark") return darkBurst();
    // NOTE: synthetic metadata carries only black/white levels, no
    // sensorNoiseProfile / replay-noise header. Replay with
    // --noise-source recorded therefore falls back to legacy
    // (noiseAlpha/noiseBeta defaults); prefer --noise-source legacy for
    // synthetic runs so the fallback is explicit. Synthetic pixel noise
    // N(0,0.002) is intentionally not matched to any sensor profile: these
    // bursts test sampling/alignment reconstruction, not noise calibration.
    if (name != "synthetic:shifted" && name != "synthetic:static" && name != "synthetic:flat" && name != "synthetic:motion")
        throw std::invalid_argument("unknown synthetic scene");
    rawr::zsl_container::Bundle out{};
    for (unsigned n = 0; n < 30; ++n) {
        rawr::zsl_container::PackedFrame f{};
        f.width = f.height = 512; f.frameId = n + 1; f.timestampNs = 1000000000ull + n * 33333333ull;
        f.metadata = "effectiveWhiteLevel\t16383\nblackLevelPhysicalRggb\t64,64,64,64\n";
        std::vector<uint16_t> raw(512 * 512);
        std::mt19937 rng(391u + n);
        std::normal_distribution<float> noise(0.f, .002f);
        const bool shifted = name != "synthetic:static" && n != 15;
        const float dx = shifted ? (halton(n + 1, 2) - .5f) * 3.f : 0.f;
        const float dy = shifted ? (halton(n + 1, 3) - .5f) * 3.f : 0.f;
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x) {
            auto v = sample(float(x) - dx, float(y) - dy, name == "synthetic:flat",
                            name == "synthetic:motion" ? float(int(n) - 15) * 2.f : 0.f);
            const int c = !(y & 1) && !(x & 1) ? 0 : (y & 1) && (x & 1) ? 2 : 1;
            raw[y * 512 + x] = uint16_t(std::lround(64.f + std::clamp(v[c] + noise(rng), 0.f, 1.f) * 16319.f));
        }
        f.gpuPacket.resize(raw.size() * 2); std::memcpy(f.gpuPacket.data(), raw.data(), f.gpuPacket.size());
        out.frames.push_back(std::move(f));
    }
    return out;
}
}  // namespace synthetic
