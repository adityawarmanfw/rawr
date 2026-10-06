#include "video_pipeline/VideoDemosaic.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "video_demosaic.h"
#include "video_downscale.h"
#include "video_pipeline/VideoCrop.h"

namespace rawrcam::video {
namespace {
constexpr VkDeviceSize kMaxLscBytes = 64 * 1024;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " VkResult=" + std::to_string(result));
}

uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("video demosaic host memory unavailable");
}

struct Push {
    float black[4];
    float invRange[4];
    float wb[4];
    uint32_t width, height, outWidth, outHeight;
    uint32_t cropX, cropY, pattern, stridePixels;
    uint32_t bufferEnabled, reduceCfa, lscEnabled, lscWidth, lscHeight, monitorEnabled;
};
static_assert(sizeof(Push) == 104);
}  // namespace

VideoDemosaic::VideoDemosaic(const rawr::vk::GpuContext& context, uint32_t rawWidth, uint32_t rawHeight,
                             uint32_t outputWidth, uint32_t outputHeight)
    : context_(context),
      rawWidth_(rawWidth),
      rawHeight_(rawHeight),
      outputWidth_(outputWidth),
      outputHeight_(outputHeight) {
    if (!context_.device || rawWidth < 2 || rawHeight < 2 || outputWidth == 0 || outputHeight == 0 ||
        (outputWidth & 1u) != 0u || (outputHeight & 1u) != 0u)
        throw std::invalid_argument("video demosaic geometry/device invalid");
    // Crop rule shared with the idle preview crop (see VideoCrop.h); behavior
    // is unchanged, only the owner moved.
    const VideoSourceRect sourceRect = videoSourceRect(rawWidth, rawHeight, outputWidth, outputHeight);
    if (sourceRect.width == 0 || sourceRect.height == 0)
        throw std::invalid_argument("video output exceeds RAW sensor area");
    reduceCfa_ = sourceRect.reduceCfa;
    cropX_ = sourceRect.x;
    cropY_ = sourceRect.y;
    __android_log_print(ANDROID_LOG_INFO, "RawrNativeVideo",
                        "VIDEO_RAW_STAGE input=%ux%u crop=%u,%u %ux%u output=%ux%u method=%s", rawWidth_, rawHeight_,
                        cropX_, cropY_, sourceRect.width, sourceRect.height, outputWidth_, outputHeight_,
                        method());

    try {
        const VkDevice device = context_.device;
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = bindings.size();
        setInfo.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &descriptorLayout_),
              "video demosaic descriptor layout");

        VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_), "video demosaic pipeline layout");

        VkShaderModuleCreateInfo shaderInfo{};
        shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderInfo.codeSize = video_demosaic_spv_size;
        shaderInfo.pCode = reinterpret_cast<const uint32_t*>(video_demosaic_spv);
        VkShaderModule shader = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader), "video demosaic shader");
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout_;
        const VkResult pipelineResult =
            vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
        vkDestroyShaderModule(device, shader, nullptr);
        check(pipelineResult, "video demosaic pipeline");

        constexpr uint32_t slots = kFramesInFlight;
        std::array<VkDescriptorPoolSize, 2> sizes{
            {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, slots * 3u}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slots * 2u}}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = slots;
        poolInfo.poolSizeCount = sizes.size();
        poolInfo.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_), "video demosaic descriptor pool");
        std::array<VkDescriptorSetLayout, slots> layouts{};
        layouts.fill(descriptorLayout_);
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = slots;
        allocate.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(device, &allocate, descriptors_.data()), "video demosaic descriptor sets");

        dummyRaw_ = rawr::vk::createOwnedImage(context_.physicalDevice, device, 1, 1, VK_FORMAT_R16_UINT,
                                                      VK_IMAGE_USAGE_STORAGE_BIT);
        for (uint32_t i = 0; i < slots; ++i) {
            outputs_[i] =
                rawr::vk::createOwnedImage(context_.physicalDevice, device, outputWidth_, outputHeight_,
                                                  VK_FORMAT_R16G16B16A16_SFLOAT,
                                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            clipStates_[i] =
                rawr::vk::createOwnedImage(context_.physicalDevice, device, outputWidth_ / 2u,
                                                  outputHeight_ / 2u, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
            lscBuffers_[i] = makeHostBuffer(kMaxLscBytes);
        }
        if (reduceCfa_) createDownscale();
    } catch (...) {
        destroy();
        throw;
    }
}

VideoDemosaic::~VideoDemosaic() { destroy(); }

void VideoDemosaic::selectDownscaleFilter() {
    char value[PROP_VALUE_MAX]{};
    const bool box = __system_property_get("debug.rawr.video_downscale", value) > 0 && std::strcmp(value, "box") == 0;
    setAntiAlias(!box);
    if (reduceCfa_)
        __android_log_print(ANDROID_LOG_INFO, "RawrNativeVideo", "VIDEO_DOWNSCALE_FILTER method=%s", method());
}

void VideoDemosaic::createDownscale() {
    const VkDevice device = context_.device;
    fullWidth_ = outputWidth_ * 2u;
    fullHeight_ = outputHeight_ * 2u;
    fullImage_ = rawr::vk::createOwnedImage(context_.physicalDevice, device, fullWidth_, fullHeight_,
                                            VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
    fullClip_ = rawr::vk::createOwnedImage(context_.physicalDevice, device, fullWidth_ / 2u, fullHeight_ / 2u,
                                           VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = bindings.size();
    setInfo.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &downscaleLayout_), "video downscale set layout");
    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &downscaleLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &downscalePipelineLayout_),
          "video downscale pipeline layout");
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = video_downscale_spv_size;
    shaderInfo.pCode = reinterpret_cast<const uint32_t*>(video_downscale_spv);
    VkShaderModule shader = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &shaderInfo, nullptr, &shader), "video downscale shader");
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = downscalePipelineLayout_;
    const VkResult pipelineResult =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &downscalePipeline_);
    vkDestroyShaderModule(device, shader, nullptr);
    check(pipelineResult, "video downscale pipeline");

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight * 4u};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &downscalePool_), "video downscale pool");
    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
    layouts.fill(downscaleLayout_);
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = downscalePool_;
    allocate.descriptorSetCount = kFramesInFlight;
    allocate.pSetLayouts = layouts.data();
    check(vkAllocateDescriptorSets(device, &allocate, downscaleSets_.data()), "video downscale sets");
    // The images never change, so the sets are written once.
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
        const std::array<VkImageView, 4> views{fullImage_.view, fullClip_.view, outputs_[slot].view,
                                               clipStates_[slot].view};
        std::array<VkDescriptorImageInfo, 4> infos{};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            infos[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = downscaleSets_[slot];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
    }
}

void VideoDemosaic::recordDownscale(VkCommandBuffer command, uint32_t frameSlot) {
    std::array<VkImageMemoryBarrier, 2> written{};
    for (uint32_t i = 0; i < written.size(); ++i) {
        written[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        written[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        written[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        written[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        written[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        written[i].image = i == 0 ? fullImage_.image : fullClip_.image;
        written[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, written.size(), written.data());
    const uint32_t push[4] = {outputWidth_, outputHeight_, fullWidth_, fullHeight_};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, downscalePipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, downscalePipelineLayout_, 0, 1,
                            &downscaleSets_[frameSlot], 0, nullptr);
    vkCmdPushConstants(command, downscalePipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    // 16x16 workgroups; see TILE in video_downscale.comp.
    vkCmdDispatch(command, (outputWidth_ + 15u) / 16u, (outputHeight_ + 15u) / 16u, 1);
}

VideoDemosaic::HostBuffer VideoDemosaic::makeHostBuffer(VkDeviceSize bytes) {
    HostBuffer out{};
    const VkDevice device = context_.device;
    try {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &out.buffer), "video LSC buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex =
            memoryType(context_.physicalDevice, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &allocation, nullptr, &out.memory), "video LSC memory");
        check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "video LSC bind");
        check(vkMapMemory(device, out.memory, 0, bytes, 0, &out.mapped), "video LSC map");
        return out;
    } catch (...) {
        destroyHostBuffer(out);
        throw;
    }
}

void VideoDemosaic::destroyHostBuffer(HostBuffer& buffer) noexcept {
    const VkDevice device = context_.device;
    if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

void VideoDemosaic::destroy() noexcept {
    const VkDevice device = context_.device;
    if (!device) return;
    for (auto& buffer : lscBuffers_) destroyHostBuffer(buffer);
    for (auto& output : outputs_) rawr::vk::destroyOwnedImage(device, output);
    for (auto& clip : clipStates_) rawr::vk::destroyOwnedImage(device, clip);
    rawr::vk::destroyOwnedImage(device, dummyRaw_);
    rawr::vk::destroyOwnedImage(device, fullImage_);
    rawr::vk::destroyOwnedImage(device, fullClip_);
    if (downscalePool_) vkDestroyDescriptorPool(device, downscalePool_, nullptr);
    if (downscalePipeline_) vkDestroyPipeline(device, downscalePipeline_, nullptr);
    if (downscalePipelineLayout_) vkDestroyPipelineLayout(device, downscalePipelineLayout_, nullptr);
    if (downscaleLayout_) vkDestroyDescriptorSetLayout(device, downscaleLayout_, nullptr);
    downscalePool_ = VK_NULL_HANDLE;
    downscalePipeline_ = VK_NULL_HANDLE;
    downscalePipelineLayout_ = VK_NULL_HANDLE;
    downscaleLayout_ = VK_NULL_HANDLE;
    if (descriptorPool_) vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorLayout_) vkDestroyDescriptorSetLayout(device, descriptorLayout_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    descriptorLayout_ = VK_NULL_HANDLE;
}

void VideoDemosaic::record(VkCommandBuffer command, uint32_t frameSlot, const Frame& frame) {
    if (frameSlot >= outputs_.size() || (!frame.rawBuffer && !frame.rawView) || frame.cfa > 3u || frame.white <= 0.0f)
        throw std::invalid_argument("video demosaic frame invalid");
    const VkDevice device = context_.device;
    const bool useBuffer = frame.rawBuffer != VK_NULL_HANDLE && frame.rawStridePixels >= rawWidth_;
    if (!useBuffer && !frame.rawView) throw std::invalid_argument("video demosaic RAW image missing");
    const bool useLsc = frame.lensShading && frame.lensShadingWidth >= 2 && frame.lensShadingHeight >= 2 &&
                        frame.lensShadingCount == size_t(frame.lensShadingWidth) * frame.lensShadingHeight * 4u &&
                        frame.lensShadingCount * sizeof(float) <= kMaxLscBytes;
    if (useLsc) std::memcpy(lscBuffers_[frameSlot].mapped, frame.lensShading, frame.lensShadingCount * sizeof(float));

    VkDescriptorImageInfo rawImage{};
    rawImage.imageView = useBuffer ? dummyRaw_.view : frame.rawView;
    rawImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo rawBuffer{useBuffer ? frame.rawBuffer : lscBuffers_[frameSlot].buffer, 0, VK_WHOLE_SIZE};
    // With anti-aliased 2x reduction the demosaic fills the shared
    // full-resolution images; recordDownscale then writes this slot's outputs.
    const bool antiAlias = reduceCfa_ && antiAlias_;
    VkDescriptorImageInfo output{};
    output.imageView = antiAlias ? fullImage_.view : outputs_[frameSlot].view;
    output.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo lsc{lscBuffers_[frameSlot].buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo clip{VK_NULL_HANDLE, antiAlias ? fullClip_.view : clipStates_[frameSlot].view,
                               VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptors_[frameSlot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &rawImage;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &rawBuffer;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &output;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &lsc;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &clip;
    vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);

    std::array<VkImageMemoryBarrier, 5> starts{};
    uint32_t count = 0;
    if (!dummyInitialized_) {
        auto& barrier = starts[count++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.image = dummyRaw_.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dummyInitialized_ = true;
    }
    auto& outputStart = starts[count++];
    outputStart.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputStart.oldLayout = initialized_[frameSlot] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    outputStart.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputStart.srcAccessMask = initialized_[frameSlot] ? VK_ACCESS_SHADER_READ_BIT : 0u;
    outputStart.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputStart.image = outputs_[frameSlot].image;
    outputStart.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto& clipStart = starts[count++];
    clipStart.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    clipStart.oldLayout = initialized_[frameSlot] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    clipStart.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    clipStart.srcAccessMask = initialized_[frameSlot] ? VK_ACCESS_SHADER_READ_BIT : 0u;
    clipStart.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    clipStart.image = clipStates_[frameSlot].image;
    clipStart.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (antiAlias) {
        // The previous frame's downscale may still read the shared images.
        for (VkImage image : {fullImage_.image, fullClip_.image}) {
            auto& fullStart = starts[count++];
            fullStart.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            fullStart.oldLayout = fullInitialized_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            fullStart.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            fullStart.srcAccessMask = fullInitialized_ ? VK_ACCESS_SHADER_READ_BIT : 0u;
            fullStart.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            fullStart.image = image;
            fullStart.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        fullInitialized_ = true;
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, count, starts.data());
    initialized_[frameSlot] = true;

    if (useBuffer) {
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.buffer = frame.rawBuffer;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &barrier, 0, nullptr);
    }

    Push push{};
    for (size_t i = 0; i < 4; ++i) {
        push.black[i] = frame.black[i];
        push.invRange[i] = 1.0f / std::max(frame.white - frame.black[i], 1.0f);
        push.wb[i] = frame.wb[i];
    }
    push.width = rawWidth_;
    push.height = rawHeight_;
    push.outWidth = antiAlias ? fullWidth_ : outputWidth_;
    push.outHeight = antiAlias ? fullHeight_ : outputHeight_;
    push.cropX = cropX_;
    push.cropY = cropY_;
    push.pattern = frame.cfa;
    push.stridePixels = frame.rawStridePixels;
    push.bufferEnabled = useBuffer ? 1u : 0u;
    push.reduceCfa = reduceCfa_ && !antiAlias ? 1u : 0u;  // fused box average only for the debug A/B
    push.lscEnabled = useLsc ? 1u : 0u;
    push.lscWidth = frame.lensShadingWidth;
    push.lscHeight = frame.lensShadingHeight;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptors_[frameSlot], 0,
                            nullptr);
    vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    // 16x16 workgroups; see VIDEO_TILE in video_demosaic.comp.
    vkCmdDispatch(command, (push.outWidth + 15u) / 16u, (push.outHeight + 15u) / 16u, 1);
    if (antiAlias) recordDownscale(command, frameSlot);

    std::array<VkImageMemoryBarrier, 2> ready{};
    for (uint32_t i = 0; i < ready.size(); ++i) {
        ready[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ready[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        ready[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ready[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ready[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ready[i].image = i == 0 ? outputs_[frameSlot].image : clipStates_[frameSlot].image;
        ready[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, ready.size(), ready.data());
}
}  // namespace rawrcam::video
