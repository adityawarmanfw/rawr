#pragma once

#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace image_scopes::tests {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceSize allocationSize = 0;
    bool coherent = false;
};

struct RgbaImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t bytesPerPixel = 4;
};

class VulkanTestContext {
public:
    VulkanTestContext();
    ~VulkanTestContext();
    VulkanTestContext(const VulkanTestContext&) = delete;
    VulkanTestContext& operator=(const VulkanTestContext&) = delete;

    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    std::uint32_t queueFamily() const { return queueFamily_; }
    VkCommandBuffer commandBuffer() const { return commandBuffer_; }
    VkCommandPool commandPool() const { return commandPool_; }
    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    const std::string& deviceName() const { return deviceName_; }
    bool hasTimestamps() const { return timestampValidBits_ != 0; }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred = 0) const;
    void destroyBuffer(Buffer& b) const;
    void* map(const Buffer& b) const;
    void unmap(const Buffer& b) const;
    void flush(const Buffer& b, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;
    void invalidate(const Buffer& b, VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) const;

    RgbaImage createRgbaImage(std::uint32_t width, std::uint32_t height,
                              VkFormat format = VK_FORMAT_R8G8B8A8_UNORM) const;
    void destroyRgbaImage(RgbaImage& image) const;
    void uploadRgba(RgbaImage& image, std::span<const std::uint8_t> rgba);
    std::vector<std::uint8_t> downloadRgba(RgbaImage& image);

    void beginCommands();
    void submitAndWait();

    VkQueryPool createTimestampQueryPool(std::uint32_t count) const;
    void destroyQueryPool(VkQueryPool pool) const;
    double timestampDeltaMs(VkQueryPool pool, std::uint32_t first, std::uint32_t second) const;

private:
    std::uint32_t findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags required,
                                 VkMemoryPropertyFlags preferred, bool* coherent) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    std::uint32_t timestampValidBits_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    std::string deviceName_;
};

} // namespace image_scopes::tests
