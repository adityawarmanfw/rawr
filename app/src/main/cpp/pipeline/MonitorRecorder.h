#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
namespace rawrcam::monitoring {
class MonitoringOverlayProcessor;
class ImageScopesProcessor;
}  // namespace rawrcam::monitoring
namespace rawrcam::presentation {
class SwapchainRenderer;
}
namespace rawrcam::diagnostics {
class GpuTimingTracker;
}
namespace rawrcam::pipeline {
struct MonitorFrame {
    VkCommandBuffer command;
    uint32_t slotIndex, swapImageIndex, width, height;
    VkImage linearImage, tonemappedImage;
    VkImageView linearView, tonemappedView;
    uint32_t diagnosticMode;
    int sensorOrientationDegrees, displayRotationDegrees, scopeDeviceRotationDegrees;
    bool renderedExposureFeedbackEnabled;
    float shadowLiftEv, blackPointEv;
    bool overlayPresentationEnabled = true;
};
class MonitorRecorder final {
   public:
    MonitorRecorder(monitoring::MonitoringOverlayProcessor& overlay, monitoring::ImageScopesProcessor& scopes,
                    presentation::SwapchainRenderer& presentation, diagnostics::GpuTimingTracker& timing)
        : monitoringOverlay_(overlay), imageScopes_(scopes), presentation_(presentation), performance_(timing) {}
    VkImageView rawStateView(uint32_t slot) const;
    void recordOverlays(const MonitorFrame& frame) const;
    void recordSceneExposure(VkCommandBuffer command, uint32_t slotIndex, VkImageView linearView, uint32_t width,
                             uint32_t height, const std::array<float, 3>& lumaWeights, float clipLuma) const;
    void present(const MonitorFrame& frame, uint32_t cropX = 0, uint32_t cropY = 0, uint32_t cropW = 0,
                 uint32_t cropH = 0) const;
    void recordVideoScopes(VkCommandBuffer command, uint32_t frameSlot, VkImageView monitorView, VkImage monitorImage,
                           uint32_t width, uint32_t height, int sensorOrientationDegrees,
                           int deviceRotationDegrees) const;
    void restoreVideoScopes(VkCommandBuffer command, uint32_t frameSlot) const;

   private:
    static void transitionImageForSampling(VkCommandBuffer command, VkImage image);
    static void transitionImageForCompute(VkCommandBuffer command, VkImage image);
    monitoring::MonitoringOverlayProcessor& monitoringOverlay_;
    monitoring::ImageScopesProcessor& imageScopes_;
    presentation::SwapchainRenderer& presentation_;
    diagnostics::GpuTimingTracker& performance_;
};
}  // namespace rawrcam::pipeline
