#include <rawr/raw_multiframe_output/MultiframeOutputAdapter.h>

#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "multiframe_cfa_project.h"
#include "multiframe_cfa_project_f32.h"
#include "multiframe_packed_cfa.h"
#include "multiframe_rgb_prepare.h"

namespace rawr::raw_multiframe_output {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(result));
    }
}

struct ProjectionPush {
    float blackByPhase[4];
    float whiteLevel;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t cfa;
    float codeScale;
};
static_assert(sizeof(ProjectionPush) == 36);

// 1A RGB-prepare push block. Field order mirrors prepare_rgb.comp P{}:
// 8 uints, black vec4, invRange vec4, threshold float. Keep under the
// 128-byte guaranteed push-constant limit; the shared pipeline layout
// sizes to this (projection's 32B block still fits).
struct RgbPreparePush {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t lscWidth;
    std::uint32_t lscHeight;
    std::uint32_t refWidth;
    std::uint32_t refHeight;
    std::uint32_t cfa;
    std::uint32_t useSensorClip;
    float black[4];
    float invRange[4];
    float clipThreshold;
    std::uint32_t pad0;
    std::uint32_t pad1;
    std::uint32_t pad2;
};
static_assert(sizeof(RgbPreparePush) == 80);
static_assert(sizeof(RgbPreparePush) <= 128);

}  // namespace

std::uint32_t MultiframeOutputAdapter::memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    throw std::runtime_error("multiframe output: no matching memory type");
}

void MultiframeOutputAdapter::initialize(VkPhysicalDevice physical, VkDevice device, std::uint32_t queueFamily,
                                         Submit submit, std::uint32_t width, std::uint32_t height) {
    reset();
    if (!physical || !device || !submit || width == 0 || height == 0) {
        throw std::invalid_argument("multiframe output: invalid initialize");
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * sizeof(std::uint16_t);
    if (bytes > std::numeric_limits<VkDeviceSize>::max() || bytes > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("multiframe output: RAW16 size overflow");
    }
    physical_ = physical;
    device_ = device;
    queueFamily_ = queueFamily;
    submit_ = std::move(submit);
    width_ = width;
    height_ = height;
    readbackBytes_ = static_cast<VkDeviceSize>(bytes);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16_UINT;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &imageInfo, nullptr, &projection_), "multiframe output create projection image");
    VkMemoryRequirements imageRequirements{};
    vkGetImageMemoryRequirements(device_, projection_, &imageRequirements);
    VkMemoryAllocateInfo imageAllocation{};
    imageAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAllocation.allocationSize = imageRequirements.size;
    imageAllocation.memoryTypeIndex = memoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &imageAllocation, nullptr, &projectionMemory_),
          "multiframe output allocate projection image");
    check(vkBindImageMemory(device_, projection_, projectionMemory_, 0), "multiframe output bind projection image");
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = projection_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16_UINT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    check(vkCreateImageView(device_, &viewInfo, nullptr, &projectionView_), "multiframe output create projection view");

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = readbackBytes_;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &bufferInfo, nullptr, &readback_), "multiframe output create readback buffer");
    VkMemoryRequirements bufferRequirements{};
    vkGetBufferMemoryRequirements(device_, readback_, &bufferRequirements);
    VkMemoryAllocateInfo bufferAllocation{};
    bufferAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufferAllocation.allocationSize = bufferRequirements.size;
    bufferAllocation.memoryTypeIndex = memoryType(
        bufferRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &bufferAllocation, nullptr, &readbackMemory_),
          "multiframe output allocate readback buffer");
    check(vkBindBufferMemory(device_, readback_, readbackMemory_, 0), "multiframe output bind readback buffer");
    check(vkMapMemory(device_, readbackMemory_, 0, readbackBytes_, 0, &mapped_),
          "multiframe output map readback buffer");

    VkDescriptorSetLayoutBinding bindings[5]{};
    for (std::uint32_t i = 0; i < 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 5;
    descriptorLayoutInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device_, &descriptorLayoutInfo, nullptr, &descriptorLayout_),
          "multiframe output descriptor layout");
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(RgbPreparePush);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    check(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
          "multiframe output pipeline layout");
    const auto makeProjectionPipeline = [&](const unsigned char* spv, std::size_t spvSize, VkPipeline* out) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spvSize;
        moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(spv);
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module), "multiframe output shader module");
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = pipelineLayout_;
        const VkResult pipelineResult = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, out);
        vkDestroyShaderModule(device_, module, nullptr);
        check(pipelineResult, "multiframe output compute pipeline");
    };
    makeProjectionPipeline(multiframe_cfa_project_spv, multiframe_cfa_project_spv_size, &pipeline_);
    makeProjectionPipeline(multiframe_cfa_project_f32_spv, multiframe_cfa_project_f32_spv_size, &pipelineF32_);

    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_), "multiframe output descriptor pool");
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorLayout_;
    check(vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_), "multiframe output descriptor set");

    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = queueFamily_;
    check(vkCreateCommandPool(device_, &commandPoolInfo, nullptr, &commandPool_), "multiframe output command pool");
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool_;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device_, &commandInfo, &command_), "multiframe output command buffer");
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device_, &fenceInfo, nullptr, &fence_), "multiframe output fence");
}

void MultiframeOutputAdapter::recordReadback(VkImage source, bool sourceIsProjection) {
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = sourceIsProjection ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = source;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(command_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &copy);
    VkBufferMemoryBarrier hostBarrier{};
    hostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = readback_;
    hostBarrier.size = readbackBytes_;
    VkImageMemoryBarrier toGeneral = toTransfer;
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGeneral.dstAccessMask = sourceIsProjection ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                         &hostBarrier, 1, &toGeneral);
}

PackedRaw16 MultiframeOutputAdapter::copyMapped() const {
    PackedRaw16 out{};
    out.width = width_;
    out.height = height_;
    out.rowStrideBytes = width_ * sizeof(std::uint16_t);
    out.pixels.resize(static_cast<std::size_t>(readbackBytes_));
    std::memcpy(out.pixels.data(), mapped_, out.pixels.size());
    return out;
}

PackedRaw16 MultiframeOutputAdapter::readBase(VkImage rawR16Uint) {
    if (!device_ || !rawR16Uint) {
        throw std::invalid_argument("multiframe output: invalid base image");
    }
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "multiframe output prior fence");
    check(vkResetFences(device_, 1, &fence_), "multiframe output reset fence");
    check(vkResetCommandBuffer(command_, 0), "multiframe output reset command");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command_, &begin), "multiframe output begin base");
    recordReadback(rawR16Uint, false);
    check(vkEndCommandBuffer(command_), "multiframe output end base");
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command_;
    submit_(submitInfo, fence_);
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "multiframe output base fence");
    return copyMapped();
}

PackedRaw16 MultiframeOutputAdapter::projectMerged(VkImage mergedImage, VkImageView mergedView,
                                                   const CfaProjectionParameters& parameters) {
    if (!device_ || !mergedImage || !mergedView || parameters.cfa > 3u ||
        !(std::isfinite(parameters.codeScale) && parameters.codeScale >= 1.0f)) {
        throw std::invalid_argument("multiframe output: invalid merged image");
    }
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "multiframe output prior fence");
    VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, mergedView, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, projectionView_, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    check(vkResetFences(device_, 1, &fence_), "multiframe output reset fence");
    check(vkResetCommandBuffer(command_, 0), "multiframe output reset command");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command_, &begin), "multiframe output begin projection");
    VkMemoryBarrier sourceBarrier{};
    sourceBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    sourceBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &sourceBarrier, 0, nullptr, 0, nullptr);
    if (!projectionInitialized_) {
        VkImageMemoryBarrier initialize{};
        initialize.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        initialize.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        initialize.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        initialize.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        initialize.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        initialize.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        initialize.image = projection_;
        initialize.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        initialize.subresourceRange.levelCount = 1;
        initialize.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &initialize);
        projectionInitialized_ = true;
    }
    ProjectionPush push{};
    std::memcpy(push.blackByPhase, parameters.blackByPhase.data(), sizeof(push.blackByPhase));
    push.whiteLevel = parameters.whiteLevel;
    push.width = width_;
    push.height = height_;
    push.cfa = parameters.cfa;
    push.codeScale = parameters.codeScale;
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, parameters.sourceIsCfaR32f ? pipelineF32_ : pipeline_);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0,
                            nullptr);
    vkCmdPushConstants(command_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command_, (width_ + 15u) / 16u, (height_ + 15u) / 16u, 1);
    recordReadback(projection_, true);
    check(vkEndCommandBuffer(command_), "multiframe output end projection");
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command_;
    submit_(submitInfo, fence_);
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "multiframe output projection fence");
    return copyMapped();
}

void MultiframeOutputAdapter::prepareRgb(VkImageView mergedView, std::uint32_t lscWidth,
                                         std::uint32_t lscHeight, const std::vector<float>& lscGains,
                                         const SensorClipParams& sensorClip) {
    prepare(mergedView, lscWidth, lscHeight, lscGains, sensorClip, false);
}

void MultiframeOutputAdapter::preparePackedCfa(VkImageView mergedView, std::uint32_t lscWidth,
                                               std::uint32_t lscHeight, const std::vector<float>& lscGains,
                                               const SensorClipParams& sensorClip) {
    prepare(mergedView, lscWidth, lscHeight, lscGains, sensorClip, true);
}

void MultiframeOutputAdapter::prepare(VkImageView mergedView, std::uint32_t lscWidth, std::uint32_t lscHeight,
                                      const std::vector<float>& lscGains, const SensorClipParams& sensorClip,
                                      bool packedCfa) {
    if (!device_ || !mergedView || rgb_) throw std::invalid_argument("multiframe RGB: invalid prepare");
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "multiframe RGB prior fence");
    sensorClipOrApplied_ = false;
    const bool hasLsc = lscWidth >= 2 && lscHeight >= 2 &&
        lscGains.size() == std::size_t(lscWidth) * lscHeight * 4 &&
        std::all_of(lscGains.begin(), lscGains.end(), [](float g) { return std::isfinite(g) && g > 0.f; });
    const float unity[4] = {1, 1, 1, 1};
    const VkDeviceSize bytes = hasLsc ? lscGains.size() * sizeof(float) : sizeof(unity);
    // The still demosaicers read their packed CFA input through a sampler.
    const VkImageUsageFlags sampled = packedCfa ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u;
    auto createImage = [&](VkFormat format, uint32_t w, uint32_t h, VkImage& image,
                           VkDeviceMemory& memory, VkImageView& view) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {w, h, 1};
        ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   (format == VK_FORMAT_R16G16B16A16_SFLOAT ? sampled : 0u);
        check(vkCreateImage(device_, &ci, nullptr, &image), "multiframe RGB image");
        VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device_, image, &mr);
        VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size; ai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &ai, nullptr, &memory), "multiframe RGB memory");
        check(vkBindImageMemory(device_, image, memory, 0), "multiframe RGB bind");
        VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &vi, nullptr, &view), "multiframe RGB view");
    };
    // Packed CFA: one texel per 2x2 cell (the still demosaicer's input).
    if (packedCfa && ((width_ | height_) & 1u)) throw std::invalid_argument("multiframe packed CFA: odd geometry");
    createImage(VK_FORMAT_R16G16B16A16_SFLOAT, packedCfa ? width_ / 2 : width_, packedCfa ? height_ / 2 : height_,
                rgb_, rgbMemory_, rgbView_);
    createImage(VK_FORMAT_R16_UINT, (width_ + 1) / 2, (height_ + 1) / 2, clip_, clipMemory_, clipView_);
    VkBufferCreateInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    check(vkCreateBuffer(device_, &bi, nullptr, &lscBuffer_), "multiframe RGB LSC buffer");
    VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(device_, lscBuffer_, &mr);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &ai, nullptr, &lscMemory_), "multiframe RGB LSC memory");
    check(vkBindBufferMemory(device_, lscBuffer_, lscMemory_, 0), "multiframe RGB LSC bind");
    void* mapped = nullptr;
    check(vkMapMemory(device_, lscMemory_, 0, bytes, 0, &mapped), "multiframe RGB LSC map");
    std::memcpy(mapped, hasLsc ? lscGains.data() : unity, std::size_t(bytes));
    vkUnmapMemory(device_, lscMemory_);
    VkShaderModuleCreateInfo mi{}; mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = packedCfa ? multiframe_packed_cfa_spv_size : multiframe_rgb_prepare_spv_size;
    mi.pCode = reinterpret_cast<const uint32_t*>(packedCfa ? multiframe_packed_cfa_spv : multiframe_rgb_prepare_spv);
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_, &mi, nullptr, &module), "multiframe RGB shader");
    VkComputePipelineCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pi.stage.module = module; pi.stage.pName = "main";
    pi.layout = pipelineLayout_;
    const auto pipelineResult = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &rgbPipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    check(pipelineResult, "multiframe RGB pipeline");
    // NOTE: descriptorSet_ is shared by the CFA-projection and RGB-prepare
    // pipelines (layout has 5 bindings; CFA uses 0..1, RGB uses 0..4).
    // releaseDngResources destroys projectionView_ while the set still points
    // at it; safe only because every use overwrites bindings first via
    // vkUpdateDescriptorSets and the prior fence is waited. Do not submit
    // without a preceding update. Binding 4 carries the optional reference
    // RAW (1A sensor OR); a null view disables the sensor term in-shader,
    // so legacy callers keep derived-only behavior bit-for-bit.
    const bool useSensorClip =
        sensorClip.refRawView != VK_NULL_HANDLE && sensorClip.refWidth == width_ &&
        sensorClip.refHeight == height_ && sensorClip.refWidth >= 2 && sensorClip.refHeight >= 2 &&
        sensorClip.cfa <= 3u && std::isfinite(sensorClip.whiteLevel) && std::isfinite(sensorClip.clipThreshold);
    VkDescriptorImageInfo images[5] = {{VK_NULL_HANDLE, mergedView, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, rgbView_, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, clipView_, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                                       // Placeholder must be a valid R16U view when the
                                       // sensor term is disabled (shader never loads it).
                                       {VK_NULL_HANDLE,
                                        useSensorClip ? sensorClip.refRawView : clipView_, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo buffer{lscBuffer_, 0, bytes};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (i == 3) writes[i].pBufferInfo = &buffer; else writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
    check(vkResetCommandBuffer(command_, 0), "multiframe RGB reset command");
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command_, &begin), "multiframe RGB begin");
    VkImageMemoryBarrier barriers[2]{};
    for (int i = 0; i < 2; ++i) {
        auto& b = barriers[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL; b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = i == 0 ? rgb_ : clip_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkMemoryBarrier ready{}; ready.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    ready.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // UNDEFINED -> GENERAL for fresh images: no prior accesses, so use
    // TOP_OF_PIPE as source stage (spec-correct; COMPUTE->COMPUTE here is a
    // validation-layer violation even though benign on mobile).
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &ready, 0, nullptr, 2, barriers);
    const uint32_t pushLscW = hasLsc ? lscWidth : 1u;
    const uint32_t pushLscH = hasLsc ? lscHeight : 1u;
    RgbPreparePush push{};
    push.width = width_;
    push.height = height_;
    push.lscWidth = pushLscW;
    push.lscHeight = pushLscH;
    push.refWidth = useSensorClip ? sensorClip.refWidth : 0u;
    push.refHeight = useSensorClip ? sensorClip.refHeight : 0u;
    push.cfa = useSensorClip ? sensorClip.cfa : 0u;
    push.useSensorClip = useSensorClip ? 1u : 0u;
    for (int i = 0; i < 4; ++i) {
        const float black = useSensorClip ? sensorClip.blackByPhase[static_cast<std::size_t>(i)] : 0.0f;
        push.black[i] = std::isfinite(black) ? black : 0.0f;
        const float denom =
            useSensorClip ? (sensorClip.whiteLevel - push.black[i]) : 1.0f;
        push.invRange[i] = (std::isfinite(denom) && denom > 0.0f) ? (1.0f / denom) : 1.0f;
    }
    push.clipThreshold =
        (useSensorClip && std::isfinite(sensorClip.clipThreshold)) ? sensorClip.clipThreshold : 0.995f;
    push.pad0 = 0u;
    push.pad1 = 0u;
    push.pad2 = 0u;
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, rgbPipeline_);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(command_, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(command_, (width_ + 31) / 32, (height_ + 31) / 32, 1);
    ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &ready, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(command_), "multiframe RGB end");
    check(vkResetFences(device_, 1, &fence_), "multiframe RGB reset fence");
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &command_;
    submit_(si, fence_);
    check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "multiframe RGB fence");
    sensorClipOrApplied_ = useSensorClip;
}

void MultiframeOutputAdapter::releaseDngResources() noexcept {
    if (device_ && mapped_) vkUnmapMemory(device_, readbackMemory_);
    if (device_ && readback_) vkDestroyBuffer(device_, readback_, nullptr);
    if (device_ && readbackMemory_) vkFreeMemory(device_, readbackMemory_, nullptr);
    if (device_ && projectionView_) vkDestroyImageView(device_, projectionView_, nullptr);
    if (device_ && projection_) vkDestroyImage(device_, projection_, nullptr);
    if (device_ && projectionMemory_) vkFreeMemory(device_, projectionMemory_, nullptr);
    mapped_ = nullptr; readback_ = VK_NULL_HANDLE; readbackMemory_ = VK_NULL_HANDLE;
    projectionView_ = VK_NULL_HANDLE; projection_ = VK_NULL_HANDLE; projectionMemory_ = VK_NULL_HANDLE;
}

void MultiframeOutputAdapter::reset() noexcept {
    if (device_ && fence_) (void)vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    releaseDngResources();
    if (device_ && rgbView_) vkDestroyImageView(device_, rgbView_, nullptr);
    if (device_ && clipView_) vkDestroyImageView(device_, clipView_, nullptr);
    if (device_ && rgb_) vkDestroyImage(device_, rgb_, nullptr);
    if (device_ && clip_) vkDestroyImage(device_, clip_, nullptr);
    if (device_ && rgbMemory_) vkFreeMemory(device_, rgbMemory_, nullptr);
    if (device_ && clipMemory_) vkFreeMemory(device_, clipMemory_, nullptr);
    if (device_ && lscBuffer_) vkDestroyBuffer(device_, lscBuffer_, nullptr);
    if (device_ && lscMemory_) vkFreeMemory(device_, lscMemory_, nullptr);
    if (device_ && rgbPipeline_) vkDestroyPipeline(device_, rgbPipeline_, nullptr);
    rgbView_ = clipView_ = VK_NULL_HANDLE; rgb_ = clip_ = VK_NULL_HANDLE;
    rgbMemory_ = clipMemory_ = lscMemory_ = VK_NULL_HANDLE;
    lscBuffer_ = VK_NULL_HANDLE; rgbPipeline_ = VK_NULL_HANDLE;
    sensorClipOrApplied_ = false;
    if (device_ && fence_) vkDestroyFence(device_, fence_, nullptr);
    if (device_ && commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (device_ && descriptorPool_) vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (device_ && pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (device_ && pipelineF32_) vkDestroyPipeline(device_, pipelineF32_, nullptr);
    if (device_ && pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (device_ && descriptorLayout_) vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    if (device_ && readback_) vkDestroyBuffer(device_, readback_, nullptr);
    if (device_ && readbackMemory_) vkFreeMemory(device_, readbackMemory_, nullptr);
    if (device_ && projectionView_) vkDestroyImageView(device_, projectionView_, nullptr);
    if (device_ && projection_) vkDestroyImage(device_, projection_, nullptr);
    if (device_ && projectionMemory_) vkFreeMemory(device_, projectionMemory_, nullptr);
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
    submit_ = {};
    width_ = 0;
    height_ = 0;
    projection_ = VK_NULL_HANDLE;
    projectionMemory_ = VK_NULL_HANDLE;
    projectionView_ = VK_NULL_HANDLE;
    projectionInitialized_ = false;
    readback_ = VK_NULL_HANDLE;
    readbackMemory_ = VK_NULL_HANDLE;
    mapped_ = nullptr;
    readbackBytes_ = 0;
    descriptorLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipelineF32_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    command_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
}

}  // namespace rawr::raw_multiframe_output
