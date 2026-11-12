#pragma once
#include <rawr/raw_gpu_pipeline/MultiframeRecorder.h>
#include <rawr/raw_gpu_pipeline/ResourceArena.h>
#include <rawr/raw_gpu_pipeline/ScratchLayout.h>
#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>
#include <rawr/raw_merge_rexture_gpu/RawMergeRextureGpu.h>

#include <cstdint>
#include <functional>

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
    // Reads the raw U16 image directly. gain = reference exposure / frame exposure scales a companion to the base (ignored
    // for the base).
    void recordPrepare(VkCommandBuffer, bool base, VkImageView rawU16, const MultiframeFrameParameters&,
                       float gain = 1.0f) const;

    // Base-frame kernel: structure-tensor integration and the anisotropic covariance (needs recordPrepare(base) first).
    // Uses rx_alt_mean / rx_alt_var as scratch, so it must run before the first companion's preparation.
    void recordBaseKernel(VkCommandBuffer, bool debug) const;

    // Verification build only (makeRextureScratchLayout debug = true): RCD demosaic of the whole base frame (fp32 work planes,
    // the spec's white balance) followed by the native-range bound, kept in rx_rcd_work. Production builds demosaic strip by
    // strip inside recordFinalize and never call this.
    void recordBaseDemosaic(VkCommandBuffer, VkImageView rawU16, const MultiframeFrameParameters&) const;

    // Base: per-level tile structure (gradient tensor + flat-tile mode) of the reference pyramid. After recordPrepare(base).
    void recordBaseTiles(VkCommandBuffer) const;
    // Companion alignment level l (0 = coarsest .. 3 = finest); record 0..3 in order after recordPrepare(companion).
    // debug: also keeps the prior (candidate upsampling) and the block-match result of the level for verification.
    void recordAlignmentLevel(VkCommandBuffer, std::uint32_t level, bool debug, std::uint32_t slot = 0) const;

    // Burst start: zero the support map and the accumulators.
    void recordBurstClear(VkCommandBuffer) const;
    // Robustness of the aligned companion (after its last alignment level): fused motion prior + photometric test + 5 x 5
    // minimum + support / cover update.
    void recordRobustness(VkCommandBuffer, bool debug, std::uint32_t slot = 0) const;

    // Adds the aligned companion to the running per-pixel sums (after recordRobustness), scaled by gain (as in
    // recordPrepare); a companion exposed longer than the base (gain < 1) leaves its clipped samples out.
    void recordAccumulate(VkCommandBuffer, VkImageView rawU16, const MultiframeFrameParameters&, bool debug,
                          float gain = 1.0f) const;
    // Two aligned companions (slots 0 and 1: fine flow + robustness kept apart) in one pass: the per-pixel state is read and
    // written once for both. The frames must share black / white levels.
    void recordAccumulatePair(VkCommandBuffer, VkImageView rawSlot0, VkImageView rawSlot1, const MultiframeFrameParameters&,
                              float gain0 = 1.0f, float gain1 = 1.0f) const;

    // Final pass (after the last companion): flags the 16 x 16 tiles that may need the fallback demosaic, then strip by strip
    // the RCD demosaic of the base (flagged tiles only) and the finalize into linear_output (RGBA16F), then the strip-wise
    // grain pass that lowers the noise where few frames were merged to the level of the merged areas around it.
    void recordFinalize(VkCommandBuffer, VkImageView baseRawU16, const MultiframeFrameParameters&, bool debug) const;
    // Tests and tools only: rows per strip of the grain pass (0 = default). The output must not depend on it.
    static void setStripRowsForTesting(std::uint32_t rows) noexcept;

    // Number of marks recordFinalize emits for an image of this height.
    static std::uint32_t finalizeMarks(std::uint32_t height, bool debug) noexcept;

    // Profiling: called with the stage name after each dispatch group of the companion stages (never set in production).
    using Mark = std::function<void(const char*)>;
    void setMark(Mark mark) const { mark_ = std::move(mark); }
    // Number of marks recordAlignmentLevel emits for level l.
    static std::uint32_t alignmentMarks(std::uint32_t level) noexcept { return level == 0 ? 4u : 3u; }

   private:
    mutable Mark mark_;
    ResourceArena& arena_;
    VulkanExecutor& executor_;
    rawr::raw_merge_rexture_gpu::Config config_;
    void recordPyramid(VkCommandBuffer, bool base) const;
    void recordGrainPass(VkCommandBuffer) const;
    void recordRcd(VkCommandBuffer, VkImageView rawU16, const MultiframeFrameParameters&, std::int32_t y0, std::int32_t rows,
                   bool gate) const;
};

}  // namespace rawr::raw_gpu_pipeline
