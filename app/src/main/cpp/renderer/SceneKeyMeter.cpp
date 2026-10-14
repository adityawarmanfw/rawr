#include "SceneKeyMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "color/AutoKeyExposure.h"

namespace rawrcam::renderer {

std::optional<SceneKey> measureSceneKey(DngSource& source, bool shading, const std::array<float, 3>& whiteBalance,
                                        const std::array<float, 9>& cameraToAp1ColumnMajor) {
    using namespace rawrcam::color;
    constexpr std::array<float, 3> kAp1Luma{0.2722287168f, 0.6740817658f, 0.0536895174f};
    std::array<float, 3> lumaRow{};
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r) lumaRow[c] += kAp1Luma[r] * cameraToAp1ColumnMajor[c * 3 + r];
    float whiteLuma = 0.0f;
    for (size_t c = 0; c < 3; ++c) whiteLuma += lumaRow[c] * whiteBalance[c];
    const float clipLuma = 0.9f * whiteLuma;

    // The camera meters its half-resolution preview in 4-pixel blocks, about
    // 510 samples across; match that density with even (CFA-aligned) blocks.
    const uint32_t x0 = source.crop[0] & ~1u, y0 = source.crop[1] & ~1u;
    const uint32_t w = source.crop[2] & ~1u, h = source.crop[3] & ~1u;
    if (w < 2 || h < 2) return std::nullopt;
    const uint32_t block = std::max(2u, (w / 510u) & ~1u);
    const uint32_t blocksX = w / block, blocksY = h / block;
    const auto& pattern = source.info().cfa.pattern;
    const float binsPerEv = float(kSceneKeyBins) / (kSceneKeyMaxEv - kSceneKeyMinEv);
    std::array<std::uint32_t, kSceneKeyBins> bins{};
    for (uint32_t by = 0; by < blocksY; ++by) {
        // Centre 2x2 cell of the block, as one preview pixel.
        const uint32_t sy = y0 + by * block + ((block / 2u) & ~1u);
        const auto rows = source.read(x0, sy, w, 2);
        for (uint32_t bx = 0; bx < blocksX; ++bx) {
            const uint32_t lx = bx * block + ((block / 2u) & ~1u);
            float rgb[3]{};
            for (uint32_t yy = 0; yy < 2; ++yy)
                for (uint32_t xx = 0; xx < 2; ++xx) {
                    const uint32_t sx = x0 + lx + xx, syy = sy + yy;
                    const int c = pattern[((syy & 1u) << 1) | (sx & 1u)];
                    const float v = source.normalizedSample(rows[size_t(yy) * w + lx + xx], sx, syy, shading) / 255.0f;
                    rgb[c] += c == 1 ? 0.5f * v : v;
                }
            float luma = 0.0f;
            for (int c = 0; c < 3; ++c) luma += lumaRow[c] * std::max(rgb[c] * whiteBalance[c], 0.0f);
            const bool centre = 4u * bx >= blocksX && 4u * bx < 3u * blocksX && 4u * by >= blocksY &&
                                4u * by < 3u * blocksY;
            uint32_t bin = 0;
            if (clipLuma > 0.0f && luma >= clipLuma) {
                bin = kSceneKeyBins - 1u;
            } else if (luma > 0.0f && std::isfinite(luma)) {
                bin = uint32_t(std::clamp((std::log2(luma) - kSceneKeyMinEv) * binsPerEv, 0.0f,
                                          float(kSceneKeyBins - 2u)));
            }
            bins[bin] += centre ? 3u : 1u;
        }
    }
    const auto key = sceneKeyEv(bins.data());
    const auto high = scenePercentileEv(bins.data(), 0.95f);
    if (!key || !high) return std::nullopt;
    return SceneKey{*key, *high};
}

}  // namespace rawrcam::renderer
