#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "MonitoringState.h"
#include "image_scopes/types.h"
#include "imaging/FrameLimits.h"
#include "vulkan/ImageResources.h"

namespace image_scopes {
class ImageScopes;
}

namespace rawrcam::monitoring {

using RenderedExposureFeedback = image_scopes::DisplayExposureStatsData;
using SceneExposureStats = image_scopes::SceneExposureStatsData;

class ImageScopesProcessor final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    ImageScopesProcessor();
    ~ImageScopesProcessor();
    ImageScopesProcessor(const ImageScopesProcessor&) = delete;
    ImageScopesProcessor& operator=(const ImageScopesProcessor&) = delete;

    void initialize(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, VkCommandPool commandPool,
                    uint32_t queueFamilyIndex, float timestampPeriodNs, uint32_t inputWidth, uint32_t inputHeight,
                    Diagnostic diagnostic = {});
    void reset() noexcept;
    void setPresentationState(const ScopePresentationState& state) noexcept { state_ = state; }
    [[nodiscard]] ScopePresentationState presentationState() const noexcept { return state_; }

    void record(VkCommandBuffer command, uint32_t frameSlot, VkImageView displayView, VkImage displayImage,
                uint32_t sourceQuarterTurns, bool measureExposureFeedback, uint32_t sourceWidth = 0,
                uint32_t sourceHeight = 0,
                image_scopes::SamplingMode sampling = image_scopes::SamplingMode::FullReference);
    // Scene key meter on the scene-linear rgba16f image (before render gain).
    // lumaWeights maps its RGB to luma; samples at/above clipLuma are clipped.
    // spot (normalized x0,y0,x1,y1 of the image; empty = none) meters only inside it.
    void recordSceneExposure(VkCommandBuffer command, uint32_t frameSlot, VkImageView linearView, uint32_t width,
                             uint32_t height, const std::array<float, 3>& lumaWeights, float clipLuma,
                             const std::array<float, 4>& spot = {});
    [[nodiscard]] std::optional<SceneExposureStats> consumeSceneExposure(uint32_t frameSlot);
    // Clip luma the slot's scene statistics were binned with (0 if none).
    [[nodiscard]] float sceneClipLuma(uint32_t frameSlot) const noexcept {
        return frameSlot < sceneClipLuma_.size() ? sceneClipLuma_[frameSlot] : 0.0f;
    }
    void retireFrameSlot(uint32_t frameSlot);
    [[nodiscard]] std::optional<RenderedExposureFeedback> consumeExposureFeedback(uint32_t frameSlot);
    void discardFrameSlot(uint32_t frameSlot) noexcept;

    [[nodiscard]] VkImageView renderedView(uint32_t frameSlot, uint32_t placementIndex) const;
    [[nodiscard]] VkImage renderedImage(uint32_t frameSlot, uint32_t placementIndex) const;
    [[nodiscard]] static constexpr float renderAspect() noexcept {
        return static_cast<float>(kRenderWidth) / static_cast<float>(kRenderHeight);
    }

   private:
    void transitionImagesToGeneral(VkQueue queue, VkCommandPool commandPool);
    void consumeExposureStatsTiming(uint32_t frameSlot) noexcept;

    static constexpr uint32_t kRenderWidth = 512;
    static constexpr uint32_t kRenderHeight = 384;
    VkDevice device_ = VK_NULL_HANDLE;
    Diagnostic diagnostic_{};
    VkQueryPool exposureStatsTimingPool_ = VK_NULL_HANDLE;
    float timestampPeriodNs_ = 0.0f;
    uint32_t timestampValidBits_ = 0;
    uint64_t exposureStatsTimingSamples_ = 0;
    double exposureStatsTimingTotalNs_ = 0.0;
    double exposureStatsTimingBestNs_ = 0.0;
    double exposureStatsTimingWorstNs_ = 0.0;
    bool exposureStatsTimingComplete_ = false;
    uint32_t inputWidth_ = 0;
    uint32_t inputHeight_ = 0;
    ScopePresentationState state_{};
    std::unique_ptr<image_scopes::ImageScopes> backend_;
    struct ExposureStatsReadback {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize allocationSize = 0;
        bool coherent = false;
        bool recorded = false;
    };
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    std::array<ExposureStatsReadback, rawrcam::imaging::kRealtimeFramesInFlight> exposureStatsReadback_{};
    std::array<ExposureStatsReadback, rawrcam::imaging::kRealtimeFramesInFlight> sceneStatsReadback_{};
    std::array<float, rawrcam::imaging::kRealtimeFramesInFlight> sceneClipLuma_{};
    void createReadback(ExposureStatsReadback& rb, VkDeviceSize bytes);
    void destroyReadback(ExposureStatsReadback& rb) noexcept;
    bool readReadback(ExposureStatsReadback& rb, void* out, size_t bytes);
    std::array<std::array<rawrcam::vulkan::OwnedImage, 3>, rawrcam::imaging::kRealtimeFramesInFlight> targets_{};
};

}  // namespace rawrcam::monitoring
