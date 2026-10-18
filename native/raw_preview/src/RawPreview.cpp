#include "raw_preview/RawPreview.hpp"

#include "raw_highlight/Coloropp.hpp"
#include "raw_highlight/GuideChain.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace raw_preview {
namespace {
std::vector<uint32_t> readSpirv(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open SPIR-V: " + p);
    auto n = f.tellg();
    if (n <= 0 || (n % 4) != 0) throw std::runtime_error("Invalid SPIR-V: " + p);
    std::vector<uint32_t> v(size_t(n) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), n);
    return v;
}
void checkVkResult(VkResult r, const char* m) {
    if (r != VK_SUCCESS) throw std::runtime_error(m);
}
struct PushConstants {
    float black[4];
    float invRange[4];
    float wb[4];
    float clipThreshold;
    float edgeStrength;
    float chromaBlend;
    float highlightWarningThreshold;
    float shadowWarningThreshold;
    uint32_t cellScale;
    uint32_t width;
    uint32_t height;
    uint32_t pattern;
    uint32_t lscEnabled;
    uint32_t lscWidth;
    uint32_t lscHeight;
    uint32_t highlightReconstructionEnabled;
    uint32_t rawStridePixels;
    uint32_t rawBufferEnabled;
};
}  // namespace

RawPreview::RawPreview(const RawPreviewCreateInfo& ci)
    : physicalDevice_(ci.physicalDevice), device_(ci.device), objects_(ci.device) {
    if (!device_ || !physicalDevice_) throw std::runtime_error("RawPreview: null Vulkan device");
    if (ci.workgroupX != 8 || ci.workgroupY != 8)
        throw std::runtime_error("RawPreview requires the validated 8x8 workgroup size");

    // trailingBufferBindings trailing bindings are STORAGE_BUFFER (LSC map,
    // imported AHB words, ...); the rest are STORAGE_IMAGE.
    auto createLayoutAndPipeline = [&](uint32_t bindingCount, uint32_t trailingBufferBindings,
                                       const std::string& shaderPath,
                                       VkDescriptorSetLayout& descriptorSetLayout, VkPipelineLayout& pipelineLayout,
                                       VkPipeline& pipeline) {
        std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
        for (uint32_t i = 0; i < bindingCount; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = (i + trailingBufferBindings >= bindingCount)
                                             ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslInfo{};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = bindingCount;
        dslInfo.pBindings = bindings.data();
        checkVkResult(vkCreateDescriptorSetLayout(device_, &dslInfo, nullptr, &descriptorSetLayout),
                      "vkCreateDescriptorSetLayout failed");
        objects_.adoptDescriptorSetLayout(descriptorSetLayout);

        VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        checkVkResult(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout),
                      "vkCreatePipelineLayout failed");
        objects_.adoptPipelineLayout(pipelineLayout);

        auto spirv = readSpirv(shaderPath);
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
        moduleInfo.pCode = spirv.data();
        VkShaderModule module{};
        checkVkResult(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module), "vkCreateShaderModule failed");
        rawr::vk::DeviceObjects scratch(device_);
        scratch.adoptShaderModule(module);

        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = module;
        stageInfo.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = pipelineLayout;
        checkVkResult(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
                      "vkCreateComputePipelines failed");
        objects_.adoptPipeline(pipeline);
    };

    createLayoutAndPipeline(4, 2, ci.shaderPath, dsl_, layout_, pipeline_);
    if (!ci.cfaStateShaderPath.empty())
        createLayoutAndPipeline(6, 3, ci.cfaStateShaderPath, cfaStateDsl_, cfaStateLayout_, cfaStatePipeline_);
    if (ci.conditionalRendering) {
        beginConditional_ = reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdBeginConditionalRenderingEXT"));
        endConditional_ = reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdEndConditionalRenderingEXT"));
        if (!beginConditional_ || !endConditional_) beginConditional_ = nullptr, endConditional_ = nullptr;
    }

    auto createTypedPipeline = [&](const std::vector<VkDescriptorType>& types, uint32_t pushBytes,
                                   const std::string& shaderPath, VkDescriptorSetLayout& descriptorSetLayout,
                                   VkPipelineLayout& pipelineLayout, VkPipeline& pipeline) {
        const uint32_t bindingCount = uint32_t(types.size());
        std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
        for (uint32_t i = 0; i < bindingCount; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = types[i];
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds.bindingCount = bindingCount;
        ds.pBindings = bindings.data();
        checkVkResult(vkCreateDescriptorSetLayout(device_, &ds, nullptr, &descriptorSetLayout),
                      "RawPreview highlight set layout failed");
        objects_.adoptDescriptorSetLayout(descriptorSetLayout);
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &descriptorSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        checkVkResult(vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout),
                      "RawPreview highlight pipeline layout failed");
        objects_.adoptPipelineLayout(pipelineLayout);
        auto spirv = readSpirv(shaderPath);
        VkShaderModuleCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize = spirv.size() * sizeof(uint32_t);
        sm.pCode = spirv.data();
        VkShaderModule module{};
        checkVkResult(vkCreateShaderModule(device_, &sm, nullptr, &module),
                      "RawPreview highlight shader module failed");
        rawr::vk::DeviceObjects scratch(device_);
        scratch.adoptShaderModule(module);
        VkPipelineShaderStageCreateInfo st{};
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = module;
        st.pName = "main";
        VkComputePipelineCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage = st;
        pi.layout = pipelineLayout;
        checkVkResult(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline),
                      "RawPreview highlight compute pipeline failed");
        objects_.adoptPipeline(pipeline);
    };
    auto createImagePipeline = [&](uint32_t bindingCount, uint32_t pushBytes, const std::string& shaderPath,
                                   VkDescriptorSetLayout& descriptorSetLayout, VkPipelineLayout& pipelineLayout,
                                   VkPipeline& pipeline) {
        createTypedPipeline(std::vector<VkDescriptorType>(bindingCount, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), pushBytes,
                            shaderPath, descriptorSetLayout, pipelineLayout, pipeline);
    };
    std::string seedPath = ci.highlightGuideSeedShaderPath, propagatePath = ci.highlightGuidePropagateShaderPath,
                smoothPath = ci.highlightGuideSmoothShaderPath, applyPath = ci.highlightApplyShaderPath;
    std::string coloroppPath = ci.coloroppShaderPath, coloroppTonePath = ci.coloroppToneShaderPath;
    const size_t cfaSlash = ci.cfaStateShaderPath.find_last_of("/\\");
    const std::string shaderDir =
        cfaSlash == std::string::npos ? std::string() : ci.cfaStateShaderPath.substr(0, cfaSlash + 1);
    if (coloroppPath.empty() && !ci.cfaStateShaderPath.empty()) {
        coloroppPath = shaderDir + "raw_preview_coloropp.comp.spv";
        coloroppTonePath = shaderDir + "raw_preview_coloropp_tone.comp.spv";
    }
    if (seedPath.empty() && !ci.cfaStateShaderPath.empty()) {
        const std::string& dir = shaderDir;
        seedPath = dir + "raw_preview_highlight_guide_seed.comp.spv";
        propagatePath = dir + "raw_preview_highlight_guide_propagate.comp.spv";
        smoothPath = dir + "raw_preview_highlight_guide_smooth.comp.spv";
        applyPath = dir + "raw_preview_highlight_apply.comp.spv";
    }
    auto readable = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return bool(f);
    };
    const bool allHighlightShaders =
        readable(seedPath) && readable(propagatePath) && readable(smoothPath) && readable(applyPath);
    // The CFA-state path reconstructs highlights only through the raw_highlight
    // passes, the same ones stills use, so their shaders are required with it.
    if (!ci.cfaStateShaderPath.empty() && !allHighlightShaders)
        throw std::runtime_error("RawPreview: highlight guide shaders missing next to " + ci.cfaStateShaderPath);
    if (allHighlightShaders) {
        const auto seed = readSpirv(seedPath), propagate = readSpirv(propagatePath), smooth = readSpirv(smoothPath);
        guidePipelines_ = std::make_unique<rawr::highlight::GuideChainPipelines>(
            device_, rawr::highlight::GuideChainShaders{{seed.data(), seed.size()},
                                                        {propagate.data(), propagate.size()},
                                                        {smooth.data(), smooth.size()}});
        createImagePipeline(3, 32, applyPath, highlightApplyDsl_, highlightApplyLayout_, highlightApplyPipeline_);
    }
    if (!ci.cfaStateShaderPath.empty() && readable(coloroppPath) && readable(coloroppTonePath)) {
        createTypedPipeline({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                            sizeof(rawr::highlight::ColoroppPush), coloroppPath, coloroppDsl_, coloroppLayout_,
                            coloroppPipeline_);
        createImagePipeline(2, sizeof(rawr::highlight::ColoroppTonePush), coloroppTonePath, coloroppToneDsl_,
                            coloroppToneLayout_, coloroppTonePipeline_);
    }

    VkDescriptorPoolSize poolSizes[2]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 128;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    checkVkResult(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_), "vkCreateDescriptorPool failed");
    objects_.adoptDescriptorPool(pool_);
}

// Members release in reverse order: per-view caches and guide chains first,
// then the guide pipelines, then objects_ (pipelines, layouts, pool).
RawPreview::~RawPreview() = default;

RawPreview::HighlightResources& RawPreview::highlightResourcesFor(VkImageView outputView, uint32_t width,
                                                                  uint32_t height) {
    auto found = highlightResources_.find(outputView);
    if (found != highlightResources_.end()) return found->second;
    HighlightResources r{};
    r.chain = std::make_unique<rawr::highlight::GuideChain>(physicalDevice_, *guidePipelines_, (width + 3u) / 4u,
                                                            (height + 3u) / 4u);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &highlightApplyDsl_;
    checkVkResult(vkAllocateDescriptorSets(device_, &ai, &r.applySet), "RawPreview highlight apply set failed");
    return highlightResources_.emplace(outputView, std::move(r)).first->second;
}

RawPreview::ColoroppResources& RawPreview::coloroppResourcesFor(VkImageView outputView, uint32_t width,
                                                                uint32_t height) {
    auto found = coloroppResources_.find(outputView);
    if (found != coloroppResources_.end()) return found->second;
    ColoroppResources r{rawr::vk::DeviceObjects(device_)};
    r.demosaiced = r.objects.adoptOwnedImage(rawr::vk::createOwnedImage(
        physicalDevice_, device_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT));
    // Same usage as the caller's linear image: the display path samples,
    // blits (film) and stores to it.
    r.sdr = r.objects.adoptOwnedImage(rawr::vk::createOwnedImage(
        physicalDevice_, device_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    VkDescriptorSetLayout layouts[2]{coloroppDsl_, coloroppToneDsl_};
    VkDescriptorSet sets[2]{};
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    checkVkResult(vkAllocateDescriptorSets(device_, &ai, sets), "RawPreview Inpaint Opposed sets failed");
    r.coloroppSet = sets[0];
    r.toneSet = sets[1];
    return coloroppResources_.emplace(outputView, std::move(r)).first->second;
}

VkDescriptorSet RawPreview::descriptorSetFor(VkImageView inputView, VkImageView outputView, VkBuffer lscBuffer,
                                              VkBuffer importBuffer) {
    ViewPair key{inputView, outputView, importBuffer};
    auto it = descriptorCache_.find(key);
    if (it != descriptorCache_.end()) return it->second;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &dsl_;
    VkDescriptorSet set{};
    checkVkResult(vkAllocateDescriptorSets(device_, &allocInfo, &set), "vkAllocateDescriptorSets failed");

    VkDescriptorImageInfo images[2]{};
    images[0].imageView = inputView;
    images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[1].imageView = outputView;
    images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    // Binding 3 carries the imported AHB words when buffer-direct preview is
    // active; otherwise it aliases the (valid, unread) LSC buffer so the
    // layout stays fully bound.
    VkDescriptorBufferInfo lsc{lscBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo import{importBuffer != VK_NULL_HANDLE ? importBuffer : lscBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet all[4]{writes[0], writes[1], {}, {}};
    all[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    all[2].dstSet = set;
    all[2].dstBinding = 2;
    all[2].descriptorCount = 1;
    all[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    all[2].pBufferInfo = &lsc;
    all[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    all[3].dstSet = set;
    all[3].dstBinding = 3;
    all[3].descriptorCount = 1;
    all[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    all[3].pBufferInfo = &import;
    vkUpdateDescriptorSets(device_, 4, all, 0, nullptr);
    descriptorCache_.emplace(key, set);
    return set;
}

VkDescriptorSet RawPreview::cfaStateDescriptorSetFor(VkImageView inputView, VkImageView outputView,
                                                     VkImageView stateView, VkBuffer lscBuffer,
                                                     VkBuffer importBuffer, VkBuffer clipFlagBuffer) {
    ViewTriple key{inputView, outputView, stateView, importBuffer};
    auto it = cfaStateDescriptorCache_.find(key);
    if (it != cfaStateDescriptorCache_.end()) return it->second;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = pool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &cfaStateDsl_;
    VkDescriptorSet set{};
    checkVkResult(vkAllocateDescriptorSets(device_, &allocInfo, &set), "vkAllocateDescriptorSets failed");

    VkDescriptorImageInfo images[3]{};
    images[0].imageView = inputView;
    images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[1].imageView = outputView;
    images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[2].imageView = stateView;
    images[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    VkDescriptorBufferInfo lsc{lscBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo import{importBuffer != VK_NULL_HANDLE ? importBuffer : lscBuffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo clipFlag{clipFlagBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet all[6]{writes[0], writes[1], writes[2], {}, {}, {}};
    all[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    all[3].dstSet = set;
    all[3].dstBinding = 3;
    all[3].descriptorCount = 1;
    all[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    all[3].pBufferInfo = &lsc;
    all[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    all[4].dstSet = set;
    all[4].dstBinding = 4;
    all[4].descriptorCount = 1;
    all[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    all[4].pBufferInfo = &import;
    all[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    all[5].dstSet = set;
    all[5].dstBinding = 5;
    all[5].descriptorCount = 1;
    all[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    all[5].pBufferInfo = &clipFlag;
    vkUpdateDescriptorSets(device_, 6, all, 0, nullptr);
    cfaStateDescriptorCache_.emplace(key, set);
    return set;
}

RawPreview::ClipFlag& RawPreview::clipFlagFor(VkImageView outputView) {
    auto found = clipFlags_.find(outputView);
    if (found != clipFlags_.end()) return found->second;
    // Cached only once complete, so a failed allocation is retried, never reused unbound.
    ClipFlag f{rawr::vk::DeviceObjects(device_)};
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 16;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               (beginConditional_ ? VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT : 0u);
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checkVkResult(vkCreateBuffer(device_, &bi, nullptr, &f.buffer), "RawPreview clip flag vkCreateBuffer failed");
    f.objects.adoptBuffer(f.buffer);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, f.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if (req.memoryTypeBits & (1u << i)) type = i;
    if (type == UINT32_MAX) throw std::runtime_error("RawPreview clip flag no memory type");
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory{};
    checkVkResult(vkAllocateMemory(device_, &ai, nullptr, &memory), "RawPreview clip flag vkAllocateMemory failed");
    f.objects.adoptMemory(memory);
    checkVkResult(vkBindBufferMemory(device_, f.buffer, memory, 0), "RawPreview clip flag bind failed");
    return clipFlags_.emplace(outputView, std::move(f)).first->second;
}

RawPreview::LscBuffer& RawPreview::lscBufferFor(VkImageView outputView, size_t floatCount) {
    const VkDeviceSize needed = std::max<VkDeviceSize>(sizeof(float) * 4u, VkDeviceSize(floatCount * sizeof(float)));
    auto found = lscBuffers_.find(outputView);
    if (found != lscBuffers_.end() && found->second.bytes >= needed) return found->second;
    LscBuffer b{rawr::vk::DeviceObjects(device_)};
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = needed;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checkVkResult(vkCreateBuffer(device_, &bi, nullptr, &b.buffer), "RawPreview LSC vkCreateBuffer failed");
    b.objects.adoptBuffer(b.buffer);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) throw std::runtime_error("RawPreview LSC no coherent host-visible memory");
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory{};
    checkVkResult(vkAllocateMemory(device_, &ai, nullptr, &memory), "RawPreview LSC vkAllocateMemory failed");
    b.objects.adoptMemory(memory);
    checkVkResult(vkBindBufferMemory(device_, b.buffer, memory, 0), "RawPreview LSC vkBindBufferMemory failed");
    checkVkResult(vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "RawPreview LSC vkMapMemory failed");
    b.bytes = needed;
    // Reallocation changes descriptor buffer handles; invalidate cached sets using this output view.
    for (auto it = descriptorCache_.begin(); it != descriptorCache_.end();)
        it = (it->first.output == outputView) ? descriptorCache_.erase(it) : std::next(it);
    for (auto it = cfaStateDescriptorCache_.begin(); it != cfaStateDescriptorCache_.end();)
        it = (it->first.output == outputView) ? cfaStateDescriptorCache_.erase(it) : std::next(it);
    // Replacing an entry releases the smaller buffer it held.
    return lscBuffers_.insert_or_assign(outputView, std::move(b)).first->second;
}

RawPreviewRecordResult RawPreview::record(const RawPreviewRecordInfo& recordInfo) {
    if (!recordInfo.commandBuffer || !recordInfo.inputRawR16UintView || !recordInfo.outputLinearRgba16fView ||
        recordInfo.width < 2 || recordInfo.height < 2)
        throw std::runtime_error("RawPreview::record invalid arguments");

    const bool writeCfaState = recordInfo.outputCfaStateR16UintView != VK_NULL_HANDLE;
    if (writeCfaState && !cfaStatePipeline_)
        throw std::runtime_error("RawPreview::record requires cfaStateShaderPath when CFA-state output is enabled");
    // Inpaint Opposed mirrors still/video: repair post-WB RGB into the linear
    // output, then compress highlights into a separate SDR image for display.
    const bool useColoropp = recordInfo.parameters.highlightReconstructionEnabled &&
                             recordInfo.parameters.highlightMethod == 1u && writeCfaState && coloroppPipeline_ &&
                             coloroppTonePipeline_;
    const bool usePropagatedHighlight =
        recordInfo.parameters.highlightReconstructionEnabled && writeCfaState && !useColoropp;
    const uint32_t cellScale = std::max(1u, recordInfo.cellScale);
    const uint32_t outputWidth = (recordInfo.width >> 1u) / cellScale, outputHeight = (recordInfo.height >> 1u) / cellScale;
    ColoroppResources* coloropp =
        useColoropp ? &coloroppResourcesFor(recordInfo.outputLinearRgba16fView, outputWidth, outputHeight) : nullptr;
    if (coloropp && !coloropp->initialized) {
        VkImageMemoryBarrier init[2]{};
        VkImage images[2]{coloropp->demosaiced.image, coloropp->sdr.image};
        for (uint32_t i = 0; i < 2; ++i) {
            init[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            init[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            init[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            init[i].srcQueueFamilyIndex = init[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            init[i].image = images[i];
            init[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            init[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(recordInfo.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, init);
        coloropp->initialized = true;
    }
    // The CFA-state pass writes the Inpaint Opposed source image instead of the
    // caller's linear output; Coloropp then writes the linear output.
    const VkImageView formedView = coloropp ? coloropp->demosaiced.view : recordInfo.outputLinearRgba16fView;

    const bool lscValid = recordInfo.lensShadingMap && recordInfo.lensShadingMapWidth >= 2u &&
                          recordInfo.lensShadingMapHeight >= 2u &&
                          recordInfo.lensShadingMapFloatCount ==
                              size_t(recordInfo.lensShadingMapWidth) * recordInfo.lensShadingMapHeight * 4u;
    auto& lsc = lscBufferFor(formedView, lscValid ? recordInfo.lensShadingMapFloatCount : 4u);
    if (lscValid)
        std::memcpy(lsc.mapped, recordInfo.lensShadingMap, recordInfo.lensShadingMapFloatCount * sizeof(float));
    else {
        const float identity[4] = {1, 1, 1, 1};
        std::memcpy(lsc.mapped, identity, sizeof(identity));
    }
    const bool useImportBuffer =
        recordInfo.inputRawImportBuffer != VK_NULL_HANDLE && recordInfo.inputRawStridePixels != 0u;
    // Predicate for the colour-propagation chain: cleared here, set by the
    // CFA-state pass on any sensor-clipped sample. The apply pass only touches
    // clipped pixels, so skipping both reproduces the unclipped frame exactly.
    const bool predicatedHighlight = usePropagatedHighlight && beginConditional_ != nullptr;
    ClipFlag* clipFlag = writeCfaState ? &clipFlagFor(formedView) : nullptr;
    if (clipFlag) {
        // WAR on the previous frame's predicate read, then RAW for the shader.
        vkCmdPipelineBarrier(recordInfo.commandBuffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 (beginConditional_ ? VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT : 0u),
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
        vkCmdFillBuffer(recordInfo.commandBuffer, clipFlag->buffer, 0, VK_WHOLE_SIZE, 0u);
        VkBufferMemoryBarrier cleared{};
        cleared.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cleared.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        cleared.srcQueueFamilyIndex = cleared.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cleared.buffer = clipFlag->buffer;
        cleared.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(recordInfo.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &cleared, 0, nullptr);
    }
    VkDescriptorSet descriptorSet =
        writeCfaState
            ? cfaStateDescriptorSetFor(recordInfo.inputRawR16UintView, formedView,
                                       recordInfo.outputCfaStateR16UintView, lsc.buffer,
                                       useImportBuffer ? recordInfo.inputRawImportBuffer : VK_NULL_HANDLE,
                                       clipFlag->buffer)
            : descriptorSetFor(recordInfo.inputRawR16UintView, recordInfo.outputLinearRgba16fView, lsc.buffer,
                               useImportBuffer ? recordInfo.inputRawImportBuffer : VK_NULL_HANDLE);

    PushConstants push{};
    for (int channel = 0; channel < 4; ++channel) {
        push.black[channel] = recordInfo.parameters.blackLevel[channel];
        const float range = recordInfo.parameters.whiteLevel - recordInfo.parameters.blackLevel[channel];
        push.invRange[channel] = 1.0f / (range > 1.0f ? range : 1.0f);
        push.wb[channel] = recordInfo.parameters.whiteBalance[channel];
    }
    // Classification is sensor provenance and remains active even when recovery
    // is disabled (RAW warning overlays consume the same state).
    push.clipThreshold = std::max(0.90f, std::min(1.0f, recordInfo.parameters.clipThreshold));
    push.edgeStrength = std::max(0.0f, recordInfo.parameters.edgeStrength);
    push.chromaBlend = std::max(0.0f, std::min(1.0f, recordInfo.parameters.chromaBlend));
    push.highlightWarningThreshold = std::max(0.0f, std::min(1.0f, recordInfo.parameters.highlightWarningThreshold));
    push.shadowWarningThreshold = std::max(0.0f, std::min(1.0f, recordInfo.parameters.shadowWarningThreshold));
    push.cellScale = cellScale;
    push.width = recordInfo.width;
    push.height = recordInfo.height;
    push.pattern = uint32_t(recordInfo.parameters.pattern);
    push.lscEnabled = lscValid ? 1u : 0u;
    push.lscWidth = lscValid ? recordInfo.lensShadingMapWidth : 0u;
    push.lscHeight = lscValid ? recordInfo.lensShadingMapHeight : 0u;
    push.rawStridePixels = useImportBuffer ? recordInfo.inputRawStridePixels : 0u;
    push.rawBufferEnabled = useImportBuffer ? 1u : 0u;
    // Mode 2 forms ordinary WB RGB and writes sensor provenance, leaving the
    // image-wide reconstruction to the synchronized guide passes below.
    push.highlightReconstructionEnabled =
        (usePropagatedHighlight || useColoropp) ? 2u
                                                : (recordInfo.parameters.highlightReconstructionEnabled ? 1u : 0u);

    VkPipeline pipeline = writeCfaState ? cfaStatePipeline_ : pipeline_;
    VkPipelineLayout pipelineLayout = writeCfaState ? cfaStateLayout_ : layout_;
    vkCmdBindPipeline(recordInfo.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(recordInfo.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                            &descriptorSet, 0, nullptr);
    vkCmdPushConstants(recordInfo.commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(recordInfo.commandBuffer, (outputWidth + 7) / 8, (outputHeight + 7) / 8, 1);

    if (coloropp) {
        recordColoropp(recordInfo, *coloropp, lsc.buffer, lscValid);
        if (recordInfo.parameters.bypassHighlightTone)
            return {};
        return {coloropp->sdr.image, coloropp->sdr.view};
    }
    if (!usePropagatedHighlight) return {};

    HighlightResources& hr = highlightResourcesFor(recordInfo.outputLinearRgba16fView, outputWidth, outputHeight);
    VkMemoryBarrier rgbReady{};
    rgbReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    rgbReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    rgbReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(recordInfo.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &rgbReady, 0, nullptr, 0, nullptr);
    const float greenWb = 0.5f * (recordInfo.parameters.whiteBalance[1] + recordInfo.parameters.whiteBalance[2]);
    const float ceilings[3]{recordInfo.parameters.whiteBalance[0], greenWb, recordInfo.parameters.whiteBalance[3]};
    VkConditionalRenderingBeginInfoEXT condBegin{};
    condBegin.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
    condBegin.buffer = clipFlag->buffer;
    const rawr::highlight::GuideChain::Conditional conditional{beginConditional_, endConditional_, &condBegin};
    if (predicatedHighlight) {
        VkBufferMemoryBarrier flagReady{};
        flagReady.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        flagReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        flagReady.dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT;
        flagReady.srcQueueFamilyIndex = flagReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        flagReady.buffer = clipFlag->buffer;
        flagReady.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(recordInfo.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT, 0, 0, nullptr, 1, &flagReady, 0,
                             nullptr);
    }
    VkImageView finalGuide = hr.chain->record(recordInfo.commandBuffer, recordInfo.outputLinearRgba16fView,
                                              recordInfo.outputCfaStateR16UintView, outputWidth, outputHeight,
                                              ceilings, predicatedHighlight ? &conditional : nullptr);
    VkDescriptorImageInfo applyInfo[3]{{VK_NULL_HANDLE, recordInfo.outputLinearRgba16fView, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, recordInfo.outputCfaStateR16UintView, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, finalGuide, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet applyWrites[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        applyWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        applyWrites[i].dstSet = hr.applySet;
        applyWrites[i].dstBinding = i;
        applyWrites[i].descriptorCount = 1;
        applyWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        applyWrites[i].pImageInfo = &applyInfo[i];
    }
    vkUpdateDescriptorSets(device_, 3, applyWrites, 0, nullptr);
    struct ApplyPc {
        uint32_t width, height, enabled, pad;
        float r, g, b, fp;
    } ap{outputWidth,
         outputHeight,
         1u,
         0u,
         recordInfo.parameters.whiteBalance[0],
         greenWb,
         recordInfo.parameters.whiteBalance[3],
         0.0f};
    static_assert(sizeof(ApplyPc) == 32);
    vkCmdBindPipeline(recordInfo.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, highlightApplyPipeline_);
    vkCmdBindDescriptorSets(recordInfo.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, highlightApplyLayout_, 0, 1,
                            &hr.applySet, 0, nullptr);
    vkCmdPushConstants(recordInfo.commandBuffer, highlightApplyLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ap),
                       &ap);
    if (predicatedHighlight) beginConditional_(recordInfo.commandBuffer, &condBegin);
    vkCmdDispatch(recordInfo.commandBuffer, (outputWidth + 15u) / 16u, (outputHeight + 15u) / 16u, 1);
    if (predicatedHighlight) endConditional_(recordInfo.commandBuffer);
    return {};
}

void RawPreview::recordColoropp(const RawPreviewRecordInfo& recordInfo, ColoroppResources& r, VkBuffer lscBuffer,
                                bool lscValid) {
    using namespace rawr::highlight;
    const VkCommandBuffer cmd = recordInfo.commandBuffer;
    const uint32_t cellScale = std::max(1u, recordInfo.cellScale);
    const uint32_t width = (recordInfo.width >> 1u) / cellScale, height = (recordInfo.height >> 1u) / cellScale;
    auto computeToCompute = [&]() {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
    };
    computeToCompute();

    VkDescriptorImageInfo images[2]{{VK_NULL_HANDLE, r.demosaiced.view, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, recordInfo.outputLinearRgba16fView, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo shading{lscBuffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = r.coloroppSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (i == 2) writes[i].pBufferInfo = &shading;
        else writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    const auto& p = recordInfo.parameters;
    const float greenWb = 0.5f * (p.whiteBalance[1] + p.whiteBalance[2]);
    // Preview pixels are 2*cellScale square sensor blocks from the sensor origin; the CFA-state
    // pass has already applied lens shading and white balance.
    const ColoroppPush push{width,
                            height,
                            lscValid ? recordInfo.lensShadingMapWidth : 0u,
                            lscValid ? recordInfo.lensShadingMapHeight : 0u,
                            uint32_t(p.pattern),
                            coloroppClipValue(p.highlightThreshold),
                            lscValid ? 1u : 0u,
                            0u,
                            {coloroppWhiteBalance(p.whiteBalance[0]), coloroppWhiteBalance(greenWb),
                             coloroppWhiteBalance(p.whiteBalance[3]), 0.0f},
                            recordInfo.width,
                            recordInfo.height,
                            0u,
                            0u,
                            2u * cellScale};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, coloroppPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, coloroppLayout_, 0, 1, &r.coloroppSet, 0, nullptr);
    vkCmdPushConstants(cmd, coloroppLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (width + 15u) / 16u, (height + 15u) / 16u, 1);
    computeToCompute();

    if (p.bypassHighlightTone) return;

    VkDescriptorImageInfo toneImages[2]{{VK_NULL_HANDLE, recordInfo.outputLinearRgba16fView, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, r.sdr.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet toneWrites[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        toneWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        toneWrites[i].dstSet = r.toneSet;
        toneWrites[i].dstBinding = i;
        toneWrites[i].descriptorCount = 1;
        toneWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        toneWrites[i].pImageInfo = &toneImages[i];
    }
    vkUpdateDescriptorSets(device_, 2, toneWrites, 0, nullptr);
    const ColoroppTonePush tonePush{width, height, coloroppToneCompression(p.highlightCompression),
                                    coloroppToneExposureGain(p.highlightExposureGain)};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, coloroppTonePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, coloroppToneLayout_, 0, 1, &r.toneSet, 0, nullptr);
    vkCmdPushConstants(cmd, coloroppToneLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tonePush), &tonePush);
    vkCmdDispatch(cmd, (width + 15u) / 16u, (height + 15u) / 16u, 1);
}
}  // namespace raw_preview