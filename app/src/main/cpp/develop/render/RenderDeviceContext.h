#pragma once
#include <vulkan/vulkan.h>

#include <cstddef>
#include <mutex>
namespace rawrcam::develop::rendered {
struct RenderDeviceContext {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    std::mutex* queueSubmitMutex = nullptr;
};
struct RenderImageView {
    VkImage image;
    VkImageView view;
};
struct RenderOutputView {
    const void* pixels = nullptr;
    size_t bytes = 0;
    VkImageView imageView = VK_NULL_HANDLE;
    const void* gainmapPixels = nullptr;
    size_t gainmapBytes = 0;
    uint32_t gainmapWidth = 0;
    uint32_t gainmapHeight = 0;
};
}  // namespace rawrcam::develop::rendered
