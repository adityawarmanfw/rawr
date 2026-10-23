#pragma once

#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include "geometry/LensDistortion.h"
#include "imaging/FrameLimits.h"
#include "monitoring/MonitoringState.h"
#include "presentation/PresentRecorder.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::presentation {

class SwapchainRenderer final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;

    explicit SwapchainRenderer(Diagnostic diagnostic);
    ~SwapchainRenderer();

    SwapchainRenderer(const SwapchainRenderer&) = delete;
    SwapchainRenderer& operator=(const SwapchainRenderer&) = delete;

    void initialize(const vulkan::VulkanContext& context);
    void createSwapchain(VkSurfaceKHR surface, ANativeWindow* window);
    void destroySwapchain();

    void setFrameSources(
        const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& tonemappedViews,
        const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& linearViews,
        const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& overlayViews,
        const std::array<std::array<VkImageView, 3>, rawrcam::imaging::kRealtimeFramesInFlight>& scopeViews,
        float scopeSourceAspect);
    void setVideoFrameSources(const std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight>& views);
    void setScopePresentationState(const rawrcam::monitoring::ScopePresentationState& state) noexcept {
        scopeState_ = state;
    }
    void clearFrameSources() noexcept;

    [[nodiscard]] bool ready() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return swapExtent_; }

    VkResult acquireNextImage(VkSemaphore imageAvailable, uint32_t* imageIndex) const;
    VkResult present(VkQueue queue, VkSemaphore renderFinished, uint32_t imageIndex) const;

    void record(VkCommandBuffer command, uint32_t frameSlot, uint32_t imageIndex, uint32_t previewWidth,
                uint32_t previewHeight, int sensorOrientationDegrees, int displayRotationDegrees,
                uint32_t diagnosticMode, bool monitoringOverlayEnabled, uint32_t exifOrientation = 0,
                bool videoSource = false, uint32_t cropX = 0, uint32_t cropY = 0, uint32_t cropW = 0,
                uint32_t cropH = 0, const rawrcam::geometry::LensDistortionModel* lensWarp = nullptr,
                uint32_t lensMirror = 0);

    // Updated only after a fresh live RAW frame is accepted for presentation.
    void noteLivePresentation(uint64_t generation, int64_t timestamp) {
        std::lock_guard<std::mutex> lock(lensWarpMutex_);
        livePresentation_ = {generation, timestamp};
    }
    std::pair<uint64_t, int64_t> livePresentation() const {
        std::lock_guard<std::mutex> lock(lensWarpMutex_);
        return livePresentation_;
    }
    void resetLogging() noexcept { recorder_.resetLogging(); }
    // Lens calibration the last live-viewfinder frame was undistorted with
    // (nullopt = none), so display taps and face boxes map through it.
    [[nodiscard]] std::optional<rawrcam::geometry::LensDistortionModel> presentedLensWarp() const {
        std::lock_guard<std::mutex> lock(lensWarpMutex_);
        return presentedLensWarp_;
    }
    // Lens profile mirror the last live-viewfinder frame was flipped with.
    [[nodiscard]] uint32_t presentedLensMirror() const {
        std::lock_guard<std::mutex> lock(lensWarpMutex_);
        return presentedLensMirror_;
    }

   private:
    struct PresentPush {
        uint32_t quarterTurns;
        float srcAspect;
        float dstAspect;
        uint32_t diagnosticMode;
        uint32_t overlayEnabled;
        float cornerRadius;
        uint32_t exifOrientation;
        uint32_t cropEnabled;
        float cropX0;
        float cropY0;
        float cropX1;
        float cropY1;
        uint32_t lensWarp;
        float lensCx, lensCy, lensFx, lensFy, lensFxOut, lensFyOut;
        std::array<float, 8> k;
        uint32_t lensMirror;
    };

    std::pair<uint64_t, int64_t> livePresentation_{};
    mutable std::mutex lensWarpMutex_;
    std::optional<rawrcam::geometry::LensDistortionModel> presentedLensWarp_;
    uint32_t presentedLensMirror_ = 0;

    void createPipeline();
    void createFramebuffers();
    void rebuildDescriptors();
    VkShaderModule createShaderModule(const unsigned char* bytes, size_t size) const;

    Diagnostic diagnostic_;
    PresentRecorder recorder_;
    const vulkan::VulkanContext* context_ = nullptr;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surfaceFormat_{};
    VkExtent2D swapExtent_{};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<bool> swapInitialized_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, rawrcam::imaging::kRealtimeFramesInFlight> tonemappedSets_{};
    std::array<VkDescriptorSet, rawrcam::imaging::kRealtimeFramesInFlight> linearSets_{};
    std::array<VkDescriptorSet, rawrcam::imaging::kRealtimeFramesInFlight> videoSets_{};
    std::array<std::array<VkDescriptorSet, 3>, rawrcam::imaging::kRealtimeFramesInFlight> scopeSets_{};
    std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> tonemappedViews_{};
    std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> linearViews_{};
    std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> videoViews_{};
    std::array<VkImageView, rawrcam::imaging::kRealtimeFramesInFlight> overlayViews_{};
    std::array<std::array<VkImageView, 3>, rawrcam::imaging::kRealtimeFramesInFlight> scopeViews_{};
    rawrcam::monitoring::ScopePresentationState scopeState_{};
    float scopeSourceAspect_ = 1.0f;
    bool hasFrameSources_ = false;
    bool hasVideoFrameSources_ = false;
};

}  // namespace rawrcam::presentation
