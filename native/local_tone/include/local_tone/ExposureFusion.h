#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include "rawr/vk/GpuContext.h"

// Exposure-fusion local tone mapping after Bart Wronski, "Exposure Fusion -
// local tonemapping for real-time rendering" (2022), whose WebGL demo is
// public domain; the method is Mertens, Kautz and Van Reeth (2007).
//
// Three synthetic exposures of the scene-linear image (highlights, midtones,
// shadows) go through a global filmic curve to a perceptual lightness; each
// is weighted by how close it sits to 0.5; the lightness is blended across a
// Laplacian pyramid from a coarsest level up to quarter resolution; a 3x3
// guided upsample turns the blend into a per-pixel gain applied in place to
// the scene-linear image (before the real tone curve). The curve and
// constants follow the demo: three.js r138 ACES fit, sqrt lightness, luma
// weights (0.1, 0.7, 0.2) on Rec.709 primaries.
namespace rawr::local_tone {

struct ImageRef {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // RGBA16F, GENERAL layout, storage usage
};

// App defaults (tuned on device captures with AgX: shadows +2.5 EV).
inline constexpr float kDefaultShadowsEv = 2.5f;
inline constexpr float kDefaultHighlightsEv = 2.0f;
inline constexpr float kMinShadowsEv = 1.0f, kMaxShadowsEv = 4.0f;

// The lift brings dark regions toward mid grey, where their noise shows. It
// is limited so that the darkest region lifted to mid grey keeps a per-pixel
// SNR of at least kMinLiftedSnr, from the frame's sensor noise model
// (variance = S x + O, x = signal / white) over the merged frame count,
// at the render gain (sensor signal -> rendered linear). Never below
// kMinShadowsEv; an unknown noise model leaves the lift as set.
inline constexpr float kMinLiftedSnr = 1.0f;
inline constexpr float kMidGrey = 0.18f;
struct LiftNoise {
    float shot = 0.0f, read = 0.0f;  // S, O (normalized to white)
    float renderGain = 1.0f;
    std::uint32_t frames = 1;
};
inline float noiseLimitedShadowsEv(float shadowsEv, const LiftNoise& n) noexcept {
    if (!(shadowsEv > 0.0f) || !(n.shot >= 0.0f) || !(n.read >= 0.0f) || !(n.shot + n.read > 0.0f) ||
        !(n.renderGain > 0.0f) || !std::isfinite(n.shot + n.read + n.renderGain))
        return shadowsEv;
    const float t2 = kMinLiftedSnr * kMinLiftedSnr;
    const float frames = float(std::max<std::uint32_t>(1u, n.frames));
    // Smallest signal with x / sqrt((S x + O) / frames) >= kMinLiftedSnr.
    const float a = t2 * n.shot / frames;
    const float x = 0.5f * (a + std::sqrt(a * a + 4.0f * t2 * n.read / frames));
    const float maxLift = std::log2(kMidGrey / (n.renderGain * x));
    return std::clamp(maxLift, std::min(shadowsEv, kMinShadowsEv), shadowsEv);
}

// App tone parameters (tonemap::LocalToneParams) as fusion parameters; the
// rest stay at the demo's defaults. shadowsEv 0 = off.
inline bool fusionActive(float shadowsEv) noexcept { return std::isfinite(shadowsEv) && shadowsEv > 0.0f; }

// The blend runs from coarsestMip up to this mip (quarter resolution, the
// demo's "display_mip"); the guided upsample covers the rest.
inline constexpr std::uint32_t kFusionBaseMip = 2;
inline constexpr std::uint32_t kFusionMaxDownLevels = 7;

struct FusionParams {
    float exposure = 1.0f;      // scales the image before the lightness curve (demo "exposure")
    float shadowsEv = 1.5f;     // brighter synthetic exposure, EV over the midtones one
    float highlightsEv = 2.0f;  // darker synthetic exposure, EV under the midtones one
    float sigma = 5.0f;         // exposure preference sharpness around lightness 0.5
    std::uint32_t coarsestMip = 6;        // coarsest pyramid level, mips below full resolution (demo "mip")
    bool boostLocalContrast = false;      // weight Laplacians by their magnitude
};

inline FusionParams fusionParams(float shadowsEv, float highlightsEv) noexcept {
    FusionParams p{};
    p.shadowsEv = shadowsEv;
    p.highlightsEv = highlightsEv;
    return p;
}

inline bool validParams(const FusionParams& p) noexcept {
    return std::isfinite(p.exposure) && p.exposure > 0.0f && std::isfinite(p.shadowsEv) && p.shadowsEv >= 0.0f &&
           p.shadowsEv <= 6.0f && std::isfinite(p.highlightsEv) && p.highlightsEv >= 0.0f && p.highlightsEv <= 6.0f &&
           std::isfinite(p.sigma) && p.sigma > 0.0f && p.coarsestMip > kFusionBaseMip &&
           p.coarsestMip <= kFusionBaseMip + kFusionMaxDownLevels;
}

struct FusionRecordInfo {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0;
    ImageRef measure;  // same content as targets[0]
    std::array<ImageRef, 2> targets{};
    std::uint32_t targetCount = 1;
    std::array<float, 9> cameraToWorking{1, 0, 0, 0, 1, 0, 0, 0, 1};  // camera RGB -> AP1, column-major
    float renderGain = 1.0f;  // linear render gain (AE post-gain)
    FusionParams params;
    std::uint32_t slot = 0;
};

class ExposureFusionProcessor final {
   public:
    static constexpr std::uint32_t kMaxSlots = 4;
    ExposureFusionProcessor(const rawr::vk::GpuContext& gpu, std::uint32_t slots = 1);
    ~ExposureFusionProcessor();
    ExposureFusionProcessor(const ExposureFusionProcessor&) = delete;
    ExposureFusionProcessor& operator=(const ExposureFusionProcessor&) = delete;

    // Records the passes into info.commandBuffer: compute reads of `measure`
    // and read/write of the targets must already be visible (GENERAL layout);
    // ends with a compute write -> compute/transfer read barrier on every
    // target. Returns false (records nothing) for invalid input.
    bool record(const FusionRecordInfo& info);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rawr::local_tone
