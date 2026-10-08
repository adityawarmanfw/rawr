#pragma once
#include <algorithm>
#include <array>
#include <cmath>

#include "Types.hpp"

namespace rcd {
// Streaming counterpart of rcd_balance.comp for CPU-backed, tiled DNG imports.
// Feed <=8x8 blocks with even origins, in the established 0..255 CFA domain.
// The frame-wide result must be reused for every demosaic tile.
class BalanceAccumulator {
    double sum_[3]{}, count_[3]{};

   public:
    void addBlock(const float* data, uint32_t stride, uint32_t width, uint32_t height, BayerPattern pattern) {
        double sum[3]{}, count[3]{};
        const auto p = static_cast<uint32_t>(pattern);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                float v = data[y * stride + x] * (1.0f / 255.0f);
                if (!std::isfinite(v) || v <= .001f || v >= .987f) return;
                unsigned px = (x & 1u) ^ (p & 1u), py = (y & 1u) ^ ((p >> 1u) & 1u);
                unsigned c = px == py ? (px ? 2 : 0) : 1;
                sum[c] += v;
                count[c] += 1;
            }
        for (int c = 0; c < 3; ++c) {
            sum_[c] += sum[c];
            count_[c] += count[c];
        }
    }
    std::array<float, 3> gains() const {
        double mean[3]{};
        for (int c = 0; c < 3; ++c) {
            if (count_[c] <= 0) return {1, 1, 1};
            mean[c] = sum_[c] / count_[c];
            if (mean[c] <= 1e-6) return {1, 1, 1};
        }
        double lo = std::min({mean[0], mean[1], mean[2]});
        return {float(std::clamp(lo / mean[0], .05, 1.)), float(std::clamp(lo / mean[1], .05, 1.)),
                float(std::clamp(lo / mean[2], .05, 1.))};
    }
};
}  // namespace rcd
