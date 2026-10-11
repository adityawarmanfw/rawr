#include "vulkan_test_context.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace image_scopes::tests {
namespace {
void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(what);
}

[[maybe_unused]] bool hasInstanceExtension(const char* name) {
    std::uint32_t count = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
          "vkEnumerateInstanceExtensionProperties count failed");
    std::vector<VkExtensionProperties> props(count);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data()),
          "vkEnumerateInstanceExtensionProperties failed");
    for (const auto& p : props)
        if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}

#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
[[maybe_unused]] bool hasDeviceExtension(VkPhysicalDevice pd, const char* name) {
    std::uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr),
          "vkEnumerateDeviceExtensionProperties count failed");
    std::vector<VkExtensionProperties> props(count);
    check(vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, props.data()),
          "vkEnumerateDeviceExtensionProperties failed");
    for (const auto& p : props)
        if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}
#endif
}  // namespace

VulkanTestContext::VulkanTestContext() {
    std::vector<const char*> instanceExtensions;
    VkInstanceCreateFlags instanceFlags = 0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "image_scopes_validation";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.flags = instanceFlags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
    ici.ppEnabledExtensionNames = instanceExtensions.data();
    check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance failed");

    try {
        std::uint32_t pdCount = 0;
        check(vkEnumeratePhysicalDevices(instance_, &pdCount, nullptr), "vkEnumeratePhysicalDevices count failed");
        if (!pdCount) throw std::runtime_error("no Vulkan physical device");
        std::vector<VkPhysicalDevice> devices(pdCount);
        check(vkEnumeratePhysicalDevices(instance_, &pdCount, devices.data()), "vkEnumeratePhysicalDevices failed");

        for (VkPhysicalDevice pd : devices) {
            std::uint32_t qCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
            std::vector<VkQueueFamilyProperties> qp(qCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, qp.data());
            for (std::uint32_t q = 0; q < qCount; ++q) {
                if (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    physicalDevice_ = pd;
                    queueFamily_ = q;
                    timestampValidBits_ = qp[q].timestampValidBits;
                    break;
                }
            }
            if (physicalDevice_) break;
        }
        if (!physicalDevice_) throw std::runtime_error("no compute-capable Vulkan queue");

        vkGetPhysicalDeviceProperties(physicalDevice_, &properties_);
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
        deviceName_ = properties_.deviceName;

        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
            throw std::runtime_error(
                "VK_FORMAT_R8G8B8A8_UNORM optimal image does not support storage-image access on this device");
        VkImageFormatProperties ifp{};
        const VkResult ifr = vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice_, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &ifp);
        if (ifr != VK_SUCCESS)
            throw std::runtime_error("VK_FORMAT_R8G8B8A8_UNORM optimal storage image is not creatable on this device");

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        std::vector<const char*> deviceExtensions;
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (hasDeviceExtension(physicalDevice_, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            deviceExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
        dci.ppEnabledExtensionNames = deviceExtensions.data();
        check(vkCreateDevice(physicalDevice_, &dci, nullptr, &device_), "vkCreateDevice failed");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily_;
        check(vkCreateCommandPool(device_, &pci, nullptr, &commandPool_), "vkCreateCommandPool failed");

        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = commandPool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &cai, &commandBuffer_), "vkAllocateCommandBuffers failed");

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device_, &fci, nullptr, &fence_), "vkCreateFence failed");
    } catch (...) {
        if (device_) {
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
        throw;
    }
}

VulkanTestContext::~VulkanTestContext() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

std::uint32_t VulkanTestContext::findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags required,
                                                VkMemoryPropertyFlags preferred, bool* coherent) const {
    std::uint32_t fallback = UINT32_MAX;
    for (std::uint32_t i = 0; i < memoryProperties_.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const auto flags = memoryProperties_.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) {
            if (coherent) *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return i;
        }
        if (fallback == UINT32_MAX) fallback = i;
    }
    if (fallback != UINT32_MAX) {
        if (coherent)
            *coherent =
                (memoryProperties_.memoryTypes[fallback].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        return fallback;
    }
    throw std::runtime_error("no suitable Vulkan memory type");
}

Buffer VulkanTestContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                                       VkMemoryPropertyFlags preferred) const {
    Buffer b{};
    b.size = size;
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &ci, nullptr, &b.buffer), "vkCreateBuffer failed");
    try {
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device_, b.buffer, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        b.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, required, preferred, &b.coherent);
        check(vkAllocateMemory(device_, &ai, nullptr, &b.memory), "vkAllocateMemory buffer failed");
        check(vkBindBufferMemory(device_, b.buffer, b.memory, 0), "vkBindBufferMemory failed");
    } catch (...) {
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
        throw;
    }
    return b;
}

void VulkanTestContext::destroyBuffer(Buffer& b) const {
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

void* VulkanTestContext::map(const Buffer& b) const {
    void* p = nullptr;
    check(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory failed");
    return p;
}
void VulkanTestContext::unmap(const Buffer& b) const { vkUnmapMemory(device_, b.memory); }

namespace {
VkMappedMemoryRange mappedRangeFor(const Buffer& b, VkDeviceSize atomSize, VkDeviceSize offset, VkDeviceSize size) {
    if (offset > b.size) throw std::invalid_argument("mapped range offset exceeds buffer size");
    const VkDeviceSize requestedEnd =
        size == VK_WHOLE_SIZE ? b.size : std::min(b.size, offset + std::min(size, b.size - offset));
    const VkDeviceSize alignedOffset = (offset / atomSize) * atomSize;
    VkDeviceSize alignedEnd = ((requestedEnd + atomSize - 1) / atomSize) * atomSize;
    alignedEnd = std::min(alignedEnd, b.allocationSize);
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = b.memory;
    r.offset = alignedOffset;
    r.size = alignedEnd - alignedOffset;
    return r;
}
}  // namespace

void VulkanTestContext::flush(const Buffer& b, VkDeviceSize offset, VkDeviceSize size) const {
    if (b.coherent) return;
    auto r = mappedRangeFor(b, properties_.limits.nonCoherentAtomSize, offset, size);
    check(vkFlushMappedMemoryRanges(device_, 1, &r), "vkFlushMappedMemoryRanges failed");
}
void VulkanTestContext::invalidate(const Buffer& b, VkDeviceSize offset, VkDeviceSize size) const {
    if (b.coherent) return;
    auto r = mappedRangeFor(b, properties_.limits.nonCoherentAtomSize, offset, size);
    check(vkInvalidateMappedMemoryRanges(device_, 1, &r), "vkInvalidateMappedMemoryRanges failed");
}

RgbaImage VulkanTestContext::createRgbaImage(std::uint32_t width, std::uint32_t height, VkFormat format) const {
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R16G16B16A16_SFLOAT &&
        format != VK_FORMAT_R16_UINT)
        throw std::invalid_argument("unsupported test image format");
    RgbaImage out{};
    out.width = width;
    out.height = height;
    out.bytesPerPixel = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : format == VK_FORMAT_R16_UINT ? 2u : 4u;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &ci, nullptr, &out.image), "vkCreateImage failed");
    try {
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device_, out.image, &mr);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, nullptr);
        check(vkAllocateMemory(device_, &ai, nullptr, &out.memory), "vkAllocateMemory image failed");
        check(vkBindImageMemory(device_, out.image, out.memory, 0), "vkBindImageMemory failed");
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = out.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &vi, nullptr, &out.view), "vkCreateImageView failed");
    } catch (...) {
        if (out.view) vkDestroyImageView(device_, out.view, nullptr);
        if (out.image) vkDestroyImage(device_, out.image, nullptr);
        if (out.memory) vkFreeMemory(device_, out.memory, nullptr);
        throw;
    }
    return out;
}

void VulkanTestContext::destroyRgbaImage(RgbaImage& image) const {
    if (image.view) vkDestroyImageView(device_, image.view, nullptr);
    if (image.image) vkDestroyImage(device_, image.image, nullptr);
    if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
    image = {};
}

void VulkanTestContext::beginCommands() {
    check(vkResetCommandBuffer(commandBuffer_, 0), "vkResetCommandBuffer failed");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer_, &bi), "vkBeginCommandBuffer failed");
}

void VulkanTestContext::submitAndWait() {
    check(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer failed");
    check(vkResetFences(device_, 1, &fence_), "vkResetFences failed");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &commandBuffer_;
    check(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit failed");
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences failed");
}

void VulkanTestContext::uploadRgba(RgbaImage& image, std::span<const std::uint8_t> rgba) {
    const VkDeviceSize bytes = VkDeviceSize(image.width) * image.height * image.bytesPerPixel;
    if (rgba.size_bytes() < bytes) throw std::invalid_argument("RGBA upload span too small");
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    try {
        void* mapped = map(staging);
        std::memcpy(mapped, rgba.data(), static_cast<std::size_t>(bytes));
        flush(staging);
        unmap(staging);

        beginCommands();
        VkImageMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        before.srcAccessMask = image.layout == VK_IMAGE_LAYOUT_GENERAL ? VK_ACCESS_SHADER_READ_BIT : 0;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        before.oldLayout = image.layout;
        before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.image = image.image;
        before.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        before.subresourceRange.levelCount = 1;
        before.subresourceRange.layerCount = 1;
        const VkPipelineStageFlags srcStage = image.layout == VK_IMAGE_LAYOUT_GENERAL
                                                  ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        vkCmdPipelineBarrier(commandBuffer_, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &before);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {image.width, image.height, 1};
        vkCmdCopyBufferToImage(commandBuffer_, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);

        VkImageMemoryBarrier after{};
        after.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        after.srcQueueFamilyIndex = after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after.image = image.image;
        after.subresourceRange = before.subresourceRange;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &after);
        submitAndWait();
        image.layout = VK_IMAGE_LAYOUT_GENERAL;
    } catch (...) {
        destroyBuffer(staging);
        throw;
    }
    destroyBuffer(staging);
}

std::vector<std::uint8_t> VulkanTestContext::downloadRgba(RgbaImage& image) {
    const VkDeviceSize bytes = VkDeviceSize(image.width) * image.height * image.bytesPerPixel;
    Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    try {
        beginCommands();
        VkImageMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        before.oldLayout = image.layout;
        before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        before.srcQueueFamilyIndex = before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.image = image.image;
        before.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        before.subresourceRange.levelCount = 1;
        before.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &before);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {image.width, image.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer_, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1,
                               &copy);

        VkImageMemoryBarrier after{};
        after.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        after.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        after.srcQueueFamilyIndex = after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        after.image = image.image;
        after.subresourceRange = before.subresourceRange;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &after);
        submitAndWait();
        image.layout = VK_IMAGE_LAYOUT_GENERAL;

        std::vector<std::uint8_t> out(static_cast<std::size_t>(bytes));
        void* mapped = map(staging);
        invalidate(staging);
        std::memcpy(out.data(), mapped, out.size());
        unmap(staging);
        destroyBuffer(staging);
        return out;
    } catch (...) {
        destroyBuffer(staging);
        throw;
    }
}

VkQueryPool VulkanTestContext::createTimestampQueryPool(std::uint32_t count) const {
    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = count;
    VkQueryPool p = VK_NULL_HANDLE;
    check(vkCreateQueryPool(device_, &ci, nullptr, &p), "vkCreateQueryPool failed");
    return p;
}
void VulkanTestContext::destroyQueryPool(VkQueryPool pool) const {
    if (pool) vkDestroyQueryPool(device_, pool, nullptr);
}

double VulkanTestContext::timestampDeltaMs(VkQueryPool pool, std::uint32_t first, std::uint32_t second) const {
    std::uint64_t firstValue = 0;
    std::uint64_t secondValue = 0;
    check(vkGetQueryPoolResults(device_, pool, first, 1, sizeof(firstValue), &firstValue, sizeof(std::uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "vkGetQueryPoolResults(first) failed");
    check(vkGetQueryPoolResults(device_, pool, second, 1, sizeof(secondValue), &secondValue, sizeof(std::uint64_t),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "vkGetQueryPoolResults(second) failed");
    const std::uint64_t mask = timestampValidBits_ >= 64 ? UINT64_MAX : ((1ull << timestampValidBits_) - 1ull);
    const std::uint64_t delta = (secondValue - firstValue) & mask;
    return static_cast<double>(delta) * static_cast<double>(properties_.limits.timestampPeriod) / 1.0e6;
}

}  // namespace image_scopes::tests
