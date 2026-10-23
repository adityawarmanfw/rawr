#pragma once
#include <vulkan/vulkan.h>

#include <array>
#include <functional>
#include <string>
#include <utility>

#include "geometry/LensDistortion.h"
namespace rawrcam::presentation {
struct ScopeDraw {
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    float x = 0, y = 0, width = 0, height = 0, sourceAspect = 1.0f, cornerRadius = 0.0f;
    uint32_t quarterTurns = 0;
    bool enabled = false;
};
struct RecordInfo {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkImage swapImage = VK_NULL_HANDLE;
    bool swapInitialized = false;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D swapExtent{};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSet tonemappedSet = VK_NULL_HANDLE;
    VkDescriptorSet linearSet = VK_NULL_HANDLE;
    uint32_t previewWidth = 0, previewHeight = 0;
    // Idle video-mode crop window in preview-source pixels (pre-rotation).
    // cropW == 0 disables. Never set for video-source frames (already cropped).
    uint32_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    int sensorOrientationDegrees = 0, displayRotationDegrees = 0;
    uint32_t exifOrientation = 0;  // 0 keeps camera rotation; 1..8 for DNG display.
    uint32_t diagnosticMode = 0;
    bool overlayEnabled = false;
    // Lens profile calibration to undistort the live viewfinder; null = off.
    const rawrcam::geometry::LensDistortionModel* lensWarp = nullptr;
    // Lens profile mirror (geometry::kLensMirror* bits), live viewfinder only.
    uint32_t lensMirror = 0;
    std::array<ScopeDraw, 3> scopes{};
};
class PresentRecorder final {
   public:
    using Diagnostic = std::function<void(const std::string&)>;
    explicit PresentRecorder(Diagnostic d) : diagnostic_(std::move(d)) {}
    void resetLogging() noexcept { mapLogged_ = false; }
    void record(const RecordInfo&);

   private:
    Diagnostic diagnostic_;
    bool mapLogged_ = false;
};
}  // namespace rawrcam::presentation
