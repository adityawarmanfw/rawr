#pragma once
#include <rawr/raw_gpu_pipeline/FrameParameters.h>
#include <rawr/raw_gpu_pipeline/ResourceArena.h>
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>
#include <rawr/raw_merge_hdrplus_gpu/RawMergeHdrPlusGpu.h>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace rawr::raw_gpu_pipeline {

// Alignment and padding stages of the HDR+ Quality merge (the pad / pyramid / tile-alignment machinery of upstream's
// spatial path, whose per-frame merge is not used): padded frames, pyramids, tile alignment, finalize.
class HdrPlusRecorder final {
   public:
    HdrPlusRecorder(ResourceArena& arena, VulkanExecutor& executor, rawr::raw_merge_hdrplus_gpu::Config config,
                    rawr::raw_merge_hdrplus_gpu::Geometry geometry);
    void setHotPixels(VkBuffer buffer, std::uint32_t count) noexcept {
        hotPixelBuffer_ = buffer;
        hotPixelCount_ = count;
    }
    // Scale applied to the next companion's black-subtracted signal
    // (reference exposure / companion exposure); 1 when they match.
    void setCompanionGain(float gain) noexcept { companionGain_ = gain; }
    [[nodiscard]] const std::array<float, 4>& referenceBlack() const noexcept { return reference_.blackByPhase; }
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
    void recordFinalize(VkCommandBuffer command);

   private:
    ResourceArena& arena_;
    VulkanExecutor& executor_;
    rawr::raw_merge_hdrplus_gpu::Config config_{};
    rawr::raw_merge_hdrplus_gpu::Geometry geometry_{};
    RawNormalization reference_{};
    VkBuffer hotPixelBuffer_ = VK_NULL_HANDLE;
    std::uint32_t hotPixelCount_ = 0;
    float companionGain_ = 1.0f;

    void recordPrepare(VkCommandBuffer, bool reference, VkImageView rawU16, const RawNormalization&);
    void recordPyramid(VkCommandBuffer, bool reference);
    // Separable binomial blur of a width x height window (x pass into tmp).
    void recordBlur(VkCommandBuffer, const ArenaImage& src, std::array<std::int32_t, 2> srcOffset,
                    const ArenaImage& tmp, const ArenaImage& dst, std::uint32_t width, std::uint32_t height,
                    std::int32_t kernelSize, std::int32_t stride, bool quantizeHalf);
};

// Scratch for the HDR+ Quality (frequency) merge: the alignment resources plus RGBA tiles, three spectra, per-tile statistics and the
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
    // Uniform merges of frames whose exposure drifted (AE settling in the
    // ZSL ring): gains[i] = reference exposure / frame i exposure scales each
    // companion to the reference before alignment and merge, as upstream's
    // prepare_texture does; the merge itself stays the uniform one.
    void setExposureNormalization(std::vector<float> gains) { normalizationGains_ = std::move(gains); }
    void beginPass(std::uint32_t pass) noexcept;
    // Align-once mode: companions are aligned (and the reference prepared)
    // only in pass 0; later passes read through a coordinate offset.
    [[nodiscard]] bool alignsThisPass() const noexcept { return !config_.frequencyAlignOnce || pass_ == 0u; }
    void recordReference(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame);
    // Companion for this pass: padded frame + pyramid + all alignment levels
    // when alignsThisPass() (storing the shifts in slot), else the padded frame.
    void recordCompanionAlign(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame,
                              std::uint32_t slot);
    // The same as two submissions, so preview work can interleave: padded
    // frame, pyramid and the coarse levels; then the finest level (most of
    // the cost) and the shift store. Only when alignsThisPass().
    void recordCompanionAlignCoarse(VkCommandBuffer command, VkImageView rawU16, const RawNormalization& frame,
                                    std::uint32_t slot);
    void recordCompanionAlignFinest(VkCommandBuffer command, std::uint32_t slot);
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
    std::vector<float> normalizationGains_{};
    // Companion prepare gain: the drift normalization.
    float frameGain(std::uint32_t slot) const noexcept {
        return slot < normalizationGains_.size() ? normalizationGains_[slot] : 1.0f;
    }
    // Raw-pixel offset from this pass's padded coordinates into the prepared frame.
    std::array<std::int32_t, 2> passOffset() const noexcept;
    VkDeviceSize alignSlotBytes() const noexcept;
};

}  // namespace rawr::raw_gpu_pipeline
