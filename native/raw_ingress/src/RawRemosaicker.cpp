#include "rawr/raw_ingress/RawRemosaicker.h"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "raw_convert.h"
#include "raw_disparity.h"
#include "raw_remosaic.h"

namespace rawr::raw_ingress {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("raw remosaic: ") + what + " VkResult=" + std::to_string(r));
}
struct PushConstants {
    std::uint32_t width, height, rowStrideBytes, block, order, raw10, pass, blocksX, blocksY, parityR, parityB;
    float codesPerDn;
    float blackLevel;
    std::uint32_t fit, fitStride;
    std::uint32_t fieldRow0, fieldHeight;
    std::uint32_t disparity;
    float noiseSlope, noiseFloor;
};
struct ConvertPushConstants {
    std::uint32_t width, height, pass, order;
    float blackLevel, clipLevel;
    std::uint32_t cw, ch;
};
void memoryBarrier(VkCommandBuffer command, VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                         0, nullptr, 0, nullptr);
}
}  // namespace

struct RawRemosaicker::Pipelines {
    VkDevice device{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline general{}, detail{}, disparity{};
    VkDescriptorSetLayout convertSetLayout{};
    VkPipelineLayout convertLayout{};
    VkPipeline convert{};
    ~Pipelines() {
        if (convert) vkDestroyPipeline(device, convert, nullptr);
        if (convertLayout) vkDestroyPipelineLayout(device, convertLayout, nullptr);
        if (convertSetLayout) vkDestroyDescriptorSetLayout(device, convertSetLayout, nullptr);
        if (disparity) vkDestroyPipeline(device, disparity, nullptr);
        if (detail) vkDestroyPipeline(device, detail, nullptr);
        if (general) vkDestroyPipeline(device, general, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
};

std::shared_ptr<RawRemosaicker::Pipelines> RawRemosaicker::prepare(const rawr::vk::GpuContext& context,
                                                               VkPipelineCache cache) {
    if (!context.device || !context.physicalDevice) throw std::invalid_argument("raw remosaic: no device");
    auto result = std::make_shared<Pipelines>();
    const VkDevice d = result->device = context.device;
    auto& setLayout_ = result->setLayout;
    auto& pipelineLayout_ = result->layout;
    auto& pipeline_ = result->general;
    auto& detailPipeline_ = result->detail;
    std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<std::uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &setLayout_), "set layout");

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(d, &pli, nullptr, &pipelineLayout_), "pipeline layout");

    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = raw_remosaic_spv_size;
    mi.pCode = reinterpret_cast<const std::uint32_t*>(raw_remosaic_spv);
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(d, &mi, nullptr, &module), "shader module");
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = pipelineLayout_;
    VkResult created = vkCreateComputePipelines(d, cache, 1, &ci, nullptr, &pipeline_);
    if (created == VK_SUCCESS) {
        const std::uint32_t enabled = 9u;
        VkSpecializationMapEntry entry{0, 0, sizeof(enabled)};
        VkSpecializationInfo spec{1, &entry, sizeof(enabled), &enabled};
        ci.stage.pSpecializationInfo = &spec;
        created = vkCreateComputePipelines(d, cache, 1, &ci, nullptr, &detailPipeline_);
    }
    vkDestroyShaderModule(d, module, nullptr);
    check(created, "pipeline");
    // Disparity correction: its own module (shared memory), same layout.
    mi.codeSize = raw_disparity_spv_size;
    mi.pCode = reinterpret_cast<const std::uint32_t*>(raw_disparity_spv);
    check(vkCreateShaderModule(d, &mi, nullptr, &module), "disparity shader module");
    ci.stage.module = module;
    ci.stage.pSpecializationInfo = nullptr;
    created = vkCreateComputePipelines(d, cache, 1, &ci, nullptr, &result->disparity);
    vkDestroyShaderModule(d, module, nullptr);
    check(created, "disparity pipeline");

    // Thin-feature stage: its own descriptor set (corrected quad, Detail's output, destination, four cell grids,
    // each written through one view and read through a readonly alias).
    {
        std::array<VkDescriptorSetLayoutBinding, 11> cb{};
        for (std::uint32_t i = 0; i < cb.size(); ++i)
            cb[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo cli{};
        cli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        cli.bindingCount = static_cast<std::uint32_t>(cb.size());
        cli.pBindings = cb.data();
        check(vkCreateDescriptorSetLayout(d, &cli, nullptr, &result->convertSetLayout), "convert set layout");
        VkPushConstantRange crange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ConvertPushConstants)};
        VkPipelineLayoutCreateInfo cpli{};
        cpli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        cpli.setLayoutCount = 1;
        cpli.pSetLayouts = &result->convertSetLayout;
        cpli.pushConstantRangeCount = 1;
        cpli.pPushConstantRanges = &crange;
        check(vkCreatePipelineLayout(d, &cpli, nullptr, &result->convertLayout), "convert pipeline layout");
        mi.codeSize = raw_convert_spv_size;
        mi.pCode = reinterpret_cast<const std::uint32_t*>(raw_convert_spv);
        check(vkCreateShaderModule(d, &mi, nullptr, &module), "convert shader module");
        ci.stage.module = module;
        ci.stage.pSpecializationInfo = nullptr;
        ci.layout = result->convertLayout;
        created = vkCreateComputePipelines(d, cache, 1, &ci, nullptr, &result->convert);
        vkDestroyShaderModule(d, module, nullptr);
        check(created, "convert pipeline");
    }

    return result;
}

std::unique_ptr<RawRemosaicker> RawRemosaicker::create(const rawr::vk::GpuContext& context,
                                                    std::shared_ptr<Pipelines> pipelines) {
    return std::unique_ptr<RawRemosaicker>(new RawRemosaicker(context, std::move(pipelines)));
}

RawRemosaicker::RawRemosaicker(const rawr::vk::GpuContext& context, std::shared_ptr<Pipelines> pipelines)
    : pipelines_(pipelines ? std::move(pipelines) : prepare(context)), context_(context) {
    if (pipelines_->device != context.device) throw std::invalid_argument("raw remosaic: pipeline device mismatch");
    setLayout_ = pipelines_->setLayout;
    pipelineLayout_ = pipelines_->layout;
    pipeline_ = pipelines_->general;
    detailPipeline_ = pipelines_->detail;
    disparityPipeline_ = pipelines_->disparity;
    convertSetLayout_ = pipelines_->convertSetLayout;
    convertPipelineLayout_ = pipelines_->convertLayout;
    convertPipeline_ = pipelines_->convert;
    try {
        const VkDevice d = context_.device;
        std::array<VkDescriptorPoolSize, 2> sizes{
            {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (8 + 11) * kSlots}}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 2 * kSlots;
        pi.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pi.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(d, &pi, nullptr, &pool_), "descriptor pool");
        std::array<VkDescriptorSetLayout, kSlots> layouts{};
        layouts.fill(setLayout_);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = kSlots;
        ai.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(d, &ai, sets_.data()), "descriptor sets");
        layouts.fill(convertSetLayout_);
        ai.pSetLayouts = layouts.data();
        check(vkAllocateDescriptorSets(d, &ai, convertSets_.data()), "convert descriptor sets");
    } catch (...) {
        destroy();
        throw;
    }
}

RawRemosaicker::~RawRemosaicker() { destroy(); }

void RawRemosaicker::destroy() noexcept {
    const VkDevice d = context_.device;
    if (!d) return;
    releaseScratch();
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    pipelines_.reset();
    pool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    detailPipeline_ = VK_NULL_HANDLE;
    disparityPipeline_ = VK_NULL_HANDLE;
    convertPipeline_ = VK_NULL_HANDLE;
    convertPipelineLayout_ = VK_NULL_HANDLE;
    convertSetLayout_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void RawRemosaicker::releaseScratch() noexcept {
    const VkDevice d = context_.device;
    rawr::vk::destroyOwnedImage(d, scratch_.raw);
    rawr::vk::destroyOwnedImage(d, scratch_.green);
    rawr::vk::destroyOwnedImage(d, scratch_.moments);
    rawr::vk::destroyOwnedImage(d, scratch_.pd);
    rawr::vk::destroyOwnedImage(d, scratch_.detailOut);
    rawr::vk::destroyOwnedImage(d, scratch_.cell);
    rawr::vk::destroyOwnedImage(d, scratch_.ls);
    rawr::vk::destroyOwnedImage(d, scratch_.lf);
    rawr::vk::destroyOwnedImage(d, scratch_.wm);
    if (scratch_.diff) vkDestroyBuffer(d, scratch_.diff, nullptr);
    if (scratch_.diffMemory) vkFreeMemory(d, scratch_.diffMemory, nullptr);
    scratch_ = {};
}

void RawRemosaicker::ensureScratch(std::uint32_t width, std::uint32_t height, std::uint32_t block, bool needsDiff,
                                   bool needsMoments, bool needsPd, bool needsConvert) {
    if (scratch_.raw.image && scratch_.width == width && scratch_.height == height && scratch_.block == block &&
        scratch_.needsDiff == needsDiff && scratch_.needsMoments == needsMoments && scratch_.needsPd == needsPd &&
        scratch_.needsConvert == needsConvert)
        return;
    releaseScratch();
    const VkDevice d = context_.device;
    const auto make = [&](VkDeviceSize bytes, VkBuffer& buffer, VkDeviceMemory& memory) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;  // gain field upload
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(d, &bi, nullptr, &buffer), "scratch buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(d, buffer, &req);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice, &props);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = i;
        for (std::uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
            if (req.memoryTypeBits & (1u << i)) type = i;
        if (type == UINT32_MAX) throw std::runtime_error("raw remosaic: no memory type for scratch");
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        check(vkAllocateMemory(d, &ai, nullptr, &memory), "scratch memory");
        check(vkBindBufferMemory(d, buffer, memory, 0), "scratch bind");
    };
    try {
        const std::uint32_t bw = (width + block - 1) / block, bh = (height + block - 1) / block;
        scratch_.raw = rawr::vk::createOwnedImage(context_.physicalDevice, d, width, height, VK_FORMAT_R16_UINT,
                                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        scratch_.green = rawr::vk::createOwnedImage(context_.physicalDevice, d, width, height, VK_FORMAT_R16_UINT,
                                                    VK_IMAGE_USAGE_STORAGE_BIT);
        // Detail block moments: one texel per red/blue block (bx >> 1, by); a 1x1 placeholder otherwise. The
        // disparity correction keeps its regression terms here first (one texel per 4 x 4-block cell).
        const std::uint32_t momentsWidth = std::max(needsMoments ? (bw + 1) / 2 : 1u, needsPd ? (bw + 3) / 4 : 1u);
        const std::uint32_t momentsHeight = std::max(needsMoments ? bh : 1u, needsPd ? (bh + 3) / 4 : 1u);
        scratch_.moments = rawr::vk::createOwnedImage(context_.physicalDevice, d, momentsWidth, momentsHeight,
                                                      VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        // Disparity input (the loaded raw); a 1x1 placeholder otherwise.
        scratch_.pd = rawr::vk::createOwnedImage(context_.physicalDevice, d, needsPd ? width : 1, needsPd ? height : 1,
                                                 VK_FORMAT_R16_UINT,
                                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        // Thin-feature stage: Detail's output and the cell grids (1x1 placeholders otherwise).
        const std::uint32_t cells = needsConvert ? width / 4 : 1u, cellRows = needsConvert ? height / 4 : 1u;
        scratch_.detailOut = rawr::vk::createOwnedImage(context_.physicalDevice, d, needsConvert ? width : 1,
                                                        needsConvert ? height : 1, VK_FORMAT_R16_UINT,
                                                        VK_IMAGE_USAGE_STORAGE_BIT);
        for (auto* grid : {&scratch_.cell, &scratch_.ls, &scratch_.lf, &scratch_.wm})
            *grid = rawr::vk::createOwnedImage(context_.physicalDevice, d, cells, cellRows, VK_FORMAT_R16G16B16A16_SFLOAT,
                                               VK_IMAGE_USAGE_STORAGE_BIT);
        // Detail needs no block statistics; retain a valid minimal descriptor when phase fitting is off.
        make(needsDiff ? std::max<VkDeviceSize>(VkDeviceSize(bw) * bh * 8u, kGainFieldFloats * 4u) : VkDeviceSize(4),
             scratch_.diff, scratch_.diffMemory);
    } catch (...) {
        releaseScratch();
        throw;
    }
    scratch_.width = width;
    scratch_.height = height;
    scratch_.block = block;
    scratch_.needsDiff = needsDiff;
    scratch_.needsMoments = needsMoments;
    scratch_.needsPd = needsPd;
    scratch_.needsConvert = needsConvert;
}

void RawRemosaicker::record(VkCommandBuffer command, std::uint32_t slot, const Frame& f) {
    if (slot >= kSlots || !f.source || !f.destination || f.width < 2u * f.block || f.height < 2u * f.block ||
        f.width % 2u != 0 || (f.block != 2u && f.block != 4u) || f.order > 3u || (f.raw10 && f.width % 4u != 0) ||
        (!f.raw10 && f.rowStrideBytes % 2u != 0))
        throw std::invalid_argument("raw remosaic: invalid record arguments");
    if (f.rowStrideBytes < (f.raw10 ? f.width / 4u * 5u : f.width * 2u))
        throw std::invalid_argument("raw remosaic: row stride below width");
    if (f.phaseFit && (!f.hexadecaToQuad || f.fitStride % 2u == 0u || f.width / 4u < 9u || f.height / 4u < 9u))
        throw std::invalid_argument("raw remosaic: phase fit needs the Hexadeca reduction, an odd stride, 9x9 blocks");
    const bool field = f.gainField != nullptr;
    if (field && (f.raw10 || f.hexadecaToQuad || f.block != 2u || f.fieldHeight < f.fieldRow0 + f.height))
        throw std::invalid_argument("raw remosaic: gain field needs quad RAW16 inside its frame");
    const bool disparity = f.disparity;
    if (disparity && (f.raw10 || f.hexadecaToQuad || f.block != 2u || f.width < 8u || f.height < 8u))
        throw std::invalid_argument("raw remosaic: disparity correction needs quad RAW16");
    const bool convert = f.convert;
    if (convert && (f.quality != Quality::Detail || f.block != 2u || f.hexadecaToQuad || f.width % 4u || f.height % 4u ||
                    f.width < 32u || f.height < 32u))
        throw std::invalid_argument("raw remosaic: the thin-feature stage needs quad Detail, dimensions a multiple of 4");
    const bool quadOnly = f.quality == Quality::High || f.quality == Quality::Detail;
    if ((f.hexadecaToQuad && (f.block != 4u || !quadOnly || f.width % 8u || f.height % 8u)) ||
        (quadOnly && !f.hexadecaToQuad && f.block != 2u) || static_cast<unsigned>(f.quality) > 3u)
        throw std::invalid_argument("raw remosaic: unsupported High/reduction configuration");
    const auto width = f.hexadecaToQuad ? f.width / 2u : f.width;
    const auto height = f.hexadecaToQuad ? f.height / 2u : f.height;
    const auto block = f.hexadecaToQuad ? 2u : f.block;
    ensureScratch(width, height, block, f.quality != Quality::Detail || f.phaseFit || field, f.quality == Quality::Detail,
                  disparity, convert);
    // The slot has retired before record(). Always refresh descriptors:
    // callers may destroy/recreate buffers and views with recycled handle
    // values, which cannot safely serve as resource-lifetime cache keys.
    {
        VkDescriptorBufferInfo source{f.source, 0, VK_WHOLE_SIZE};
        // With the thin-feature stage Detail writes its own image and `destination` receives the final result.
        VkDescriptorImageInfo image{VK_NULL_HANDLE, convert ? scratch_.detailOut.view : f.destination,
                                    VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo raw{VK_NULL_HANDLE, scratch_.raw.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo green{VK_NULL_HANDLE, scratch_.green.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo diff{scratch_.diff, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo moments{VK_NULL_HANDLE, scratch_.moments.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo pd{VK_NULL_HANDLE, scratch_.pd.view, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 11> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = sets_[slot];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &source;
        writes[1].pImageInfo = &image;
        writes[2].pImageInfo = &raw;
        writes[3].pImageInfo = &green;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo = &diff;
        writes[5].pImageInfo = &raw;
        writes[6].pImageInfo = &green;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[7].pBufferInfo = &diff;
        writes[8].pImageInfo = &moments;
        writes[9].pImageInfo = &moments;
        writes[10].pImageInfo = &pd;
        vkUpdateDescriptorSets(context_.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        if (convert) {
            const VkImageView views[11] = {scratch_.raw.view,  scratch_.detailOut.view, f.destination,      scratch_.cell.view,
                                           scratch_.cell.view, scratch_.ls.view,        scratch_.ls.view,   scratch_.lf.view,
                                           scratch_.lf.view,   scratch_.wm.view,        scratch_.wm.view};
            VkDescriptorImageInfo infos[11];
            std::array<VkWriteDescriptorSet, 11> cw{};
            for (std::uint32_t i = 0; i < 11; ++i) {
                infos[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
                cw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                cw[i].dstSet = convertSets_[slot];
                cw[i].dstBinding = i;
                cw[i].descriptorCount = 1;
                cw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                cw[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(context_.device, 11, cw.data(), 0, nullptr);
        }
    }
    const std::uint32_t bw = (width + block - 1) / block, bh = (height + block - 1) / block;
    // Block parity ((y & 1) << 1 | (x & 1)) of red and blue for this order (0=RGGB 1=GRBG 2=GBRG 3=BGGR).
    static constexpr std::uint32_t kParityR[4] = {0u, 1u, 2u, 3u}, kParityB[4] = {3u, 2u, 1u, 0u};
    PushConstants pc{width,
                     height,
                     f.rowStrideBytes,
                     block,
                     f.order,
                     f.raw10 ? 1u : 0u,
                     0u,
                     bw,
                     bh,
                     kParityR[f.order],
                     kParityB[f.order],
                     f.hexadecaToQuad ? 4.0f : 1.0f,
                     f.blackLevel,
                     f.phaseFit ? 1u : 0u,
                     f.fitStride,
                     f.fieldRow0,
                     f.fieldHeight,
                     disparity ? 1u : 0u,
                     f.noiseSlope,
                     f.noiseFloor};
    const auto stamp = [&](std::uint32_t index) {
        if (f.timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, f.timestamps, index);
    };
    stamp(0);
    // The scratch is shared across slots: the previous frame's passes must be
    // done with it before this frame writes it. Its contents never carry over,
    // so each frame starts from UNDEFINED.
    VkImageMemoryBarrier toWrite[9]{};
    for (int i = 0; i < 9; ++i) {
        toWrite[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toWrite[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toWrite[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        toWrite[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toWrite[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toWrite[i].srcQueueFamilyIndex = toWrite[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toWrite[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    toWrite[0].image = scratch_.raw.image;
    toWrite[1].image = scratch_.green.image;
    toWrite[2].image = scratch_.moments.image;
    toWrite[3].image = scratch_.pd.image;
    std::uint32_t transitions = 4;
    if (convert)
        for (auto* image : {&scratch_.detailOut, &scratch_.cell, &scratch_.ls, &scratch_.lf, &scratch_.wm})
            toWrite[transitions++].image = image->image;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, transitions, toWrite);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[slot], 0, nullptr);
    const auto pass = [&](std::uint32_t index, std::uint32_t gx, std::uint32_t gy) {
        const VkPipeline selected = index == 9u ? detailPipeline_ : index == 15u || index == 16u ? disparityPipeline_ : pipeline_;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, selected);
        pc.pass = index;
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(command, (gx + 15u) / 16u, (gy + 15u) / 16u, 1);
    };
    // Raw rows into the cached image: a transfer for RAW16, an unpack for RAW10.
    if (f.hexadecaToQuad) {
        if (f.phaseFit) {
            // Statistics and curve live in the diff buffer; the green image
            // holds the 4x4 block sums until the green pass overwrites it.
            const auto rw = [&] {
                memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            };
            const std::uint32_t bw4 = width / 2u, bh4 = height / 2u;
            pass(13u, 16u, 16u);
            rw();
            pass(10u, bw4, bh4);
            rw();
            stamp(5);
            pass(11u, (bw4 - 8u) / f.fitStride + 1u, (bh4 - 8u) / f.fitStride + 1u);
            rw();
            stamp(6);
            pass(12u, 16u, 16u);
            rw();
            stamp(7);
        }
        pass(6u, width, height);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else if (field) {
        // Coefficients into the diff buffer (the previous frame's passes are done reading it), then the load.
        VkMemoryBarrier before{};
        before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before,
                             0, nullptr, 0, nullptr);
        vkCmdUpdateBuffer(command, scratch_.diff, 0, kGainFieldFloats * sizeof(float), f.gainField);
        VkMemoryBarrier after{};
        after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &after, 0,
                             nullptr, 0, nullptr);
        pass(14u, f.width, f.height);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else if (f.raw10) {
        pass(3u, f.width, f.height);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
        VkBufferImageCopy region{};
        region.bufferRowLength = f.rowStrideBytes / 2u;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {f.width, f.height, 1};
        // With the disparity correction the load goes to its input image.
        vkCmdCopyBufferToImage(command, f.source, disparity ? scratch_.pd.image : scratch_.raw.image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    }
    if (disparity) {
        // One workgroup per 16 x 16 blocks (the pass helper divides by 16): gate + terms into the
        // moments image, then window sums + correction into the raw image.
        stamp(4);
        pass(15u, width / 2u, height / 2u);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        stamp(5);
        pass(16u, width / 2u, height / 2u);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    stamp(1);
    // One invocation per 2x2 pixel group (see the shader).
    const bool fast = f.quality == Quality::Fast;
    const bool detail = f.quality == Quality::Detail;
    if (detail) {
        // Green + block moments per red/blue block, then Bayer out.
        pass(8u, (width + 1u) / 2u, (height + 1u) / 2u);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        stamp(2);
        pass(9u, (width + 1u) / 2u, (height + 1u) / 2u);
        if (convert) {
            memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            stamp(6);  // thin-feature stage start (the pool of 8 is otherwise only the Hexadeca phase fit's)
            ConvertPushConstants cpc{width, height, 0u, f.order, f.blackLevel, f.convertClip, width / 4u, height / 4u};
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, convertPipeline_);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, convertPipelineLayout_, 0, 1,
                                    &convertSets_[slot], 0, nullptr);
            const auto convertPass = [&](std::uint32_t index, std::uint32_t gx, std::uint32_t gy) {
                cpc.pass = index;
                vkCmdPushConstants(command, convertPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), &cpc);
                vkCmdDispatch(command, (gx + 15u) / 16u, (gy + 15u) / 16u, 1);
            };
            convertPass(0u, cpc.cw, cpc.ch);  // cell statistics
            memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            convertPass(1u, cpc.cw, cpc.ch);  // coarse / fine colour planes, ratio disagreement
            memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            convertPass(2u, cpc.cw, cpc.ch);  // weights
            memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            convertPass(3u, width, height);  // per pixel
            stamp(7);
        }
    } else {
        // Block means / green + colour difference.
        pass(fast ? 4u : 0u, (width + 1u) / 2u, (height + 1u) / 2u);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        stamp(2);
        pass(fast ? 5u : (f.quality == Quality::High ? 7u : 2u), (width + 1u) / 2u, (height + 1u) / 2u);  // Bayer out
    }
    stamp(3);
}

}  // namespace rawr::raw_ingress
