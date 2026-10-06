#pragma once
#include <android/native_window.h>
#include <jni.h>
#include <vulkan/vulkan.h>

#include <array>
#include <string>
#include <vector>

#include "imaging/FrameLimits.h"
namespace rawrcam::vulkan {
class VulkanContext;
}
namespace rawrcam::video {
class VideoOutput final {
   public:
    explicit VideoOutput(vulkan::VulkanContext& context) : context_(context) {}
    ~VideoOutput();
    VideoOutput(const VideoOutput&) = delete;
    VideoOutput& operator=(const VideoOutput&) = delete;
    bool start(JNIEnv* env, jobject surface, uint32_t width, uint32_t height, uint32_t bitDepth);
    void stop() noexcept;
    bool ready() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
    VkExtent2D extent() const noexcept { return extent_; }
    VkFormat format() const noexcept { return outputFormat_; }
    VkFormat preferredFormat(uint32_t bitDepth) const;
    double swapchainMs() const noexcept { return swapchainMs_; }
    const std::string& offeredFormats() const noexcept { return offeredFormats_; }
    const char* outputFormatName() const noexcept {
        if (!ready()) return "none";
        if (outputFormat_ == VK_FORMAT_R8G8B8A8_UNORM) return "rgba8";
        return outputFormat_ == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? "rgb10a2" : "rgba16f";
    }
    bool acquire(uint32_t frameSlot, uint32_t* imageIndex);
    uint32_t encoderImageCount() const noexcept { return static_cast<uint32_t>(images_.size()); }
    VkSemaphore available(uint32_t frameSlot) const { return available_[frameSlot]; }
    VkSemaphore rendered(uint32_t frameSlot) const { return rendered_[frameSlot]; }
    // presentTimeNs (CLOCK_MONOTONIC) becomes the encoded frame's PTS when the
    // driver supports display timing; 0 keeps the queue-time timestamp.
    VkResult present(VkQueue queue, uint32_t frameSlot, uint32_t imageIndex, uint64_t presentTimeNs);
    // Whether present() stamps frames with the caller's time.
    bool stampsPresentTime() const noexcept;
    // CLOCK_MONOTONIC at start(). Frames captured earlier predate the recording.
    int64_t startedAtNs() const noexcept { return startedAtNs_; }

   private:
    friend class VideoRecorder;
    static constexpr uint32_t kPreferredEncoderImages = 5;
    static constexpr uint64_t kEncoderAcquireWaitNs = 3'000'000;
    rawrcam::vulkan::VulkanContext& context_;
    ANativeWindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat outputFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    std::string offeredFormats_;
    double swapchainMs_ = 0;
    int64_t startedAtNs_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<bool> initialized_;
    std::array<VkSemaphore, rawrcam::imaging::kRealtimeFramesInFlight> available_{};
    std::array<VkSemaphore, rawrcam::imaging::kRealtimeFramesInFlight> rendered_{};
};
}  // namespace rawrcam::video
