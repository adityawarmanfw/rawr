#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "raw_highlight/Spirv.hpp"
#include "rawr/vk/OwnedImage.h"

namespace rawr::highlight {

// highlight_guide_seed.comp (compiled for the caller's RAWR_HL_SCALE),
// highlight_guide_propagate.comp and highlight_guide_smooth.comp.
struct GuideChainShaders {
    SpirvWords seed;
    SpirvWords propagate;
    SpirvWords smooth;
};

// Colour-propagation guide pipelines, shared by every GuideChain on a device.
class GuideChainPipelines {
   public:
    GuideChainPipelines(VkDevice device, const GuideChainShaders& shaders);
    ~GuideChainPipelines();
    GuideChainPipelines(const GuideChainPipelines&) = delete;
    GuideChainPipelines& operator=(const GuideChainPipelines&) = delete;

   private:
    friend class GuideChain;
    void destroy() noexcept;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout seedSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout seedLayout_ = VK_NULL_HANDLE;
    VkPipeline seed_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout passSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout passLayout_ = VK_NULL_HANDLE;
    VkPipeline propagate_ = VK_NULL_HANDLE;
    VkPipeline smooth_ = VK_NULL_HANDLE;
    std::array<uint32_t, 2> seedSize_{16, 16}, propagateSize_{16, 16}, smoothSize_{16, 16};
};

// Low-resolution colour guide for one working image: seed from bright valid
// pixels, jump-flood propagation into clipped regions, then four smoothing
// passes. The caller owns the apply pass, which samples finalGuide().
class GuideChain {
   public:
    // guideWidth/guideHeight: working image size divided by the seed cell
    // (4 px at RAWR_HL_SCALE 1, 8 px at RAWR_HL_SCALE 2), rounded up.
    GuideChain(VkPhysicalDevice physicalDevice, const GuideChainPipelines& pipelines, uint32_t guideWidth,
               uint32_t guideHeight);
    ~GuideChain();
    GuideChain(const GuideChain&) = delete;
    GuideChain& operator=(const GuideChain&) = delete;

    // Optional VK_EXT_conditional_rendering predicate around every guide
    // dispatch; frames whose predicate is zero skip the chain.
    struct Conditional {
        PFN_vkCmdBeginConditionalRenderingEXT begin = nullptr;
        PFN_vkCmdEndConditionalRenderingEXT end = nullptr;
        const VkConditionalRenderingBeginInfoEXT* info = nullptr;
    };
    struct Timing {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t seedQuery = 0;
        uint32_t propagationQuery = 0;
    };

    // Transitions the guide images to GENERAL once; record() calls it too.
    void recordInitialize(VkCommandBuffer command);

    // Records the chain. rgbView (rgba16f, white-balanced) and clipStateView
    // (r16ui) must already be readable by compute. `width`/`height` are the
    // working image size; channelCeilings are the post-WB channel ceilings.
    // Returns the view holding the final guide.
    VkImageView record(VkCommandBuffer command, VkImageView rgbView, VkImageView clipStateView, uint32_t width,
                       uint32_t height, const float channelCeilings[3], const Conditional* conditional = nullptr,
                       const Timing* timing = nullptr);

    // Guide view to bind when reconstruction is off and record() is skipped.
    VkImageView idleGuideView() const noexcept { return guideA_.view; }
    uint32_t guideWidth() const noexcept { return guideWidth_; }
    uint32_t guideHeight() const noexcept { return guideHeight_; }

   private:
    void destroy() noexcept;

    const GuideChainPipelines& pipelines_;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t guideWidth_ = 0, guideHeight_ = 0;
    rawr::vk::OwnedImage guideA_{}, guideB_{};
    bool initialized_ = false;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet seedSet_ = VK_NULL_HANDLE;
    VkDescriptorSet aToBSet_ = VK_NULL_HANDLE;
    VkDescriptorSet bToASet_ = VK_NULL_HANDLE;
};

}  // namespace rawr::highlight
