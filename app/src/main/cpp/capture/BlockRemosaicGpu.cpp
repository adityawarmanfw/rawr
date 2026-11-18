#include "capture/BlockRemosaicGpu.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace rawrcam::capture {
namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("block_remosaic_vulkan_" + std::to_string(r));
}
}  // namespace
BlockRemosaicGpu::BlockRemosaicGpu(const vulkan::VulkanContext& vk, uint32_t w, uint32_t h, uint32_t strips)
    : vk_(vk), width_(w), height_(h), frameBytes_(size_t(w) * h * 2) {
    try {
        if (!w || !h || w % 2 || h % 2) throw std::runtime_error("block_remosaic_geometry");
        layout_ = StripLayout::make(h, strips);
        windowHeight_ = layout_.window;
        bytes_ = size_t(w) * windowHeight_ * 2;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(vk_.physicalDevice(), &props);
        if (bytes_ > props.limits.maxStorageBufferRange || w > props.limits.maxImageDimension2D ||
            windowHeight_ > props.limits.maxImageDimension2D)
            throw std::runtime_error("block_remosaic_device_limit");
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes_;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(vk_.device(), &bi, nullptr, &buffer_));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(vk_.device(), buffer_, &req);
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(vk_.physicalDevice(), &mem);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = UINT32_MAX;
        constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & flags) == flags) {
                ai.memoryTypeIndex = i;
                break;
            }
        if (ai.memoryTypeIndex == UINT32_MAX) throw std::runtime_error("block_remosaic_memory_type");
        check(vkAllocateMemory(vk_.device(), &ai, nullptr, &memory_));
        check(vkBindBufferMemory(vk_.device(), buffer_, memory_, 0));
        check(vkMapMemory(vk_.device(), memory_, 0, bytes_, 0, &mapped_));
        output_ = rawr::vk::createOwnedImage(vk_.physicalDevice(), vk_.device(), w, windowHeight_, VK_FORMAT_R16_UINT,
                                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = vk_.queueFamily();
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(vk_.device(), &pi, nullptr, &pool_));
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = pool_;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(vk_.device(), &ci, &command_));
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(vk_.device(), &fi, nullptr, &fence_));
        remosaic_ = rawr::raw_ingress::RawRemosaicker::create(vk_.gpuContext());
    } catch (...) {
        destroy();
        throw;
    }
}
BlockRemosaicGpu::~BlockRemosaicGpu() { destroy(); }
bool BlockRemosaicGpu::supports(uint32_t block, imaging::CfaRemosaicQuality quality) {
    using Q = imaging::CfaRemosaicQuality;
    if (block == 2u) return true;
    return block == 4u && (quality == Q::Fast || quality == Q::Standard);
}
void BlockRemosaicGpu::run(std::vector<uint8_t>& pixels, uint32_t block, uint32_t order,
                           imaging::CfaRemosaicQuality quality, float black) {
    if (pixels.size() != frameBytes_ || !supports(block, quality)) throw std::runtime_error("block_remosaic_invalid");
    // The window is the mapped buffer: the pass runs on it in place.
    remosaicInStrips(pixels, width_, height_, layout_, static_cast<uint8_t*>(mapped_),
                     [&](uint8_t*, uint32_t strip) {
                         try {
                             runWindow(block, order, quality, black);
                         } catch (...) {
                             if (strip == 0) throw;
                             throw PartialFailure("block_remosaic_strip_failed");
                         }
                     });
}
void BlockRemosaicGpu::runWindow(uint32_t block, uint32_t order, imaging::CfaRemosaicQuality quality, float black) {
    using Q = imaging::CfaRemosaicQuality;
    check(vkResetCommandBuffer(command_, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command_, &bi));
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = output_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    rawr::raw_ingress::RawRemosaicker::Frame f;
    f.source = buffer_;
    f.destination = output_.view;
    f.width = width_;
    f.height = windowHeight_;
    f.rowStrideBytes = width_ * 2;
    f.block = block;
    f.order = order;
    f.quality = quality == Q::Detail     ? rawr::raw_ingress::RawRemosaicker::Quality::Detail
                : quality == Q::High     ? rawr::raw_ingress::RawRemosaicker::Quality::High
                : quality == Q::Fast     ? rawr::raw_ingress::RawRemosaicker::Quality::Fast
                                         : rawr::raw_ingress::RawRemosaicker::Quality::Standard;
    f.blackLevel = black;
    remosaic_->record(command_, 0, f);
    // The shader reads the source buffer only in its first (copy) pass, so the
    // Bayer result goes back into the same buffer.
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, windowHeight_, 1};
    vkCmdCopyImageToBuffer(command_, output_.image, VK_IMAGE_LAYOUT_GENERAL, buffer_, 1, &region);
    VkMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                         0, nullptr);
    check(vkEndCommandBuffer(command_));
    check(vkResetFences(vk_.device(), 1, &fence_));
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command_;
    {
        std::lock_guard<std::mutex> lock(vk_.mfSubmitMutex());
        check(vkQueueSubmit(vk_.multiframeQueue(), 1, &si, fence_));
    }
    check(vkWaitForFences(vk_.device(), 1, &fence_, VK_TRUE, UINT64_MAX));
}
void BlockRemosaicGpu::destroy() noexcept {
    // A run is synchronous, so nothing is in flight here.
    remosaic_.reset();
    if (fence_) vkDestroyFence(vk_.device(), fence_, nullptr);
    if (pool_) vkDestroyCommandPool(vk_.device(), pool_, nullptr);
    rawr::vk::destroyOwnedImage(vk_.device(), output_);
    if (mapped_) vkUnmapMemory(vk_.device(), memory_);
    if (buffer_) vkDestroyBuffer(vk_.device(), buffer_, nullptr);
    if (memory_) vkFreeMemory(vk_.device(), memory_, nullptr);
    fence_ = {};
    pool_ = {};
    mapped_ = nullptr;
    buffer_ = {};
    memory_ = {};
}
}  // namespace rawrcam::capture
