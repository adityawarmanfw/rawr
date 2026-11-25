// Burst noise fit against a known noise model: synthetic block statistics (noise_estimate.comp layout) with the sample
// variance of n Gaussian differences at continuous brightness levels, plus blocks whose variance texture or misalignment
// raised. The fitted variance must match the true a x + b over the whole level range: within 3% on clean statistics; with
// textured blocks it may read up to 6% high (texture only adds variance), never more than 2% low. An underestimate makes
// the merges reject frames that differ only by noise.
#include "rawr/raw_gpu_pipeline/BurstNoiseEstimate.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using rawr::raw_gpu_pipeline::fitBurstNoise;
using rawr::raw_gpu_pipeline::kNoiseBlockFloats;

namespace {

struct Case {
    const char* name;
    double a, b;
    double texturedShare;  // blocks whose variance is raised by texture / misalignment
    bool discreteLevels;   // a few flat patches instead of a continuous spread
};

struct Range {
    double low = 0.0, high = 0.0;  // most negative and most positive relative error of the fitted variance
};

// Relative error range of the fitted variance over the level range, any site.
Range errorRange(const Case& c, unsigned seed) {
    constexpr std::uint32_t kBlocks = 3000, kSamples = 256;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::chi_squared_distribution<double> chi(kSamples - 1);
    std::exponential_distribution<double> texture(1.0 / 1.5);
    std::vector<float> stats(std::size_t(kBlocks) * kNoiseBlockFloats);
    for (std::uint32_t i = 0; i < kBlocks; ++i) {
        double u = unit(rng);
        if (c.discreteLevels) u = std::floor(u * 12.0) / 11.0;
        const double level = 0.002 * std::pow(0.8 / 0.002, u);
        for (int site = 0; site < 4; ++site) {
            double v = c.a * level + c.b;
            if (unit(rng) < c.texturedShare) v *= 1.0 + texture(rng);
            const double sampleVariance = 2.0 * v * chi(rng) / double(kSamples - 1);  // of the difference of two frames
            const double meanDiff = gauss(rng) * std::sqrt(2.0 * v / kSamples);
            const double meanLevel = level + gauss(rng) * std::sqrt(v / kSamples);
            float* s = stats.data() + std::size_t(i) * kNoiseBlockFloats + site * 4;
            s[0] = float(kSamples);
            s[1] = float(meanLevel * kSamples);
            s[2] = float(meanDiff * kSamples);
            s[3] = float(sampleVariance * (kSamples - 1) + meanDiff * meanDiff * kSamples);
        }
    }
    const auto fit = fitBurstNoise(stats.data(), kBlocks);
    assert(fit);
    Range r;
    for (int site = 0; site < 4; ++site)
        for (double level = 0.002; level <= 0.8; level *= 1.25) {
            const double got = fit->profile.slopeBySite[site] * level + fit->profile.offsetBySite[site];
            const double e = got / (c.a * level + c.b) - 1.0;
            r.low = std::min(r.low, e);
            r.high = std::max(r.high, e);
        }
    std::printf("  %-26s variance error %+5.1f%% .. %+5.1f%%  (site 0: a %.4g b %.4g, true %.4g %.4g)\n", c.name,
                100.0 * r.low, 100.0 * r.high, fit->profile.slopeBySite[0], fit->profile.offsetBySite[0], c.a, c.b);
    return r;
}

}  // namespace

int main() {
    const Case cases[] = {
        {"high ISO, flat", 1.5e-4, 1e-7, 0.0, false},
        {"low ISO, flat", 2e-5, 2e-8, 0.0, false},
        {"high ISO, 40% textured", 1.5e-4, 1e-7, 0.4, false},
        {"low ISO, 40% textured", 2e-5, 2e-8, 0.4, false},
        {"high ISO, 12 patches", 1.5e-4, 1e-7, 0.0, true},
        {"read-noise heavy", 3e-5, 2e-6, 0.2, false},
    };
    bool ok = true;
    for (const auto& c : cases) {
        Range worst;
        for (unsigned seed = 1; seed <= 3; ++seed) {
            const Range r = errorRange(c, seed);
            worst.low = std::min(worst.low, r.low);
            worst.high = std::max(worst.high, r.high);
        }
        const bool pass = c.texturedShare > 0.0 ? worst.low > -0.02 && worst.high < 0.06 : worst.low > -0.03 && worst.high < 0.03;
        if (!pass) std::printf("  ^ out of bounds\n");
        ok = ok && pass;
    }
    if (!ok) {
        std::printf("burst_noise_fit_test FAILED\n");
        return 1;
    }
    std::printf("burst_noise_fit_test passed\n");
    return 0;
}
