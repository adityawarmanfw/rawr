#pragma once
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>
namespace rawr::raw_gpu_pipeline {
enum class ShaderId : std::uint32_t {
    RawNormalize,
    AlignmentLocal5,
    PyramidGaussian,
    PyramidDecimate,
    FlowUpsample,
    BlockMatch,
    LkRefine,
    FlowDenseSmooth,
    KernelEstimate,
    RobustGuide,
    RobustStats,
    RobustWarp,
    RobustFlowScale,
    RobustPhotometric,
    RobustErode5,
    AffineFit,
    AffineGate,
    SupportAccumulate,
    Accumulate,
    ReferenceAccumulate,
    A11Finalize,
    NoiseEstimate,
    FallbackChroma,
    HotPixelConceal,
    // HDR+ spatial merge (merge_hdrplus/).
    HdrpPrepare,
    HdrpHotPixel,
    HdrpAvgPool,
    HdrpBlur,
    HdrpUpsampleAlign,
    HdrpCorrectUpsampling,
    HdrpTileDiff,
    HdrpBestTile,
    HdrpWarp,
    HdrpColorDiff,
    HdrpColumnSum,
    HdrpMean,
    HdrpMergeWeight,
    HdrpAccumulate,
    HdrpFinalize,
    // HDR+ frequency-domain merge (merge_hdrplus/hdrq_*).
    HdrqToRgba,
    HdrqWarpRgba,
    HdrqRms,
    HdrqMismatch,
    HdrqRegionMean,
    HdrqMismatchNorm,
    HdrqForwardDft,
    HdrqMerge,
    HdrqDeconvolute,
    HdrqBackwardDft,
    HdrqBorder,
    HdrqAccumulate,
    HdrqShiftTable,
    HdrqHighlightsNorm,
    Count
};
struct ImageBinding {
    std::uint32_t binding;
    VkImageView view;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};
struct BufferBinding {
    std::uint32_t binding;
    VkBuffer buffer;
    VkDeviceSize offset = 0;
    VkDeviceSize range = VK_WHOLE_SIZE;
};
struct DispatchBindings {
    std::vector<ImageBinding> images;
    std::vector<BufferBinding> buffers;
};
struct TraceTarget {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize range = VK_WHOLE_SIZE;
};
class VulkanExecutor final {
   public:
    VulkanExecutor() = default;
    ~VulkanExecutor() { reset(); };
    VulkanExecutor(const VulkanExecutor&) = delete;
    VulkanExecutor& operator=(const VulkanExecutor&) = delete;
    void initialize(VkDevice device, bool enableTracePipelines);
    void reset() noexcept;
    // Call only when all command buffers recorded by the previous batch have completed.
    // Resets transient descriptor allocations so every recorded dispatch receives an
    // immutable descriptor-set snapshot for its lifetime.
    void beginBatch();
    void record(VkCommandBuffer cmd, ShaderId shader, const DispatchBindings& bindings, const void* push,
                std::uint32_t pushBytes, std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ = 1,
                const TraceTarget* trace = nullptr);

   private:
    struct Program {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline prod = VK_NULL_HANDLE;
        VkPipeline trace = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
    };
    VkDevice device_ = VK_NULL_HANDLE;
    bool trace_ = false;
    Program programs_[static_cast<std::size_t>(ShaderId::Count)]{};
};
void computeWriteBarrier(VkCommandBuffer cmd);
}  // namespace rawr::raw_gpu_pipeline
