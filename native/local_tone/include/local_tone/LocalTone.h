#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include "rawr/vk/GpuContext.h"

// Local tone mapping: a smooth regional gain on a scene-linear camera-RGB
// image, applied in place before the tone curve, film and gain map. Dark
// regions are lifted toward mid grey on an edge-aware (guided filter) base
// layer of log luminance computed at ~kTargetLongSide texels, so the
// viewfinder and full-resolution stills get the same regions; fine detail
// keeps its contrast (optionally scaled by `detail`). A pure RGB gain, so
// colour ratios are kept.
namespace rawr::local_tone {

struct Params {
    float strength = 0.0f;  // 0 = off .. 1; shadows compressed toward mid grey by (1 - strength)
    float detail = 1.0f;    // local contrast gain on (log Y - base); 1 = unchanged
};

inline constexpr float kMinDetail = 0.8f;
inline constexpr float kMaxDetail = 1.3f;
inline constexpr std::uint32_t kTargetLongSide = 256;  // low-res grid long side
inline constexpr int kRadius = 24;                     // box radius on the low-res grid
inline constexpr float kEps = 0.35f;                   // guided filter regularization (EV^2)
inline constexpr float kPivotEv = -2.4739312f;         // log2(0.18)

inline bool validParams(const Params& p) noexcept {
    return std::isfinite(p.strength) && p.strength >= 0.0f && p.strength <= 1.0f && std::isfinite(p.detail) &&
           p.detail >= kMinDetail && p.detail <= kMaxDetail;
}
inline bool active(const Params& p) noexcept { return validParams(p) && (p.strength > 0.0f || p.detail != 1.0f); }

// Full-res pixels per low-res texel for an image.
inline std::uint32_t blockSize(std::uint32_t width, std::uint32_t height) noexcept {
    const std::uint32_t longSide = std::max(width, height);
    return std::max<std::uint32_t>(1u, (longSide + kTargetLongSide - 1u) / kTargetLongSide);
}

// CPU mirror of ltm_common.glsl rawrLtmLift.
inline float regionalLiftEv(float baseEv, const Params& p) noexcept {
    const float target = baseEv < kPivotEv ? kPivotEv + (baseEv - kPivotEv) * (1.0f - p.strength) : baseEv;
    const float lift = std::clamp(target - baseEv, 0.0f, 5.0f * p.strength);
    return lift * std::clamp((baseEv + 13.0f) / 3.0f, 0.0f, 1.0f);
}

struct ImageRef {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // RGBA16F, GENERAL layout, storage usage
};

struct RecordInfo {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    std::uint32_t width = 0, height = 0;
    // Measured for the base layer; must hold the same content as targets[0].
    ImageRef measure;
    // Multiplied in place (e.g. the HDR and SDR copies of one image).
    std::array<ImageRef, 2> targets{};
    std::uint32_t targetCount = 1;
    // Camera RGB -> AP1 working, column-major 3x3.
    std::array<float, 9> cameraToWorking{1, 0, 0, 0, 1, 0, 0, 0, 1};
    float renderGain = 1.0f;  // linear render gain (AE post-gain): luminance is measured as rendered
    Params params;
    std::uint32_t slot = 0;  // frames in flight keep separate low-res images
};

class LocalToneProcessor final {
   public:
    static constexpr std::uint32_t kMaxSlots = 4;
    LocalToneProcessor(const rawr::vk::GpuContext& gpu, std::uint32_t slots = 1);
    ~LocalToneProcessor();
    LocalToneProcessor(const LocalToneProcessor&) = delete;
    LocalToneProcessor& operator=(const LocalToneProcessor&) = delete;

    // Records the passes into info.commandBuffer: compute reads of `measure`
    // and read/write of the targets must already be visible (GENERAL layout);
    // ends with a compute write -> compute/transfer read barrier on every
    // target. No-op (returns false) when the params are inactive or invalid.
    bool record(const RecordInfo& info);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rawr::local_tone
