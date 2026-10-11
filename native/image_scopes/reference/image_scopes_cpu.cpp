#include "image_scopes_cpu.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace image_scopes::cpu {
namespace {
void validate(Rgba8ImageView im) {
    if (!im.data || !im.width || !im.height) throw std::invalid_argument("image_scopes CPU: invalid image");
    const std::uint64_t minStride = static_cast<std::uint64_t>(im.width) * 4u;
    if (im.rowStrideBytes < minStride) throw std::invalid_argument("image_scopes CPU: row stride too small");
    const std::uint64_t pixels = static_cast<std::uint64_t>(im.width) * im.height;
    if (pixels > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("image_scopes CPU: pixel count exceeds uint32_t");
}
inline std::uint8_t clampByte(std::int32_t v) noexcept { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }
inline std::uint32_t hash2(std::uint32_t x, std::uint32_t y) noexcept {
    std::uint32_t h = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu ^ 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
}  // namespace

std::uint8_t encodedLumaQ16(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    const std::uint32_t q = 13933u * r + 46871u * g + 4732u * b + 32768u;
    return static_cast<std::uint8_t>(q >> 16);
}

void bt709DerivedCbCrQ16(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t& cb, std::uint8_t& cr) noexcept {
    // BT.709-derived full-range Cb/Cr axes applied to encoded sRGB R'G'B'.
    const std::int32_t cbQ16 = -7509 * static_cast<std::int32_t>(r) - 25259 * static_cast<std::int32_t>(g) +
                               32768 * static_cast<std::int32_t>(b);
    const std::int32_t crQ16 = 32768 * static_cast<std::int32_t>(r) - 29763 * static_cast<std::int32_t>(g) -
                               3005 * static_cast<std::int32_t>(b);
    auto roundQ16 = [](std::int32_t q) noexcept { return q >= 0 ? (q + 32768) / 65536 : -((-q + 32768) / 65536); };
    cb = clampByte(128 + roundQ16(cbQ16));
    cr = clampByte(128 + roundQ16(crQ16));
}

bool sampleSelected(std::uint32_t x, std::uint32_t y, SamplingMode mode) noexcept {
    if (mode == SamplingMode::FullReference) return true;
    const std::uint32_t bx = x >> 1u, by = y >> 1u;
    const std::uint32_t lane = (y & 1u) * 2u + (x & 1u);
    const std::uint32_t h = hash2(bx, by);
    if (mode == SamplingMode::Production25) return lane == (h & 3u);
    // Exactly two lanes per complete 2x2 block; diagonal orientation hashes by block.
    const bool diag = (h & 1u) != 0u;
    return diag ? (lane == 0u || lane == 3u) : (lane == 1u || lane == 2u);
}

WaveformData measureDisplayWaveform(Rgba8ImageView im, WaveformMode mode, SamplingMode samplingMode,
                                    std::uint32_t horizontalBins, std::uint32_t signalBins) {
    validate(im);
    if (!horizontalBins || !signalBins || signalBins > 256u)
        throw std::invalid_argument("image_scopes CPU: invalid waveform grid");
    WaveformData out{};
    out.mode = mode;
    out.sampling.mode = samplingMode;
    out.sampling.sourcePixelCount = im.width * im.height;
    out.sampling.gridWidth = horizontalBins;
    out.sampling.gridHeight = signalBins;
    const std::size_t planes = mode == WaveformMode::RgbOverlay ? 3u : 1u;
    out.density.assign(static_cast<std::size_t>(horizontalBins) * signalBins * planes, 0u);
    for (std::uint32_t y = 0; y < im.height; ++y) {
        const auto* row = im.data + static_cast<std::size_t>(y) * im.rowStrideBytes;
        for (std::uint32_t x = 0; x < im.width; ++x) {
            if (!sampleSelected(x, y, samplingMode)) continue;
            ++out.sampling.sampledPixelCount;
            const auto* p = row + static_cast<std::size_t>(x) * 4u;
            const std::uint32_t xb =
                std::min(horizontalBins - 1u,
                         static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * horizontalBins) / im.width));
            auto sb = [signalBins](std::uint8_t v) {
                return std::min(signalBins - 1u,
                                static_cast<std::uint32_t>((static_cast<std::uint32_t>(v) * signalBins) / 256u));
            };
            if (mode == WaveformMode::Luma) {
                ++out.density[static_cast<std::size_t>(sb(encodedLumaQ16(p[0], p[1], p[2]))) * horizontalBins + xb];
            } else
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto base = c * static_cast<std::size_t>(horizontalBins) * signalBins;
                    ++out.density[base + static_cast<std::size_t>(sb(p[c])) * horizontalBins + xb];
                }
        }
    }
    out.sampling.samplingFraction = out.sampling.sourcePixelCount ? static_cast<float>(out.sampling.sampledPixelCount) /
                                                                        out.sampling.sourcePixelCount
                                                                  : 0.f;
    return out;
}

std::uint32_t sceneExposureBin(float luma) noexcept {
    if (!(luma > 0.0f)) return 0u;
    constexpr float binsPerEv = float(kSceneExposureBins) / (kSceneExposureMaxEv - kSceneExposureMinEv);
    const float t =
        std::clamp((std::log2(luma) - kSceneExposureMinEv) * binsPerEv, 0.0f, float(kSceneExposureBins - 2u));
    return static_cast<std::uint32_t>(t);
}

SceneExposureStatsData measureSceneExposure(RgbaF32ImageView im, const float lumaWeights[3], float clipLuma,
                                            std::uint32_t block) {
    if (!im.data || !im.width || !im.height) throw std::invalid_argument("image_scopes CPU: invalid scene image");
    if (im.rowStrideFloats < static_cast<std::uint64_t>(im.width) * 4u)
        throw std::invalid_argument("image_scopes CPU: scene row stride too small");
    if (block != 2u && block != 4u) throw std::invalid_argument("image_scopes CPU: scene block must be 2 or 4");
    SceneExposureStatsData out{};
    const std::uint32_t blocksX = (im.width + block - 1u) / block, blocksY = (im.height + block - 1u) / block;
    for (std::uint32_t by = 0; by < blocksY; ++by) {
        const std::uint32_t y0 = by * block, validH = std::min(block, im.height - y0);
        for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
            const std::uint32_t x0 = bx * block, validW = std::min(block, im.width - x0);
            const std::uint32_t lane = hash2(bx, by) % (validW * validH);
            const std::uint32_t x = x0 + lane % validW, y = y0 + lane / validW;
            const float* p = im.data + static_cast<std::size_t>(y) * im.rowStrideFloats + std::size_t(x) * 4u;
            float luma = 0.0f;
            for (int c = 0; c < 3; ++c) luma += lumaWeights[c] * std::max(p[c], 0.0f);
            if (std::isnan(luma)) luma = 0.0f;
            const bool centre = 4u * x >= im.width && 4u * x < 3u * im.width && 4u * y >= im.height &&
                                4u * y < 3u * im.height;
            const std::uint32_t w = centre ? kSceneExposureCentreWeight : 1u;
            const bool clipped = clipLuma > 0.0f && luma >= clipLuma;
            out.weightedBins[clipped ? kSceneExposureBins - 1u : sceneExposureBin(luma)] += w;
            out.totalWeight += w;
            ++out.sampleCount;
            if (clipped) out.clippedWeight += w;
        }
    }
    return out;
}

VectorscopeData measureVectorscope(Rgba8ImageView im, SamplingMode samplingMode, std::uint32_t bins) {
    validate(im);
    if (!bins || bins > 256u) throw std::invalid_argument("image_scopes CPU: invalid vectorscope grid");
    VectorscopeData out{};
    out.sampling.mode = samplingMode;
    out.sampling.sourcePixelCount = im.width * im.height;
    out.sampling.gridWidth = bins;
    out.sampling.gridHeight = bins;
    out.density.assign(static_cast<std::size_t>(bins) * bins, 0u);
    for (std::uint32_t y = 0; y < im.height; ++y) {
        const auto* row = im.data + static_cast<std::size_t>(y) * im.rowStrideBytes;
        for (std::uint32_t x = 0; x < im.width; ++x) {
            if (!sampleSelected(x, y, samplingMode)) continue;
            ++out.sampling.sampledPixelCount;
            const auto* p = row + static_cast<std::size_t>(x) * 4u;
            std::uint8_t cb = 0, cr = 0;
            bt709DerivedCbCrQ16(p[0], p[1], p[2], cb, cr);
            const std::uint32_t xb = std::min(bins - 1u, static_cast<std::uint32_t>(cb) * bins / 256u),
                                yb = std::min(bins - 1u, static_cast<std::uint32_t>(cr) * bins / 256u);
            ++out.density[static_cast<std::size_t>(yb) * bins + xb];
        }
    }
    out.sampling.samplingFraction = out.sampling.sourcePixelCount ? static_cast<float>(out.sampling.sampledPixelCount) /
                                                                        out.sampling.sourcePixelCount
                                                                  : 0.f;
    return out;
}

}  // namespace image_scopes::cpu
