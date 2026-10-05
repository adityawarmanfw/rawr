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
    // Reference padded frame and pyramid only (frequency merge passes).
    void recordReferencePrepare(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame);
    // Per-pass padding offsets inside the fixed padded extent (frequency merge).
    void setPads(std::uint32_t left, std::uint32_t top) noexcept {
        geometry_.padLeft = left;
        geometry_.padTop = top;
    }
    // Companion padded frame only (frequency merge passes reusing alignment).
    void recordCompanionPadded(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame);
    // Companion: padded frame and pyramid.
    void recordCompanionPrepare(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame);
    // Companion: one coarse-to-fine alignment level against the reference
    // pyramid. Record levels levelCount()-1 down to 0 in order; each may be a
    // separate submission (bounds GPU stalls for interleaved preview work).
    [[nodiscard]] std::uint32_t levelCount() const noexcept { return std::uint32_t(geometry_.levels.size()); }
    // mark(k), when set, is called after upsample+correction (1) and tile cost (2).
    void recordCompanionAlignLevel(VkCommandBuffer command, std::uint32_t level,
                                   const std::function<void(std::uint32_t)>& mark = {});
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

// Scratch for the HDR+ frequency merge: the alignment resources of the
// spatial layout plus RGBA tiles, three spectra, per-tile statistics and the
// raw accumulator (~0.85 GB at 12.5 MP).
ScratchLayout makeHdrPlusFrequencyScratchLayout(const rawr::raw_merge_hdrplus_gpu::FrequencyGeometry& geometry,
                                                std::uint32_t width, std::uint32_t height);

// Records the HDR+ frequency-domain merge (upstream "Higher quality"): four
// half-tile-shifted passes; per pass the reference spectrum, then per
// companion alignment (via HdrPlusRecorder) and a per-frequency Wiener merge,
// then deconvolution, inverse transform and accumulation.
class HdrPlusFrequencyRecorder final {
   public:
    HdrPlusFrequencyRecorder(ResourceArena& arena, VulkanExecutor& executor, rawr::raw_merge_hdrplus_gpu::Config config,
                             rawr::raw_merge_hdrplus_gpu::FrequencyGeometry geometry);
    HdrPlusRecorder& alignment() noexcept { return align_; }
    void beginPass(std::uint32_t pass) noexcept;
    // Align-once mode: companions are aligned (and the reference prepared)
    // only in pass 0; later passes read through a coordinate offset.
    [[nodiscard]] bool alignsThisPass() const noexcept { return !config_.frequencyAlignOnce || pass_ == 0u; }
    void recordReference(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame);
    // Companion for this pass: padded frame + pyramid + all alignment levels
    // when alignsThisPass() (storing the shifts in slot), else the padded frame.
    void recordCompanionAlign(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame,
                              std::uint32_t slot);
    // mark(k), when set, is called after warp (1), mismatch+spectrum (2) and
    // mismatch normalization (3) for sub-stage GPU timestamps.
    void recordCompanionMerge(VkCommandBuffer command, std::uint32_t frameCount, std::uint32_t slot,
                              const std::function<void(std::uint32_t)>& mark = {});
    void recordPassFinish(VkCommandBuffer command, std::uint32_t frameCount);
    void recordFinalize(VkCommandBuffer command) { align_.recordFinalize(command); }

   private:
    ResourceArena& arena_;
    VulkanExecutor& executor_;
    rawr::raw_merge_hdrplus_gpu::Config config_{};
    rawr::raw_merge_hdrplus_gpu::FrequencyGeometry geometry_{};
    HdrPlusRecorder align_;
    std::uint32_t pass_ = 0;
    // Raw-pixel offset from this pass's padded coordinates into the prepared frame.
    std::array<std::int32_t, 2> passOffset() const noexcept;
    VkDeviceSize alignSlotBytes() const noexcept;
};

}  // namespace rawr::raw_gpu_pipeline
