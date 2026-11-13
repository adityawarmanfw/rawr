#pragma once
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace rawr::raw_gpu_pipeline {

// Rexture base selection: scores burst frames with rx_frame_score in one submission. Independent of the merge coordinator (own
// pipeline, command buffer and fence), so it can run on the spool thread while a previous burst is still merging.
// Pair the scores with rawr::raw_merge_rexture_gpu::chooseBaseAtPress.
class RextureFrameScorer final {
   public:
    struct Score {
        double sharp = 0.0, sat = 0.0;
    };
    using Submit = std::function<void(const VkSubmitInfo&, VkFence)>;
    RextureFrameScorer() = default;
    ~RextureFrameScorer() { reset(); }
    RextureFrameScorer(const RextureFrameScorer&) = delete;
    RextureFrameScorer& operator=(const RextureFrameScorer&) = delete;

    void initialize(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily, Submit submit);
    [[nodiscard]] bool ready() const noexcept { return pipeline_ != VK_NULL_HANDLE; }
    // frames[i].raw must be R16_UINT, GENERAL, width x height; blackByPhase / whiteLevel from frames[i].parameters.
    std::vector<Score> score(const std::vector<BurstFrame>& frames, std::uint32_t width, std::uint32_t height,
                             std::uint32_t cfa, float noiseAlpha, float noiseBeta);
    void reset() noexcept;

   private:
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    Submit submit_;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    static constexpr std::uint32_t kMaxFrames = 32;
};

}  // namespace rawr::raw_gpu_pipeline
