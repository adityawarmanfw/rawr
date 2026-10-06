#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "rawr/vk/GpuContext.h"
#include "rawr/vk/OwnedImage.h"

namespace rawrcam::video {

// Recording-owned single-frame RAW stage. Its images and descriptors never
// depend on the preview swapchain or preview's half-resolution linear image.
class VideoDemosaic final {
   public:
    // Per-frame-slot resources; the app asserts this matches its realtime
    // frames-in-flight contract.
    static constexpr uint32_t kFramesInFlight = 3;

    VideoDemosaic(const rawr::vk::GpuContext& context, uint32_t rawWidth, uint32_t rawHeight, uint32_t outputWidth,
                  uint32_t outputHeight);
    ~VideoDemosaic();
    VideoDemosaic(const VideoDemosaic&) = delete;
    VideoDemosaic& operator=(const VideoDemosaic&) = delete;

    struct Frame {
        VkImageView rawView = VK_NULL_HANDLE;
        VkBuffer rawBuffer = VK_NULL_HANDLE;
        uint32_t rawStridePixels = 0;
        std::array<float, 4> black{};
        std::array<float, 4> wb{};
        float white = 0.0f;
        uint32_t cfa = 0;
        const float* lensShading = nullptr;
        size_t lensShadingCount = 0;
        uint32_t lensShadingWidth = 0;
        uint32_t lensShadingHeight = 0;
        bool noiseProfileValid = false;
        float noiseA = 0.0f;
        float noiseB = 0.0f;
    };

    void record(VkCommandBuffer command, uint32_t frameSlot, const Frame& frame);
    VkImageView outputView(uint32_t frameSlot) const noexcept { return outputs_[frameSlot].view; }
    VkImage outputImage(uint32_t frameSlot) const noexcept { return outputs_[frameSlot].image; }
    VkImageView clipStateView(uint32_t frameSlot) const noexcept { return clipStates_[frameSlot].view; }
    VkImage clipStateImage(uint32_t frameSlot) const noexcept { return clipStates_[frameSlot].image; }
    uint32_t outputWidth() const noexcept { return outputWidth_; }
    uint32_t outputHeight() const noexcept { return outputHeight_; }
    uint32_t rawWidth() const noexcept { return rawWidth_; }
    uint32_t rawHeight() const noexcept { return rawHeight_; }
    uint32_t cropX() const noexcept { return cropX_; }
    uint32_t cropY() const noexcept { return cropY_; }
    uint32_t sensorScale() const noexcept { return reduceCfa_ ? 2u : 1u; }
    // Re-reads the 2x reduction filter at recording start. Debug A/B:
    // `adb shell setprop debug.rawr.video_downscale box` restores the former
    // 2x2 box average; anything else uses the anti-aliasing filter.
    void selectDownscaleFilter();
    void setAntiAlias(bool enabled) noexcept { antiAlias_ = enabled; }
    const char* method() const noexcept {
        return !reduceCfa_ ? "tiled_mhc_5x5" : antiAlias_ ? "tiled_mhc_5x5+lanczos3_aa_2x" : "tiled_mhc_fused_2x_area";
    }

   private:
    struct HostBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    HostBuffer makeHostBuffer(VkDeviceSize bytes);
    void destroyHostBuffer(HostBuffer& buffer) noexcept;
    void destroy() noexcept;
    void createDownscale();
    void recordDownscale(VkCommandBuffer command, uint32_t frameSlot);

    rawr::vk::GpuContext context_;
    uint32_t rawWidth_ = 0, rawHeight_ = 0;
    uint32_t outputWidth_ = 0, outputHeight_ = 0;
    uint32_t cropX_ = 0, cropY_ = 0;
    bool reduceCfa_ = false;
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> descriptors_{};
    std::array<rawr::vk::OwnedImage, kFramesInFlight> outputs_{};
    std::array<rawr::vk::OwnedImage, kFramesInFlight> clipStates_{};
    std::array<HostBuffer, kFramesInFlight> lscBuffers_{};
    rawr::vk::OwnedImage dummyRaw_{};
    std::array<bool, kFramesInFlight> initialized_{};
    bool dummyInitialized_ = false;
    // 2x reduction: the demosaic writes the full-resolution crop here, then an
    // anti-aliasing filter decimates it into outputs_. One copy is shared by
    // all slots; a barrier orders each frame after the previous one's reads.
    uint32_t fullWidth_ = 0, fullHeight_ = 0;
    rawr::vk::OwnedImage fullImage_{};
    rawr::vk::OwnedImage fullClip_{};
    bool fullInitialized_ = false;
    bool antiAlias_ = true;
    VkDescriptorSetLayout downscaleLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout downscalePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline downscalePipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool downscalePool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> downscaleSets_{};
};
}  // namespace rawrcam::video
