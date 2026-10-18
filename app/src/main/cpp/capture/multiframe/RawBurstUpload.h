#pragma once
// RAW16 bursts uploaded from CPU memory into device-local R16 images, owned
// for the life of the burst (crash-recovery loads and vivo full-size stills).
#include <rawr/zsl_ring/RawImageRing.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "vulkan/VulkanContext.h"

namespace rawrcam::capture::multiframe {
namespace raw_burst_upload_detail {
inline void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("capture_job_vulkan_" + std::to_string(r));
}
}  // namespace raw_burst_upload_detail
class RawBurstUpload final {
   public:
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkBuffer buffer{};
    VkDeviceMemory bufferMemory{};
    void* mapped{};
    struct Image {
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
    };
    std::vector<Image> images;
    RawBurstUpload() = default;
    RawBurstUpload(const RawBurstUpload&) = delete;
    RawBurstUpload& operator=(const RawBurstUpload&) = delete;
    ~RawBurstUpload() {
        for (auto& i : images) {
            if (i.view) vkDestroyImageView(device, i.view, nullptr);
            if (i.image) vkDestroyImage(device, i.image, nullptr);
            if (i.memory) vkFreeMemory(device, i.memory, nullptr);
        }
        if (mapped) vkUnmapMemory(device, bufferMemory);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (bufferMemory) vkFreeMemory(device, bufferMemory, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
    }
    uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("capture_job_memory_type");
    }
    void initialize(const vulkan::VulkanContext& vk, uint64_t bytes) {
        device = vk.device();
        physical = vk.physicalDevice();
        VkCommandPoolCreateInfo p{};
        p.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        p.queueFamilyIndex = vk.queueFamily();
        p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        raw_burst_upload_detail::check(vkCreateCommandPool(device, &p, nullptr, &pool));
        VkCommandBufferAllocateInfo c{};
        c.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        c.commandPool = pool;
        c.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        c.commandBufferCount = 1;
        raw_burst_upload_detail::check(vkAllocateCommandBuffers(device, &c, &command));
        VkFenceCreateInfo f{};
        f.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        raw_burst_upload_detail::check(vkCreateFence(device, &f, nullptr, &fence));
        VkBufferCreateInfo b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        b.size = bytes;
        b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        raw_burst_upload_detail::check(vkCreateBuffer(device, &b, nullptr, &buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo a{};
        a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        a.allocationSize = req.size;
        a.memoryTypeIndex =
            memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        raw_burst_upload_detail::check(vkAllocateMemory(device, &a, nullptr, &bufferMemory));
        raw_burst_upload_detail::check(vkBindBufferMemory(device, buffer, bufferMemory, 0));
        raw_burst_upload_detail::check(vkMapMemory(device, bufferMemory, 0, bytes, 0, &mapped));
    }
    rawr::zsl_ring::GpuRawImageView add(const vulkan::VulkanContext& vk, std::mutex& mutex, uint32_t width,
                                        uint32_t height, uint64_t timestamp, const std::vector<uint8_t>& raw) {
        return add(vk, mutex, width, height, timestamp, raw.data(), raw.size());
    }
    // Packed RAW16 (width * 2 bytes per row) into a new device-local R16 image.
    rawr::zsl_ring::GpuRawImageView add(const vulkan::VulkanContext& vk, std::mutex& mutex, uint32_t width,
                                        uint32_t height, uint64_t timestamp, const uint8_t* raw, size_t rawBytes) {
        images.emplace_back();
        auto& i = images.back();
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16_UINT;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        raw_burst_upload_detail::check(vkCreateImage(device, &ci, nullptr, &i.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, i.image, &req);
        VkMemoryAllocateInfo a{};
        a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        a.allocationSize = req.size;
        a.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        raw_burst_upload_detail::check(vkAllocateMemory(device, &a, nullptr, &i.memory));
        raw_burst_upload_detail::check(vkBindImageMemory(device, i.image, i.memory, 0));
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = i.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16_UINT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        raw_burst_upload_detail::check(vkCreateImageView(device, &vi, nullptr, &i.view));
        std::memcpy(mapped, raw, rawBytes);
        raw_burst_upload_detail::check(vkResetCommandBuffer(command, 0));
        raw_burst_upload_detail::check(vkResetFences(device, 1, &fence));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        raw_burst_upload_detail::check(vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = i.image;
        barrier.subresourceRange = vi.subresourceRange;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(command, buffer, i.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        raw_burst_upload_detail::check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        {
            std::lock_guard<std::mutex> lock(mutex);
            raw_burst_upload_detail::check(vkQueueSubmit(vk.queue(), 1, &submit, fence));
        }
        raw_burst_upload_detail::check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        return {{images.size(), timestamp, width, height}, i.image, i.view, VK_FORMAT_R16_UINT, req.size};
    }
};

}  // namespace rawrcam::capture::multiframe
