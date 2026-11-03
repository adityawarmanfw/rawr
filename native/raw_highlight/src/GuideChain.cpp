#include "raw_highlight/GuideChain.hpp"
#include "raw_highlight/SpirvLocalSize.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace rawr::highlight {
namespace {

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("highlight guide: ") + what + " VkResult=" + std::to_string(result));
}

VkDescriptorSetLayout makeImageSetLayout(VkDevice device, uint32_t bindingCount) {
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < bindingCount; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = bindingCount;
    info.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout), "set layout");
    return layout;
}

VkDescriptorSetLayout makeControlSetLayout(VkDevice device) {
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType = (i == 0 || i == 3) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 4;
    info.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout), "control set layout");
    return layout;
}

VkPipelineLayout makePipelineLayout(VkDevice device, VkDescriptorSetLayout setLayout, uint32_t pushBytes) {
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &info, nullptr, &layout), "pipeline layout");
    return layout;
}

VkPipeline makePipeline(VkDevice device, SpirvWords spirv, VkPipelineLayout layout, const char* what,
                        bool denseGuide = false) {
    if (!spirv.words || !spirv.count) throw std::invalid_argument(std::string("highlight guide: missing ") + what);
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spirv.count * sizeof(uint32_t);
    moduleInfo.pCode = spirv.words;
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), what);
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    const uint32_t dense = 1;
    const VkSpecializationMapEntry entry{0, 0, sizeof(dense)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(dense), &dense};
    if (denseGuide) info.stage.pSpecializationInfo = &specialization;
    info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    check(result, what);
    return pipeline;
}

VkImageMemoryBarrier guideBarrier(VkImage image, VkAccessFlags src, VkAccessFlags dst, VkImageLayout oldLayout,
                                  VkImageLayout newLayout) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

struct SeedPush {
    uint32_t width, height, guideWidth, guideHeight;
    float ceilingR, ceilingG, ceilingB, pad;
};
static_assert(sizeof(SeedPush) == 32);
struct PassPush {
    uint32_t width, height, step, pad;
};
static_assert(sizeof(PassPush) == 16);
struct ControlPush {
    uint32_t width, height, seedX, seedY, propagateX, propagateY, smoothX, smoothY, applyX, applyY, phase;
};
static_assert(sizeof(ControlPush) == 44);

}  // namespace

GuideChainPipelines::GuideChainPipelines(VkDevice device, const GuideChainShaders& shaders) : device_(device) {
    try {
        seedSize_ = literalLocalSize2D(shaders.seed);
        propagateSize_ = literalLocalSize2D(shaders.propagate);
        smoothSize_ = literalLocalSize2D(shaders.smooth);
        seedSetLayout_ = makeImageSetLayout(device_, 3);
        seedLayout_ = makePipelineLayout(device_, seedSetLayout_, sizeof(SeedPush));
        seed_ = makePipeline(device_, shaders.seed, seedLayout_, "seed pipeline");
        passSetLayout_ = makeImageSetLayout(device_, 2);
        passLayout_ = makePipelineLayout(device_, passSetLayout_, sizeof(PassPush));
        propagate_ = makePipeline(device_, shaders.propagate, passLayout_, "propagate pipeline");
        smooth_ = makePipeline(device_, shaders.smooth, passLayout_, "smooth pipeline");
        if (shaders.control.words && shaders.control.count) {
            smoothDense_ = makePipeline(device_, shaders.smooth, passLayout_, "dense smooth pipeline", true);
            controlSetLayout_ = makeControlSetLayout(device_);
            controlLayout_ = makePipelineLayout(device_, controlSetLayout_, sizeof(ControlPush));
            control_ = makePipeline(device_, shaders.control, controlLayout_, "guide control pipeline");
        }
    } catch (...) {
        destroy();
        throw;
    }
}

GuideChainPipelines::~GuideChainPipelines() { destroy(); }

void GuideChainPipelines::destroy() noexcept {
    if (smoothDense_) vkDestroyPipeline(device_, smoothDense_, nullptr);
    smoothDense_ = VK_NULL_HANDLE;
    if (control_) vkDestroyPipeline(device_, control_, nullptr);
    if (controlLayout_) vkDestroyPipelineLayout(device_, controlLayout_, nullptr);
    if (controlSetLayout_) vkDestroyDescriptorSetLayout(device_, controlSetLayout_, nullptr);
    control_ = VK_NULL_HANDLE;
    controlLayout_ = VK_NULL_HANDLE;
    controlSetLayout_ = VK_NULL_HANDLE;
    if (smooth_) vkDestroyPipeline(device_, smooth_, nullptr);
    if (propagate_) vkDestroyPipeline(device_, propagate_, nullptr);
    if (passLayout_) vkDestroyPipelineLayout(device_, passLayout_, nullptr);
    if (passSetLayout_) vkDestroyDescriptorSetLayout(device_, passSetLayout_, nullptr);
    if (seed_) vkDestroyPipeline(device_, seed_, nullptr);
    if (seedLayout_) vkDestroyPipelineLayout(device_, seedLayout_, nullptr);
    if (seedSetLayout_) vkDestroyDescriptorSetLayout(device_, seedSetLayout_, nullptr);
    smooth_ = propagate_ = seed_ = VK_NULL_HANDLE;
    passLayout_ = seedLayout_ = VK_NULL_HANDLE;
    passSetLayout_ = seedSetLayout_ = VK_NULL_HANDLE;
}

GuideChain::GuideChain(VkPhysicalDevice physicalDevice, const GuideChainPipelines& pipelines, uint32_t guideWidth,
                       uint32_t guideHeight)
    : pipelines_(pipelines), device_(pipelines.device_), guideWidth_(guideWidth), guideHeight_(guideHeight) {
    if (!physicalDevice || !device_ || !guideWidth || !guideHeight)
        throw std::invalid_argument("highlight guide: invalid device or geometry");
    try {
        const bool indirect = pipelines_.control_ != VK_NULL_HANDLE;
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT;
        guideA_ = rawr::vk::createOwnedImage(physicalDevice, device_, guideWidth, guideHeight,
                                             VK_FORMAT_R16G16B16A16_SFLOAT, usage);
        guideB_ = rawr::vk::createOwnedImage(physicalDevice, device_, guideWidth, guideHeight,
                                             VK_FORMAT_R16G16B16A16_SFLOAT, usage);
        VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, indirect ? 9u : 7u},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = indirect ? 4 : 3;
        poolInfo.poolSizeCount = indirect ? 2 : 1;
        poolInfo.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_), "descriptor pool");
        VkDescriptorSetLayout layouts[4]{pipelines_.seedSetLayout_, pipelines_.passSetLayout_,
                                         pipelines_.passSetLayout_, pipelines_.controlSetLayout_};
        VkDescriptorSet sets[4]{};
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = pool_;
        allocInfo.descriptorSetCount = indirect ? 4 : 3;
        allocInfo.pSetLayouts = layouts;
        check(vkAllocateDescriptorSets(device_, &allocInfo, sets), "descriptor sets");
        seedSet_ = sets[0];
        aToBSet_ = sets[1];
        bToASet_ = sets[2];
        controlSet_ = sets[3];
        if (indirect) {
            VkBufferCreateInfo buffer{};
            buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer.size = 64;
            buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            check(vkCreateBuffer(device_, &buffer, nullptr, &arguments_), "indirect buffer");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device_, arguments_, &requirements);
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    type = i;
                    break;
                }
            if (type == UINT32_MAX) throw std::runtime_error("highlight guide: no indirect memory type");
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            check(vkAllocateMemory(device_, &allocation, nullptr, &argumentsMemory_), "indirect memory");
            check(vkBindBufferMemory(device_, arguments_, argumentsMemory_, 0), "indirect buffer bind");
        }
        VkDescriptorImageInfo infos[4]{{VK_NULL_HANDLE, guideA_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, guideB_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, guideB_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, guideA_.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = i < 2 ? aToBSet_ : bToASet_;
            writes[i].dstBinding = i & 1u;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
    } catch (...) {
        destroy();
        throw;
    }
}

GuideChain::~GuideChain() { destroy(); }

void GuideChain::destroy() noexcept {
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    seedSet_ = aToBSet_ = bToASet_ = VK_NULL_HANDLE;
    if (arguments_) vkDestroyBuffer(device_, arguments_, nullptr);
    if (argumentsMemory_) vkFreeMemory(device_, argumentsMemory_, nullptr);
    arguments_ = VK_NULL_HANDLE;
    argumentsMemory_ = VK_NULL_HANDLE;
    rawr::vk::destroyOwnedImage(device_, guideB_);
    rawr::vk::destroyOwnedImage(device_, guideA_);
}

void GuideChain::recordInitialize(VkCommandBuffer command) {
    if (initialized_) return;
    VkImageMemoryBarrier init[2]{
        guideBarrier(guideA_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        guideBarrier(guideB_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_GENERAL)};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 2, init);
    initialized_ = true;
}

VkImageView GuideChain::record(VkCommandBuffer command, VkImageView rgbView, VkImageView clipStateView,
                               uint32_t width, uint32_t height, const float channelCeilings[3],
                               const Conditional* conditional, const Timing* timing, VkBuffer clipPredicate) {
    recordInitialize(command);
    const bool predicated = conditional && conditional->begin && conditional->end && conditional->info;
    indirectActive_ = pipelines_.control_ != VK_NULL_HANDLE && clipPredicate != VK_NULL_HANDLE;
    auto argumentBarrier = [&](VkPipelineStageFlags sourceStage, VkAccessFlags sourceAccess,
                               VkPipelineStageFlags targetStage, VkAccessFlags targetAccess) {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = sourceAccess;
        b.dstAccessMask = targetAccess;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = arguments_;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, sourceStage, targetStage, 0, 0, nullptr, 1, &b, 0, nullptr);
    };
    ControlPush control{guideWidth_, guideHeight_,
                        (guideWidth_ + pipelines_.seedSize_[0] - 1u) / pipelines_.seedSize_[0],
                        (guideHeight_ + pipelines_.seedSize_[1] - 1u) / pipelines_.seedSize_[1],
                        (guideWidth_ + pipelines_.propagateSize_[0] - 1u) / pipelines_.propagateSize_[0],
                        (guideHeight_ + pipelines_.propagateSize_[1] - 1u) / pipelines_.propagateSize_[1],
                        (guideWidth_ + pipelines_.smoothSize_[0] - 1u) / pipelines_.smoothSize_[0],
                        (guideHeight_ + pipelines_.smoothSize_[1] - 1u) / pipelines_.smoothSize_[1],
                        (width + 15u) / 16u, (height + 15u) / 16u, 0u};
    auto bindControl = [&]() {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.control_);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.controlLayout_, 0, 1,
                                &controlSet_, 0, nullptr);
        vkCmdPushConstants(command, pipelines_.controlLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(control), &control);
    };
    if (indirectActive_) {
        VkDescriptorImageInfo images[2]{{VK_NULL_HANDLE, guideA_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, guideB_.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkDescriptorBufferInfo buffers[2]{{arguments_, 0, VK_WHOLE_SIZE}, {clipPredicate, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = controlSet_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = (i == 0 || i == 3) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i == 0 || i == 3) writes[i].pImageInfo = &images[i == 0 ? 0 : 1];
            else writes[i].pBufferInfo = &buffers[i - 1];
        }
        vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
        argumentBarrier(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        bindControl();
        vkCmdDispatch(command, 1, 1, 1);
        argumentBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    }
    auto dispatch = [&](const std::array<uint32_t, 2>& local, VkDeviceSize commandOffset) {
        if (indirectActive_) {
            vkCmdDispatchIndirect(command, arguments_, commandOffset);
            return;
        }
        if (predicated) conditional->begin(command, conditional->info);
        vkCmdDispatch(command, (guideWidth_ + local[0] - 1u) / local[0], (guideHeight_ + local[1] - 1u) / local[1], 1);
        if (predicated) conditional->end(command);
    };
    VkImageMemoryBarrier sync[2]{
        guideBarrier(guideA_.image, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL),
        guideBarrier(guideB_.image, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL)};
    auto guideSync = [&]() {
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 2, sync);
    };

    VkDescriptorImageInfo seedInfo[3]{{VK_NULL_HANDLE, rgbView, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, clipStateView, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, guideA_.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet seedWrites[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        seedWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        seedWrites[i].dstSet = seedSet_;
        seedWrites[i].dstBinding = i;
        seedWrites[i].descriptorCount = 1;
        seedWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        seedWrites[i].pImageInfo = &seedInfo[i];
    }
    vkUpdateDescriptorSets(device_, 3, seedWrites, 0, nullptr);
    const SeedPush seedPush{width, height, guideWidth_, guideHeight_,
                            channelCeilings[0], channelCeilings[1], channelCeilings[2], 0.0f};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.seed_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.seedLayout_, 0, 1, &seedSet_, 0,
                            nullptr);
    vkCmdPushConstants(command, pipelines_.seedLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(seedPush), &seedPush);
    dispatch(pipelines_.seedSize_, 0);
    if (timing && timing->pool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timing->pool, timing->seedQuery);
    guideSync();

    if (indirectActive_) {
        // If no seeds exist, every propagation/smoothing pass preserves the
        // seed field. Initialize both ping-pong images so skipping an odd
        // number of passes cannot expose a guide retained from an older frame.
        argumentBarrier(VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        control.phase = 1u;
        bindControl();
        vkCmdDispatchIndirect(command, arguments_, 4 * sizeof(VkDispatchIndirectCommand));
        guideSync();
        argumentBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    }

    // Jump flood from the largest power of two below the guide extent down to 1.
    PassPush passPush{guideWidth_, guideHeight_, 1u, 0u};
    uint32_t step = 1u;
    while ((step << 1u) < std::max(guideWidth_, guideHeight_)) step <<= 1u;
    bool sourceA = true;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.propagate_);
    for (;;) {
        passPush.step = step;
        VkDescriptorSet set = sourceA ? aToBSet_ : bToASet_;
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.passLayout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(command, pipelines_.passLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(passPush),
                           &passPush);
        dispatch(pipelines_.propagateSize_, sizeof(VkDispatchIndirectCommand));
        guideSync();
        sourceA = !sourceA;
        if (step == 1u) break;
        step >>= 1u;
    }
    if (timing && timing->pool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timing->pool, timing->propagationQuery);
    // A broad low-resolution blur removes seed-cell colour contours while
    // leaving full-resolution luminance texture to the surviving channels.
    // A seed can reach every cell by the signed binary steps of its XY
    // distance, staying inside the image. All seed RGB sums are near one;
    // positive weighted averages retain that property. After all steps,
    // an actually executed indirect smoothing pass therefore has no empty
    // RGB cells. Empty-seed frames have groupCountX=0 and never run it.
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                       indirectActive_ ? pipelines_.smoothDense_ : pipelines_.smooth_);
    for (uint32_t pass = 0; pass < 4u; ++pass) {
        passPush.step = 0u;
        VkDescriptorSet set = sourceA ? aToBSet_ : bToASet_;
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_.passLayout_, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(command, pipelines_.passLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(passPush),
                           &passPush);
        dispatch(pipelines_.smoothSize_, 2 * sizeof(VkDispatchIndirectCommand));
        guideSync();
        sourceA = !sourceA;
    }
    return sourceA ? guideA_.view : guideB_.view;
}

}  // namespace rawr::highlight
