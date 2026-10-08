#include "post_demosaic/PostDemosaicProcessor.h"
#include "post_demosaic/LabDefringe.h"
#include "raw_highlight/GuideChain.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "fcc_chroma_median.h"
#include "fcc_fused.h"
#include "fcc_reconstruct.h"
#include "still_apply_wb.h"
#include "still_coloropp_fused_wb.h"
#include "still_coloropp.h"
#include "still_coloropp_tone.h"
#include "still_defringe.h"
#include "still_derive_clip_state.h"
#include "still_highlight_guide_propagate.h"
#include "still_highlight_guide_seed.h"
#include "still_highlight_guide_smooth.h"
#include "still_highlight_recovery.h"
#include "denoise_img_precondition.h"
#include "denoise_img_atrous.h"
#include "denoise_img_threshold.h"
#include "denoise_img_backtransform.h"

namespace rawr::post {
namespace {
void ck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
std::vector<uint32_t> words(const unsigned char* b, size_t n) {
    if (n % 4u) throw std::runtime_error("embedded post-demosaic SPIR-V not word aligned");
    std::vector<uint32_t> out(n / 4u);
    std::memcpy(out.data(), b, n);
    return out;
}
VkImageMemoryBarrier barrier(VkImage image, VkAccessFlags src, VkAccessFlags dst,
                             VkImageLayout oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                             VkImageLayout newLayout = VK_IMAGE_LAYOUT_GENERAL) {
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

// Predicate buffer size in bytes (single flag word; room for growth).
constexpr VkDeviceSize kPredBufferSize = 16;

uint32_t findPredicateMemoryType(VkPhysicalDevice pd, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    constexpr VkMemoryPropertyFlags kPrefer = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & kPrefer) == kPrefer) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    throw std::runtime_error("post-demosaic predicate has no compatible memory type");
}

VkBufferMemoryBarrier predBufferBarrier(VkBuffer buffer, VkAccessFlags src, VkAccessFlags dst) {
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.offset = 0;
    b.size = kPredBufferSize;
    return b;
}
}  // namespace

PostDemosaicProcessor::PostDemosaicProcessor(VkPhysicalDevice pd, VkDevice device, uint32_t qf, uint32_t width,
                                             uint32_t height, uint32_t fccSteps, bool normalizedChroma,
                                             float fccEdgeSigma, float fccChromaBound, float defringeStrength,
                                             float defringeEdgeThreshold, float defringeLumaFloor)
    : device_(device),
      physicalDevice_(pd),
      width_(width),
      height_(height),
      fccSteps_(std::min(fccSteps, 8u)),
      guideWidth_((width + 7u) / 8u),
      guideHeight_((height + 7u) / 8u),
      defringeEnabled_(std::isfinite(defringeStrength) && defringeStrength > 0.0f),
      defringeStrength_(defringeEnabled_ ? std::min(defringeStrength, 1.0f) : 0.0f),
      defringeEdgeThreshold_(std::isfinite(defringeEdgeThreshold) && defringeEdgeThreshold > 0.0f
                                 ? defringeEdgeThreshold
                                 : 0.02f),
      defringeLumaFloor_(std::isfinite(defringeLumaFloor) && defringeLumaFloor >= 0.0f ? defringeLumaFloor : 0.08f) {
    // Roll back partial state on failure (destructor never runs for a
    // failed construction): otherwise every failed build leaks
    // images/pipelines until OOM.
    try {
    if (!pd || !device || !width || !height)
        throw std::invalid_argument("post-demosaic invalid Vulkan context/geometry");
    wbImage_ = rawr::vk::createOwnedImage(pd, device, width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    derivedClipState_ = rawr::vk::createOwnedImage(pd, device, width / 2u, height / 2u, VK_FORMAT_R16_UINT,
                                                          VK_IMAGE_USAGE_STORAGE_BIT);
    createWbPipeline();
    createHighlightPipeline();
    createHighlightGuidePipelines();
    createDeriveClipPipeline();
    createPredicateBuffer(pd);
    // Optional: predicated guide chain via VK_EXT_conditional_rendering when
    // the device enabled it. Null pointers select the unconditional fallback,
    // which is always correct.
    beginConditional_ = reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdBeginConditionalRenderingEXT"));
    endConditional_ = reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(
        vkGetDeviceProcAddr(device_, "vkCmdEndConditionalRenderingEXT"));
    if (fccSteps_ > 0) {
        ::fcc::VulkanContext fc{pd, device, qf, nullptr};
        const float edgeSigma = (std::isfinite(fccEdgeSigma) && fccEdgeSigma > 0.0f && fccEdgeSigma <= 1.0f)
                                    ? fccEdgeSigma
                                    : 0.0f;
        const float chromaBound = (std::isfinite(fccChromaBound) && fccChromaBound > 0.0f) ? 1.0f : 0.0f;
        fcc_ = std::make_unique<::fcc::FalseColorCorrectionPipeline>(
            fc, fccShader, ::fcc::PipelineConfig{width, height, fccSteps_, normalizedChroma, edgeSigma, chromaBound});
    }
    if (defringeEnabled_) createDefringePipeline();
    } catch (...) {
    // Construction failed mid-way (see above): roll back partial state.
    fcc_.reset();
    coloropp_.reset();
    destroyPredicateBuffer();
    destroyDeriveClipPipeline();
    destroyDefringePipeline();
    destroyHighlightGuidePipelines();
    destroyHighlightPipeline();
    destroyWbPipeline();
    if (device_) {
        rawr::vk::destroyOwnedImage(device_, derivedClipState_);
        rawr::vk::destroyOwnedImage(device_, wbImage_);
    }
    throw;
    }
}
PostDemosaicProcessor::~PostDemosaicProcessor() {
    fcc_.reset();
    coloropp_.reset();
    denoise_.reset();
    destroyPredicateBuffer();
    destroyDeriveClipPipeline();
    destroyDefringePipeline();
    destroyHighlightGuidePipelines();
    destroyHighlightPipeline();
    destroyWbPipeline();
    if (device_) {
        rawr::vk::destroyOwnedImage(device_, derivedClipState_);
        rawr::vk::destroyOwnedImage(device_, wbImage_);
        rawr::vk::destroyOwnedImage(device_, denoiseOut_);
    }
}

std::vector<uint32_t> PostDemosaicProcessor::fccShader(std::string_view name) {
    if (name == "fcc_chroma_median.comp") return words(fcc_chroma_median_spv, fcc_chroma_median_spv_size);
    if (name == "fcc_reconstruct.comp") return words(fcc_reconstruct_spv, fcc_reconstruct_spv_size);
    if (name == "fcc_fused.comp") return words(fcc_fused_spv, fcc_fused_spv_size);
    throw std::runtime_error("unknown embedded FCC shader: " + std::string(name));
}

void PostDemosaicProcessor::createWbPipeline() {
    // Bindings 0..1 are the source/destination storage images; binding 2 is
    // the highlight-guide predicate storage buffer (flag word at byte 0).
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i;
        b[i].descriptorType =
            i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds.bindingCount = 3;
    ds.pBindings = b;
    ck(vkCreateDescriptorSetLayout(device_, &ds, nullptr, &wbDsl_), "WB set layout");
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &wbDsl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    ck(vkCreatePipelineLayout(device_, &pl, nullptr, &wbLayout_), "WB pipeline layout");
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = still_apply_wb_spv_size;
    sm.pCode = reinterpret_cast<const uint32_t*>(still_apply_wb_spv);
    VkShaderModule module = VK_NULL_HANDLE;
    ck(vkCreateShaderModule(device_, &sm, nullptr, &module), "WB shader module");
    VkPipelineShaderStageCreateInfo st{};
    st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    st.module = module;
    st.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = st;
    ci.layout = wbLayout_;
    VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &wbPipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    ck(r, "WB compute pipeline");
    VkDescriptorPoolSize wbPoolSizes[2]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = wbPoolSizes;
    ck(vkCreateDescriptorPool(device_, &pi, nullptr, &wbPool_), "WB descriptor pool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = wbPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &wbDsl_;
    ck(vkAllocateDescriptorSets(device_, &ai, &wbSet_), "WB descriptor set");
}

void PostDemosaicProcessor::createHighlightPipeline() {
    VkDescriptorSetLayoutBinding b[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds.bindingCount = 4;
    ds.pBindings = b;
    ck(vkCreateDescriptorSetLayout(device_, &ds, nullptr, &hlDsl_), "HL set layout");
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &hlDsl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    ck(vkCreatePipelineLayout(device_, &pl, nullptr, &hlLayout_), "HL pipeline layout");
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = still_highlight_recovery_spv_size;
    sm.pCode = reinterpret_cast<const uint32_t*>(still_highlight_recovery_spv);
    VkShaderModule module = VK_NULL_HANDLE;
    ck(vkCreateShaderModule(device_, &sm, nullptr, &module), "HL shader module");
    VkPipelineShaderStageCreateInfo st{};
    st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    st.module = module;
    st.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = st;
    ci.layout = hlLayout_;
    VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &hlPipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    ck(r, "HL compute pipeline");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    ck(vkCreateDescriptorPool(device_, &pi, nullptr, &hlPool_), "HL descriptor pool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = hlPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &hlDsl_;
    ck(vkAllocateDescriptorSets(device_, &ai, &hlSet_), "HL descriptor set");
}

void PostDemosaicProcessor::createHighlightGuidePipelines() {
    auto spirv = [](const unsigned char* bytes, size_t byteCount) {
        return rawr::highlight::SpirvWords{reinterpret_cast<const uint32_t*>(bytes), byteCount / sizeof(uint32_t)};
    };
    guidePipelines_ = std::make_unique<rawr::highlight::GuideChainPipelines>(
        device_, rawr::highlight::GuideChainShaders{
                     spirv(still_highlight_guide_seed_spv, still_highlight_guide_seed_spv_size),
                     spirv(still_highlight_guide_propagate_spv, still_highlight_guide_propagate_spv_size),
                     spirv(still_highlight_guide_smooth_spv, still_highlight_guide_smooth_spv_size)});
    guideChain_ = std::make_unique<rawr::highlight::GuideChain>(physicalDevice_, *guidePipelines_, guideWidth_,
                                                                guideHeight_);
}

void PostDemosaicProcessor::createDeriveClipPipeline() {
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds.bindingCount = 2;
    ds.pBindings = b;
    ck(vkCreateDescriptorSetLayout(device_, &ds, nullptr, &deriveClipDsl_), "derive clip set layout");
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &deriveClipDsl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    ck(vkCreatePipelineLayout(device_, &pl, nullptr, &deriveClipLayout_), "derive clip pipeline layout");
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = still_derive_clip_state_spv_size;
    sm.pCode = reinterpret_cast<const uint32_t*>(still_derive_clip_state_spv);
    VkShaderModule module = VK_NULL_HANDLE;
    ck(vkCreateShaderModule(device_, &sm, nullptr, &module), "derive clip shader module");
    VkPipelineShaderStageCreateInfo st{};
    st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    st.module = module;
    st.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = st;
    ci.layout = deriveClipLayout_;
    VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &deriveClipPipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    ck(r, "derive clip compute pipeline");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    ck(vkCreateDescriptorPool(device_, &pi, nullptr, &deriveClipPool_), "derive clip descriptor pool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = deriveClipPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &deriveClipDsl_;
    ck(vkAllocateDescriptorSets(device_, &ai, &deriveClipSet_), "derive clip descriptor set");
}
void PostDemosaicProcessor::createPredicateBuffer(VkPhysicalDevice pd) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kPredBufferSize;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ck(vkCreateBuffer(device_, &bi, nullptr, &predBuffer_), "predicate buffer");
    try {
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, predBuffer_, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findPredicateMemoryType(pd, req.memoryTypeBits);
        ck(vkAllocateMemory(device_, &ai, nullptr, &predMemory_), "predicate memory");
        ck(vkBindBufferMemory(device_, predBuffer_, predMemory_, 0), "predicate bind");
    } catch (...) {
        destroyPredicateBuffer();
        throw;
    }
}
void PostDemosaicProcessor::destroyPredicateBuffer() noexcept {
    if (!device_) return;
    if (predBuffer_) vkDestroyBuffer(device_, predBuffer_, nullptr);
    if (predMemory_) vkFreeMemory(device_, predMemory_, nullptr);
    predBuffer_ = VK_NULL_HANDLE;
    predMemory_ = VK_NULL_HANDLE;
}
void PostDemosaicProcessor::destroyDeriveClipPipeline() noexcept {
    if (!device_) return;
    if (deriveClipPool_) vkDestroyDescriptorPool(device_, deriveClipPool_, nullptr);
    if (deriveClipPipeline_) vkDestroyPipeline(device_, deriveClipPipeline_, nullptr);
    if (deriveClipLayout_) vkDestroyPipelineLayout(device_, deriveClipLayout_, nullptr);
    if (deriveClipDsl_) vkDestroyDescriptorSetLayout(device_, deriveClipDsl_, nullptr);
    deriveClipPool_ = VK_NULL_HANDLE;
    deriveClipPipeline_ = VK_NULL_HANDLE;
    deriveClipLayout_ = VK_NULL_HANDLE;
    deriveClipDsl_ = VK_NULL_HANDLE;
    deriveClipSet_ = VK_NULL_HANDLE;
}
void PostDemosaicProcessor::destroyDefringePipeline() noexcept {
    if (!device_) return;
    if (defringePool_) vkDestroyDescriptorPool(device_, defringePool_, nullptr);
    if (defringePipeline_) vkDestroyPipeline(device_, defringePipeline_, nullptr);
    if (defringeLayout_) vkDestroyPipelineLayout(device_, defringeLayout_, nullptr);
    if (defringeDsl_) vkDestroyDescriptorSetLayout(device_, defringeDsl_, nullptr);
    defringePool_ = VK_NULL_HANDLE;
    defringePipeline_ = VK_NULL_HANDLE;
    defringeLayout_ = VK_NULL_HANDLE;
    defringeDsl_ = VK_NULL_HANDLE;
    defringeSet_ = VK_NULL_HANDLE;
}
void PostDemosaicProcessor::createDefringePipeline() {
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds.bindingCount = 2;
    ds.pBindings = b;
    ck(vkCreateDescriptorSetLayout(device_, &ds, nullptr, &defringeDsl_), "defringe set layout");
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &defringeDsl_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    ck(vkCreatePipelineLayout(device_, &pl, nullptr, &defringeLayout_), "defringe pipeline layout");
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = still_defringe_spv_size;
    sm.pCode = reinterpret_cast<const uint32_t*>(still_defringe_spv);
    VkShaderModule module = VK_NULL_HANDLE;
    ck(vkCreateShaderModule(device_, &sm, nullptr, &module), "defringe shader module");
    VkPipelineShaderStageCreateInfo st{};
    st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    st.module = module;
    st.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = st;
    ci.layout = defringeLayout_;
    VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &defringePipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    ck(r, "defringe compute pipeline");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    ck(vkCreateDescriptorPool(device_, &pi, nullptr, &defringePool_), "defringe descriptor pool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = defringePool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &defringeDsl_;
    ck(vkAllocateDescriptorSets(device_, &ai, &defringeSet_), "defringe descriptor set");
}
void PostDemosaicProcessor::destroyHighlightPipeline() noexcept {
    if (!device_) return;
    if (hlPool_) vkDestroyDescriptorPool(device_, hlPool_, nullptr);
    if (hlPipeline_) vkDestroyPipeline(device_, hlPipeline_, nullptr);
    if (hlLayout_) vkDestroyPipelineLayout(device_, hlLayout_, nullptr);
    if (hlDsl_) vkDestroyDescriptorSetLayout(device_, hlDsl_, nullptr);
    hlPool_ = VK_NULL_HANDLE;
    hlPipeline_ = VK_NULL_HANDLE;
    hlLayout_ = VK_NULL_HANDLE;
    hlDsl_ = VK_NULL_HANDLE;
    hlSet_ = VK_NULL_HANDLE;
}
void PostDemosaicProcessor::destroyHighlightGuidePipelines() noexcept {
    guideChain_.reset();
    guidePipelines_.reset();
}
void PostDemosaicProcessor::destroyWbPipeline() noexcept {
    if (!device_) return;
    if (wbPool_) vkDestroyDescriptorPool(device_, wbPool_, nullptr);
    if (wbPipeline_) vkDestroyPipeline(device_, wbPipeline_, nullptr);
    if (wbLayout_) vkDestroyPipelineLayout(device_, wbLayout_, nullptr);
    if (wbDsl_) vkDestroyDescriptorSetLayout(device_, wbDsl_, nullptr);
    wbPool_ = VK_NULL_HANDLE;
    wbPipeline_ = VK_NULL_HANDLE;
    wbLayout_ = VK_NULL_HANDLE;
    wbDsl_ = VK_NULL_HANDLE;
    wbSet_ = VK_NULL_HANDLE;
}

void PostDemosaicProcessor::record(VkCommandBuffer cmd, VkImage sourceImage, VkImageView sourceView,
                                   VkImage clipStateImage, VkImageView clipStateView, const std::array<float, 3>& gains,
                                   bool highlightRecoveryEnabled, const StillDistortionCorrection& distortion,
                                   VkQueryPool timingPool, bool preserveReconstructedHighlights,
                                   uint32_t highlightMethod, float highlightThreshold,
                                   float highlightCompression, float exposureGain,
                                   const rawr::shading::LensShadingMapView& shading, uint32_t cfaPattern,
                                   const DenoiseRequest& denoise, rawr::highlight::ColoroppSensorGeometry geometry, const float* cameraToSrgbRowMajor) {
    coloroppActive_ = highlightRecoveryEnabled && highlightMethod == 1u;
    if (coloroppActive_) {
        highlightRecoveryEnabled = false;
        preserveReconstructedHighlights = true;
    }
    if (highlightRecoveryEnabled && preserveReconstructedHighlights)
        throw std::invalid_argument("Cannot stack post-RGB recovery with pre-demosaic reconstruction preservation");
    if (!cmd || !sourceImage || !sourceView) throw std::invalid_argument("post-demosaic null source");
    if (!clipStateView) {
        if (!derivedClipInitialized_) {
            auto b = barrier(derivedClipState_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            derivedClipInitialized_ = true;
        }
        VkDescriptorImageInfo di[2]{{VK_NULL_HANDLE, sourceView, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, derivedClipState_.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet dw[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            dw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            dw[i].dstSet = deriveClipSet_;
            dw[i].dstBinding = i;
            dw[i].descriptorCount = 1;
            dw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            dw[i].pImageInfo = &di[i];
        }
        vkUpdateDescriptorSets(device_, 2, dw, 0, nullptr);
        struct DerivePc {
            uint32_t width, height;
            float threshold;
            uint32_t pad;
        } dp{width_, height_, 0.995f, 0u};
        static_assert(sizeof(DerivePc) == 16);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, deriveClipPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, deriveClipLayout_, 0, 1, &deriveClipSet_, 0,
                                nullptr);
        vkCmdPushConstants(cmd, deriveClipLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dp), &dp);
        vkCmdDispatch(cmd, ((width_ / 2u) + 15u) / 16u, ((height_ / 2u) + 15u) / 16u, 1);
        auto ready = barrier(derivedClipState_.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &ready);
        clipStateImage = derivedClipState_.image;
        clipStateView = derivedClipState_.view;
    } else if (clipStateImage) {
        auto ready = barrier(clipStateImage, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &ready);
    }
    // Profiled wavelet denoise on the pre-WB camera-linear source, after clip
    // derivation (clip evidence stays on the original sensor data) and before
    // WB. Disabled by strength <= 0: downstream stages see the source untouched.
    const bool denoiseEnabled = std::isfinite(denoise.strength) && denoise.strength > 0.0f;
    if (!denoiseEnabled && timingPool) {
        // Keep queries 10-11 written so the still timing fetch stays a fixed
        // 12-query read (0ms when denoise is off).
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 10);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 11);
    }
    if (denoiseEnabled) {
        if (!denoise_) {
            raw_denoise::DenoiseShaders shaders{};
            shaders.precondition = {reinterpret_cast<const uint32_t*>(denoise_img_precondition_spv),
                                    denoise_img_precondition_spv_size / 4u};
            shaders.atrous = {reinterpret_cast<const uint32_t*>(denoise_img_atrous_spv),
                              denoise_img_atrous_spv_size / 4u};
            shaders.threshold = {reinterpret_cast<const uint32_t*>(denoise_img_threshold_spv),
                                 denoise_img_threshold_spv_size / 4u};
            shaders.backtransform = {reinterpret_cast<const uint32_t*>(denoise_img_backtransform_spv),
                                     denoise_img_backtransform_spv_size / 4u};
            denoise_ = std::make_unique<raw_denoise::DenoisePipeline>(physicalDevice_, device_, shaders);
        }
        if (!denoiseOutInitialized_) {
            denoiseOut_ = rawr::vk::createOwnedImage(
                physicalDevice_, device_, width_, height_, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            auto b = barrier(denoiseOut_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            denoiseOutInitialized_ = true;
        }
        raw_denoise::DenoiseParams dp{};
        dp.strength = std::clamp(denoise.strength, 0.0f, 8.0f);
        dp.shadows = std::isfinite(denoise.detail) ? std::clamp(denoise.detail, 0.0f, 1.8f) : 1.0f;
        dp.noiseA = denoise.noiseA;
        dp.noiseB = denoise.noiseB;
        dp.whiteBalance = gains;
        dp.forceY = std::isfinite(denoise.forceY) ? std::clamp(denoise.forceY, 0.0f, 1.0f) : 0.25f;
        dp.maxScale = std::clamp(denoise.maxScale, 1, 7);
        denoise_->setTileBoundary(denoiseTileBoundary_);
        denoise_->record(cmd, sourceView, denoiseOut_.view, width_, height_, dp, timingPool);
        denoise_->setTileBoundary({});
        auto ready = barrier(denoiseOut_.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &ready);
        sourceImage = denoiseOut_.image;
        sourceView = denoiseOut_.view;
    }
    if (!wbImageInitialized_) {
        auto b =
            barrier(wbImage_.image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        wbImageInitialized_ = true;
    }
    VkImageView wbSourceView = sourceView;
    const bool coloroppFusedWb = coloroppActive_ && !distortion.enabled;
    // Timestamp 0 opens "Color processing": WB plus, with Inpaint Opposed, its
    // reconstruction (fused into the WB pass or recorded just before it).
    if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 0);
    if (coloroppActive_) {
        prepareColoropp();
        // Without distortion resampling the WB pass is per pixel, so Inpaint
        // Opposed writes the white-balanced result itself (recorded below in
        // place of the WB dispatch). Distortion keeps the separate taps.
        if (!coloroppFusedWb) {
            coloropp_->recordPreWb(cmd, sourceView, gains, highlightThreshold, shading, cfaPattern, geometry);
            wbSourceView = coloropp_->preWbView();
        }
    }
    // Zero the highlight predicate flag word before the WB pass ORs dirty
    // workgroup bits into it.
    vkCmdFillBuffer(cmd, predBuffer_, 0, 4, 0u);
    {
        auto fillReady = predBufferBarrier(predBuffer_, VK_ACCESS_TRANSFER_WRITE_BIT,
                                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 1, &fillReady, 0, nullptr);
    }
    VkDescriptorImageInfo ii[2]{{VK_NULL_HANDLE, wbSourceView, VK_IMAGE_LAYOUT_GENERAL},
                                {VK_NULL_HANDLE, wbImage_.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[3]{};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = wbSet_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &ii[i];
    }
    VkDescriptorBufferInfo predInfo{predBuffer_, 0, 4};
    w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[2].dstSet = wbSet_;
    w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = &predInfo;
    vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    // HL off (clamp) and Inpaint Opposed (passthrough) are per-pixel, so the
    // WB pass applies them and the separate highlight dispatch is skipped.
    // Colour propagation keeps its own pass because it needs the guide chain.
    const uint32_t highlightStage = preserveReconstructedHighlights ? 2u : (highlightRecoveryEnabled ? 1u : 0u);
    const bool highlightFused = highlightStage != 1u;
    struct Pc {
        uint32_t width, height;
        float r, g, b;
        uint32_t enableDistortion;
        float fx, fy, cx, cy;
        float k1, k2, p1, p2, k3;
        uint32_t highlightMode;
    } pc{width_,
         height_,
         gains[0],
         gains[1],
         gains[2],
         distortion.enabled ? 1u : 0u,
         distortion.intrinsic[0],
         distortion.intrinsic[1],
         distortion.intrinsic[2],
         distortion.intrinsic[3],
         distortion.distortion[0],
         distortion.distortion[1],
         distortion.distortion[2],
         distortion.distortion[3],
         distortion.distortion[4],
         highlightFused ? (highlightStage == 2u ? 2u : 1u) : 0u};
    static_assert(sizeof(Pc) == 64);
    if (coloroppFusedWb) {
        // The highlight predicate stays zero: the guide chain is off with Inpaint Opposed.
        coloropp_->recordFused(cmd, sourceView, wbImage_.view, gains, highlightThreshold, shading, cfaPattern,
                               geometry);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wbPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, wbLayout_, 0, 1, &wbSet_, 0, nullptr);
        vkCmdPushConstants(cmd, wbLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (width_ + 15u) / 16u, (height_ + 15u) / 16u, 1);
    }
    if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 1);
    auto wb = barrier(wbImage_.image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    auto src = barrier(sourceImage, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    VkImageMemoryBarrier bs[2]{wb, src};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 2, bs);
    // Make the WB-written predicate flag visible to the predicated guide
    // chain. The conditional-rendering stage bit is only valid when the
    // extension is enabled, hence the fallback stage for old devices.
    const bool predicatedGuide = beginConditional_ != nullptr && endConditional_ != nullptr;
    {
        auto predReady =
            predicatedGuide
                ? predBufferBarrier(predBuffer_, VK_ACCESS_SHADER_WRITE_BIT,
                                    VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT)
                : predBufferBarrier(predBuffer_, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             predicatedGuide ? VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT
                                             : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &predReady, 0, nullptr);
    }
    VkConditionalRenderingBeginInfoEXT condBegin{};
    condBegin.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
    condBegin.buffer = predBuffer_;
    condBegin.offset = 0;
    condBegin.flags = 0;
    guideChain_->recordInitialize(cmd);

    if (highlightFused) {
        // Nothing to dispatch: WB already wrote the highlight-stage result.
    } else if (highlightRecoveryEnabled) {
        // Predicated when supported: highlight-free frames (flag word zero)
        // skip the whole chain. The recovery pass below still runs and
        // provably reproduces the clamped input for such frames.
        const rawr::highlight::GuideChain::Conditional conditional{beginConditional_, endConditional_, &condBegin};
        VkImageView finalGuide = guideChain_->record(cmd, wbImage_.view, clipStateView, width_, height_, gains.data(),
                                                     predicatedGuide ? &conditional : nullptr);
        VkDescriptorImageInfo hi[4]{{VK_NULL_HANDLE, wbImage_.view, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, sourceView, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, clipStateView, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, finalGuide, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet hw[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            hw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            hw[i].dstSet = hlSet_;
            hw[i].dstBinding = i;
            hw[i].descriptorCount = 1;
            hw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            hw[i].pImageInfo = &hi[i];
        }
        vkUpdateDescriptorSets(device_, 4, hw, 0, nullptr);
    } else {
        // OFF deliberately performs no guide construction or reconstruction.
        VkDescriptorImageInfo hi[4]{{VK_NULL_HANDLE, wbImage_.view, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, sourceView, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, clipStateView, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, guideChain_->idleGuideView(), VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet hw[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            hw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            hw[i].dstSet = hlSet_;
            hw[i].dstBinding = i;
            hw[i].descriptorCount = 1;
            hw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            hw[i].pImageInfo = &hi[i];
        }
        vkUpdateDescriptorSets(device_, 4, hw, 0, nullptr);
    }
    struct HlPc {
        uint32_t width, height, enabled, pad;
        float clipR, clipG, clipB, clipPad;
    } hp{width_, height_, preserveReconstructedHighlights ? 2u : (highlightRecoveryEnabled ? 1u : 0u),
         0u, gains[0], gains[1], gains[2], 0.0f};
    static_assert(sizeof(HlPc) == 32);
    if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 2);
    if (!highlightFused) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hlPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hlLayout_, 0, 1, &hlSet_, 0, nullptr);
        vkCmdPushConstants(cmd, hlLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(hp), &hp);
        vkCmdDispatch(cmd, (width_ + 15u) / 16u, (height_ + 15u) / 16u, 1);
    }
    if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 3);
    // The highlight-stage result lives in wbImage_ when fused, otherwise in
    // the source image; FCC and defringe then ping-pong between the two.
    const VkImage stagedImage = highlightFused ? wbImage_.image : sourceImage;
    const VkImageView stagedView = highlightFused ? wbImage_.view : sourceView;
    const VkImage spareImage = highlightFused ? sourceImage : wbImage_.image;
    const VkImageView spareView = highlightFused ? sourceView : wbImage_.view;
    if (!highlightFused) {
        auto recovered = barrier(sourceImage, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        auto wbOut = barrier(wbImage_.image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        VkImageMemoryBarrier hs[2]{recovered, wbOut};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, hs);
    }
    VkImage correctedImage = stagedImage;
    VkImageView correctedView = stagedView;
    if (fcc_) {
        ::fcc::LinearRgbImage in{stagedImage, stagedView, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                                 width_, height_};
        ::fcc::LinearRgbImage out{spareImage, spareView, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
                                  width_, height_};
        if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 4);
        fcc_->record(cmd, in, out, fccSteps_);
        if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 5);
        correctedImage = spareImage;
        correctedView = spareView;
    } else if (timingPool) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 4);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 5);
    }
    finalImage_ = correctedImage;
    finalView_ = correctedView;
    // Timestamps 16-17 bracket defringe and the Inpaint Opposed tone tap.
    if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 16);
    if (defringeEnabled_) {
        // Alternate between the two existing images; no copy is needed.
        const VkImage targetImage = correctedImage == sourceImage ? wbImage_.image : sourceImage;
        const VkImageView targetView = correctedImage == sourceImage ? wbImage_.view : sourceView;
        auto correctedReady = barrier(correctedImage, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        auto targetWrite = barrier(targetImage, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        VkImageMemoryBarrier pre[2]{correctedReady, targetWrite};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 2, pre);
        if (cameraToSrgbRowMajor) {
            if (!labDefringe_) labDefringe_ = std::make_unique<LabDefringe>(physicalDevice_, device_, width_, height_);
            labDefringe_->record(cmd, correctedView, targetView, cameraToSrgbRowMajor, defringeStrength_);
        } else {
            // Legacy camera-RGB defringe for callers without a color matrix.
            VkDescriptorImageInfo di[2]{{VK_NULL_HANDLE, correctedView, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, targetView, VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet dw[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                dw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                dw[i].dstSet = defringeSet_;
                dw[i].dstBinding = i;
                dw[i].descriptorCount = 1;
                dw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                dw[i].pImageInfo = &di[i];
            }
            vkUpdateDescriptorSets(device_, 2, dw, 0, nullptr);
            struct DefringePc {
                uint32_t width, height;
                float strength, edgeThreshold, lumaFloor;
                uint32_t pad0, pad1, pad2;
            } dp{width_, height_, defringeStrength_, defringeEdgeThreshold_, defringeLumaFloor_, 0u, 0u, 0u};
            static_assert(sizeof(DefringePc) == 32);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, defringePipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, defringeLayout_, 0, 1, &defringeSet_, 0, nullptr);
            vkCmdPushConstants(cmd, defringeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dp), &dp);
            vkCmdDispatch(cmd, (width_ + 15u) / 16u, (height_ + 15u) / 16u, 1);
        }
        auto srcReady = barrier(targetImage, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &srcReady);
        finalImage_ = targetImage;
        finalView_ = targetView;
    }
    if (coloroppActive_ && !deferColoroppTone_) {
        auto ready = barrier(outputImage(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &ready);
        coloropp_->recordTone(cmd, outputView(), highlightCompression, exposureGain);
    }
    if (timingPool) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timingPool, 17);
}
void PostDemosaicProcessor::prepareColoropp() {
    if (coloropp_) return;
    auto spirv = [](const unsigned char* bytes, size_t byteCount) {
        return rawr::highlight::SpirvWords{reinterpret_cast<const uint32_t*>(bytes), byteCount / sizeof(uint32_t)};
    };
    coloropp_ = std::make_unique<rawr::highlight::ColoroppProcessor>(
        physicalDevice_, device_,
        rawr::highlight::ColoroppShaders{spirv(still_coloropp_spv, still_coloropp_spv_size),
                                         spirv(still_coloropp_tone_spv, still_coloropp_tone_spv_size),
                                         spirv(still_coloropp_fused_wb_spv, still_coloropp_fused_wb_spv_size)},
        width_, height_);
}

VkImageView PostDemosaicProcessor::sdrOutputView() const noexcept {
    return coloroppActive_ && coloropp_ && !deferColoroppTone_ ? coloropp_->toneView() : outputView();
}
VkImage PostDemosaicProcessor::sdrOutputImage() const noexcept {
    return coloroppActive_ && coloropp_ && !deferColoroppTone_ ? coloropp_->toneImage() : outputImage();
}
uint64_t PostDemosaicProcessor::defringeAllocatedBytes() const noexcept { return labDefringe_ ? labDefringe_->allocatedBytes() : 0; }
uint64_t PostDemosaicProcessor::fccAllocatedBytes() const noexcept { return fcc_ ? fcc_->currentAllocatedBytes() : 0; }

}  // namespace rawr::post
