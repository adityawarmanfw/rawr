#include "rawr/raw_gpu_pipeline/BurstNoiseEstimate.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rawr::raw_gpu_pipeline {
namespace {
constexpr std::uint32_t kMinSamplesPerBlock = 128u;
constexpr std::uint32_t kBins = 24u;
constexpr std::size_t kMinBlocksPerBin = 24u;
constexpr std::uint32_t kMinBins = 4u;
// Low quantile of the per-block variances within a brightness bin.
constexpr double kQuantile = 0.2;
constexpr double kQuantileZ = -0.8416;  // standard normal quantile for 0.2
constexpr double kMinOffset = 1e-10;

struct Sample {
    double level;
    double variance;
    double count;
};

struct Point {
    double level;
    double variance;
};

// Relative-error weighted least squares for variance = a * x + b.
bool fitLine(const std::vector<Point>& points, double& a, double& b) {
    double s00 = 0, s01 = 0, s11 = 0, t0 = 0, t1 = 0;
    for (const auto& p : points) {
        const double w = 1.0 / (p.variance * p.variance);
        s00 += w * p.level * p.level;
        s01 += w * p.level;
        s11 += w;
        t0 += w * p.level * p.variance;
        t1 += w * p.variance;
    }
    const double det = s00 * s11 - s01 * s01;
    if (!(std::abs(det) > 0.0)) return false;
    a = (t0 * s11 - s01 * t1) / det;
    b = (s00 * t1 - s01 * t0) / det;
    if (a < 0.0) {  // flat curve: offset only
        a = 0.0;
        b = t1 / s11;
    }
    if (b < kMinOffset) {  // shot-noise dominated: refit the slope through the floor
        b = kMinOffset;
        double num = 0, den = 0;
        for (const auto& p : points) {
            const double w = 1.0 / (p.variance * p.variance);
            num += w * p.level * (p.variance - b);
            den += w * p.level * p.level;
        }
        a = den > 0.0 ? std::max(0.0, num / den) : 0.0;
    }
    return std::isfinite(a) && std::isfinite(b) && (a > 0.0 || b > kMinOffset);
}
}  // namespace

std::optional<BurstNoiseFit> fitBurstNoise(const float* blockStats, std::uint32_t blockCount) {
    if (!blockStats || blockCount == 0u) return std::nullopt;
    BurstNoiseFit fit{};
    std::uint32_t minBins = kBins;
    for (std::uint32_t site = 0; site < 4u; ++site) {
        std::vector<Sample> samples;
        samples.reserve(blockCount);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            const float* s = blockStats + std::size_t(i) * kNoiseBlockFloats + site * 4u;
            const double n = s[0];
            if (n < kMinSamplesPerBlock) continue;
            const double level = s[1] / n;
            const double diffVariance = (s[3] - double(s[2]) * s[2] / n) / (n - 1.0);
            // Difference of two independent frames carries twice the variance.
            const double variance = 0.5 * diffVariance;
            if (!(std::isfinite(level) && std::isfinite(variance) && variance > 0.0) || level < 0.0) continue;
            samples.push_back({level, variance, n});
        }
        if (site == 0u) fit.usableBlocks = std::uint32_t(samples.size());
        if (samples.size() < kMinBlocksPerBin * kMinBins) return std::nullopt;
        std::sort(samples.begin(), samples.end(), [](const Sample& l, const Sample& r) { return l.level < r.level; });
        std::vector<Point> points;
        const std::size_t perBin = std::max<std::size_t>(kMinBlocksPerBin, samples.size() / kBins);
        for (std::size_t begin = 0; begin + kMinBlocksPerBin <= samples.size(); begin += perBin) {
            const std::size_t end = std::min(samples.size(), begin + perBin);
            std::vector<double> variances, levels;
            double count = 0.0;
            for (std::size_t i = begin; i < end; ++i) {
                variances.push_back(samples[i].variance);
                levels.push_back(samples[i].level);
                count += samples[i].count;
            }
            const std::size_t q = std::size_t(kQuantile * double(variances.size() - 1));
            std::nth_element(variances.begin(), variances.begin() + q, variances.end());
            std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
            // Sample variance of n samples ~ variance * chi2(n-1)/(n-1); its low
            // quantile sits below the mean by about z*sqrt(2/(n-1)).
            const double meanCount = count / double(end - begin);
            const double bias = 1.0 + kQuantileZ * std::sqrt(2.0 / std::max(meanCount - 1.0, 1.0));
            const double v = variances[q] / std::max(bias, 0.5);
            if (v > 0.0 && std::isfinite(v)) points.push_back({levels[levels.size() / 2], v});
        }
        if (points.size() < kMinBins) return std::nullopt;
        minBins = std::min<std::uint32_t>(minBins, std::uint32_t(points.size()));
        double a = 0.0, b = 0.0;
        if (!fitLine(points, a, b)) return std::nullopt;
        fit.profile.slopeBySite[site] = float(a);
        fit.profile.offsetBySite[site] = float(b);
    }
    fit.bins = minBins;
    if (!rawr::raw_merge_common::valid(fit.profile)) return std::nullopt;
    return fit;
}

}  // namespace rawr::raw_gpu_pipeline
