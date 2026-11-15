#include "capture/ReducedRaw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "capture/ReducedRawMetadata.h"
#include "diagnostics/timing/CaptureStageTimer.h"
namespace rawrcam::capture {
namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("reduced_raw_vulkan_" + std::to_string(r));
}
}  // namespace
ReducedRaw::ReducedRaw(const vulkan::VulkanContext& vk, uint32_t w, uint32_t h, uint32_t slots)
    : vk_(vk), width_(w), height_(h), bytes_(size_t(w) * h * 2) {
    try {
        initialize(slots);
    } catch (...) {
        destroy();
        throw;
    }
}
ReducedRaw::~ReducedRaw() { destroy(); }
std::string ReducedRaw::deviceUnsupportedReason(const vulkan::VulkanContext& vk, uint32_t width, uint32_t height) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(vk.physicalDevice(), &props);
    if (size_t(width) * height * 2 > props.limits.maxStorageBufferRange) return "storage_buffer_range";
    if (width / 2 > props.limits.maxImageDimension2D || height / 2 > props.limits.maxImageDimension2D)
        return "image_dimension";
    return {};
}
void ReducedRaw::allocate(Buffer& b, size_t bytes, VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = bytes;
    ci.usage = usage;
    check(vkCreateBuffer(vk_.device(), &ci, nullptr, &b.buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(vk_.device(), b.buffer, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(vk_.physicalDevice(), &props);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = UINT32_MAX;
    constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            ai.memoryTypeIndex = i;
            break;
        }
    if (ai.memoryTypeIndex == UINT32_MAX) throw std::runtime_error("reduced_raw_memory_type");
    check(vkAllocateMemory(vk_.device(), &ai, nullptr, &b.memory));
    check(vkBindBufferMemory(vk_.device(), b.buffer, b.memory, 0));
    check(vkMapMemory(vk_.device(), b.memory, 0, bytes, 0, &b.mapped));
}
void ReducedRaw::initialize(uint32_t count) {
    if (!width_ || !height_ || width_ % 8 || height_ % 8 || count < 1 || count > 2)
        throw std::runtime_error("reduced_raw_geometry");
    if (!deviceUnsupportedReason(vk_, width_, height_).empty()) throw std::runtime_error("reduced_raw_device_limit");
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = vk_.queueFamily();
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(vk_.device(), &pi, nullptr, &pool_));
    for (uint32_t i = 0; i < count; ++i) {
        auto& s = slots_[i];
        allocate(s.source, bytes_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        allocate(s.result, bytes_ / 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        s.output =
            rawr::vk::createOwnedImage(vk_.physicalDevice(), vk_.device(), width_ / 2, height_ / 2, VK_FORMAT_R16_UINT,
                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(vk_.device(), &ai, &s.command));
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(vk_.device(), &fi, nullptr, &s.fence));
    }
    remosaic_ = rawr::raw_ingress::RawRemosaicker::create(vk_.gpuContext());
}
void ReducedRaw::submit(uint32_t index, const std::vector<uint8_t>& pixels, const metadata::FrameMetadataSnapshot& m) {
    diagnostics::CaptureStageTimer timing("reduced_upload_and_submit", m.timestampNs, index);
    auto& s = slots_.at(index);
    if (!s.command || s.pending || pixels.size() != bytes_ || !m.cameraContext ||
        !reducedRawRequested(*m.cameraContext))
        throw std::runtime_error("reduced_raw_submit_invalid");
    const auto& c = *m.cameraContext;
    // A frame whose levels are too deep for the fit still reduces, without it.
    const bool fit = c.photoPhaseFit && reducedRawFitFits(m.effectiveWhiteLevel);
    const float limit = fit ? kReducedRawMaxFitCode : kReducedRawMaxCode;
    if (!std::isfinite(m.effectiveWhiteLevel) || m.effectiveWhiteLevel <= 0 || m.effectiveWhiteLevel > limit)
        throw std::runtime_error("reduced_raw_unsupported_white_level");
    // Copy and validate in one pass; phase-fit integer statistics require bounded source codes.
    auto* out = static_cast<uint16_t*>(s.source.mapped);
    const auto* in = reinterpret_cast<const uint16_t*>(pixels.data());
    uint16_t maximum = 0;
    for (size_t i = 0; i < bytes_ / 2; ++i) {
        out[i] = in[i];
        maximum = std::max(maximum, in[i]);
    }
    if (maximum > limit) throw std::runtime_error("reduced_raw_source_code_out_of_range");
    check(vkResetCommandBuffer(s.command, 0));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(s.command, &bi));
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.output.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    rawr::raw_ingress::RawRemosaicker::Frame f;
    f.source = s.source.buffer;
    f.destination = s.output.view;
    f.width = width_;
    f.height = height_;
    f.rowStrideBytes = width_ * 2;
    f.block = 4;
    f.order = c.rawPreviewCfa;
    f.quality = rawr::raw_ingress::RawRemosaicker::Quality::Detail;
    f.hexadecaToQuad = true;
    f.phaseFit = fit;
    f.blackLevel = (m.blackLevelPhysicalRggb[0] + m.blackLevelPhysicalRggb[1] + m.blackLevelPhysicalRggb[2] +
                    m.blackLevelPhysicalRggb[3]) *
                   .25f;
    remosaic_->record(s.command, index, f);
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_ / 2, height_ / 2, 1};
    vkCmdCopyImageToBuffer(s.command, s.output.image, VK_IMAGE_LAYOUT_GENERAL, s.result.buffer, 1, &region);
    VkMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                         0, nullptr);
    check(vkEndCommandBuffer(s.command));
    check(vkResetFences(vk_.device(), 1, &s.fence));
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.command;
    {
        std::lock_guard<std::mutex> lock(vk_.mfSubmitMutex());
        check(vkQueueSubmit(vk_.multiframeQueue(), 1, &si, s.fence));
    }
    s.pending = true;
}
void ReducedRaw::collect(uint32_t index, std::vector<uint8_t>& pixels) {
    diagnostics::CaptureStageTimer timing("reduced_wait_and_readback", 0, index);
    auto& s = slots_.at(index);
    if (!s.pending) throw std::runtime_error("reduced_raw_collect_without_submit");
    check(vkWaitForFences(vk_.device(), 1, &s.fence, VK_TRUE, UINT64_MAX));
    pixels.resize(bytes_ / 4);
    std::memcpy(pixels.data(), s.result.mapped, pixels.size());
    s.pending = false;
}
void ReducedRaw::destroy() noexcept {
    for (auto& s : slots_)
        if (s.pending) vkWaitForFences(vk_.device(), 1, &s.fence, VK_TRUE, UINT64_MAX);
    remosaic_.reset();
    for (auto& s : slots_) {
        rawr::vk::destroyOwnedImage(vk_.device(), s.output);
        for (auto* b : {&s.source, &s.result}) {
            if (b->mapped) vkUnmapMemory(vk_.device(), b->memory);
            if (b->buffer) vkDestroyBuffer(vk_.device(), b->buffer, nullptr);
            if (b->memory) vkFreeMemory(vk_.device(), b->memory, nullptr);
        }
        if (s.fence) vkDestroyFence(vk_.device(), s.fence, nullptr);
    }
    if (pool_) vkDestroyCommandPool(vk_.device(), pool_, nullptr);
}
}  // namespace rawrcam::capture
