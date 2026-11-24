#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace rawrcam::pipeline {

// A radial gain map for HALs that report no Camera2 lens shading map. RAW carries the lens's natural falloff; the
// vendor camera hides it in its ISP, this does the same for the live preview. Gain is 1 at the centre and rises with
// the squared distance to kCornerGain at the corners. Layout matches ACAMERA_STATISTICS_LENS_SHADING_MAP: rows of
// width x (R, Geven, Godd, B).
struct SyntheticLensShading {
    static constexpr uint32_t kWidth = 17;
    static constexpr uint32_t kHeight = 13;
    static constexpr float kCornerGain = 1.5f;

    static const std::vector<float>& gains() {
        static const std::vector<float> map = [] {
            std::vector<float> g;
            g.reserve(static_cast<size_t>(kWidth) * kHeight * 4u);
            for (uint32_t y = 0; y < kHeight; ++y) {
                for (uint32_t x = 0; x < kWidth; ++x) {
                    const float dx = (static_cast<float>(x) / (kWidth - 1) - 0.5f) * 2.0f;
                    const float dy = (static_cast<float>(y) / (kHeight - 1) - 0.5f) * 2.0f;
                    const float r2 = (dx * dx + dy * dy) * 0.5f;  // 0 at the centre, 1 at the corners
                    const float gain = 1.0f + (kCornerGain - 1.0f) * r2;
                    for (int c = 0; c < 4; ++c) g.push_back(gain);
                }
            }
            return g;
        }();
        return map;
    }
};

}  // namespace rawrcam::pipeline
