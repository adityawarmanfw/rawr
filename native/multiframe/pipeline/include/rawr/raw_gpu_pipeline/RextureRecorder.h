#pragma once
#include <rawr/raw_gpu_pipeline/MultiframeRecorder.h>
#include <rawr/raw_gpu_pipeline/ResourceArena.h>
#include <rawr/raw_gpu_pipeline/ScratchLayout.h>
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>
#include <rawr/raw_merge_rexture_gpu/RawMergeRextureGpu.h>

#include <cstdint>

namespace rawr::raw_gpu_pipeline {

// Image names of the Rexture arena (see makeRextureScratchLayout). "rx_base_*" live for the whole burst, "rx_alt_*" for
// one companion frame.
// debug = verification build: adds full-resolution debug images (never allocated in production).
ScratchLayout makeRextureScratchLayout(const MultiframeGeometry&, bool debug = false);

// Records the Rexture merge stages against ResourceArena. Each method leaves its results in the arena so the next one
// may run in a later command buffer (same submission-safe contract as MultiframeRecorder).
class RextureRecorder final {
   public:
    RextureRecorder(ResourceArena& arena, VulkanExecutor& executor, rawr::raw_merge_rexture_gpu::Config config);

    // Frame preparation: normalise + local5 guide + pyramid + guide mean/variance (+ structure tensor for the base).
    // Reads the raw U16 image directly.
    void recordPrepare(VkCommandBuffer, bool base, VkImageView rawU16, const MultiframeFrameParameters&) const;

    // Base-frame kernel: structure-tensor integration and the anisotropic covariance (needs recordPrepare(base) first).
    // Uses rx_alt_mean / rx_alt_var as scratch, so it must run before the first companion's preparation.
    void recordBaseKernel(VkCommandBuffer, bool debug) const;

    // Clamps the RCD base to the native-sample range of each colour (5 x 5) in place; follows the RCD demosaic.
    void recordBaseDemosaicBound(VkCommandBuffer, VkImageView rawU16, const MultiframeFrameParameters&) const;

    // Base: per-level tile structure (gradient tensor + flat-tile mode) of the reference pyramid. After recordPrepare(base).
    void recordBaseTiles(VkCommandBuffer) const;
    // Companion alignment level l (0 = coarsest .. 3 = finest); record 0..3 in order after recordPrepare(companion).
    // debug: also keeps the prior (candidate upsampling) and the block-match result of the level for verification.
    void recordAlignmentLevel(VkCommandBuffer, std::uint32_t level, bool debug) const;

    // Burst start: zero the support map and set the cover map to 1 (cover = 1 + sum of the frames' clipped robustness).
    void recordBurstClear(VkCommandBuffer) const;
    // Robustness of the aligned companion (after its last alignment level): fused motion prior + photometric test + 5 x 5
    // minimum + support / cover update.
    void recordRobustness(VkCommandBuffer, bool debug) const;

    // Adds the aligned companion to the running per-pixel sums (after recordRobustness).
    void recordAccumulate(VkCommandBuffer, VkImageView rawU16, const MultiframeFrameParameters&, bool debug) const;

   private:
    ResourceArena& arena_;
    VulkanExecutor& executor_;
    rawr::raw_merge_rexture_gpu::Config config_;
    void recordPyramid(VkCommandBuffer, bool base) const;
};

}  // namespace rawr::raw_gpu_pipeline
