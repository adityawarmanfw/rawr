#pragma once
#include <rawr/raw_gpu_pipeline/MultiframeRecorder.h>
#include <rawr/raw_gpu_pipeline/ResourceArena.h>
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>
#include <rawr/raw_merge_hdrplus_gpu/RawMergeHdrPlusGpu.h>
#include <array>
#include <cstdint>
#include <functional>

namespace rawr::raw_gpu_pipeline {

// Scratch for the HDR+ spatial merge (~0.5 GB at 12.5 MP vs ~1.3 GB Wronski):
// padded raw-domain reference/companion frames, their pyramids, one aligned
// companion, blur/weight planes, the accumulator and the RGBA16F output.
ScratchLayout makeHdrPlusScratchLayout(const rawr::raw_merge_hdrplus_gpu::Geometry& geometry);

// Records the HDR+ spatial merge against an arena built from
// makeHdrPlusScratchLayout. Frames stay in raw units (reference black) until
// recordFinalize normalizes into "hdrp_output". Every method leaves its
// results in the arena, so each may run in a separate submission.
class HdrPlusRecorder final {
   public:
    HdrPlusRecorder(ResourceArena& arena, VulkanExecutor& executor, rawr::raw_merge_hdrplus_gpu::Config config,
                    rawr::raw_merge_hdrplus_gpu::Geometry geometry);
    void setHotPixels(VkBuffer buffer, std::uint32_t count) noexcept {
        hotPixelBuffer_ = buffer;
        hotPixelCount_ = count;
    }
    // Reference: padded frame, pyramid, blurred copy, noise level, and the
    // accumulator initialized with ref / frameCount.
    void recordReference(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame,
                         std::uint32_t frameCount);
    // Companion: padded frame and pyramid.
    void recordCompanionPrepare(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame);
    // Companion: one coarse-to-fine alignment level against the reference
    // pyramid. Record levels levelCount()-1 down to 0 in order; each may be a
    // separate submission (bounds GPU stalls for interleaved preview work).
    [[nodiscard]] std::uint32_t levelCount() const noexcept { return std::uint32_t(geometry_.levels.size()); }
    void recordCompanionAlignLevel(VkCommandBuffer command, std::uint32_t level);
    // Companion: warp, blur, robust weight and weighted accumulation.
    // mark(k), when set, is called after warp (1), blur (2) and weight (3)
    // so callers can write sub-stage GPU timestamps.
    void recordCompanionMerge(VkCommandBuffer command, std::uint32_t frameCount,
                              const std::function<void(std::uint32_t)>& mark = {});
    void recordFinalize(VkCommandBuffer command);

   private:
    ResourceArena& arena_;
    VulkanExecutor& executor_;
    rawr::raw_merge_hdrplus_gpu::Config config_{};
    rawr::raw_merge_hdrplus_gpu::Geometry geometry_{};
    RawNormalization reference_{};
    VkBuffer hotPixelBuffer_ = VK_NULL_HANDLE;
    std::uint32_t hotPixelCount_ = 0;

    void recordPrepare(VkCommandBuffer, bool reference, VkImageView rawU16, const RawNormalization&);
    void recordPyramid(VkCommandBuffer, bool reference);
    // Separable binomial blur of a width x height window (x pass into tmp).
    void recordBlur(VkCommandBuffer, const ArenaImage& src, std::array<std::int32_t, 2> srcOffset,
                    const ArenaImage& tmp, const ArenaImage& dst, std::uint32_t width, std::uint32_t height,
                    std::int32_t kernelSize, std::int32_t stride, bool quantizeHalf);
};

}  // namespace rawr::raw_gpu_pipeline
