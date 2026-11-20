#include "rawr/raw_ingress/BayerBinner.h"
#include "rawr/raw_ingress/BayerBinKernels.h"
#include "rawr/raw_ingress/BayerBinNoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "bayer_lin0.h"
#include "bayer_lin1.h"
#include "bayer_lin2.h"
#include "bayer_lin3.h"
#include "bayer_resample.h"

namespace rawr::raw_ingress {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("bayer bin: ") + what + " VkResult=" + std::to_string(r));
}
struct BinPc {
    float scale[4];
    float black[4];
    float gains[4];
    std::uint32_t sw, sh, strideElems, ow, oh, order;
    float span, white, codeGain;
};
static_assert(sizeof(BinPc) == 84, "push constant layout");
struct ResamplePc {
    std::uint32_t iw, ih, ow, oh;
    float sx, sy;
    std::int32_t rx, ry;
    float maxCode;
};
constexpr std::uint32_t kBands = 26, kBins = 128;
constexpr VkDeviceSize kHistBytes = kBands * kBins * 4, kFloorBytes = kBands * 4;
void barrier(VkCommandBuffer command, VkPipelineStageFlags srcStage, VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(command, srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}
}  // namespace

std::unique_ptr<BayerBinner> BayerBinner::create(const rawr::vk::GpuContext& context) {
    return std::unique_ptr<BayerBinner>(new BayerBinner(context));
}

BayerBinner::BayerBinner(const rawr::vk::GpuContext& context) : context_(context) {
    if (!context_.device || !context_.physicalDevice) throw std::invalid_argument("bayer bin: no device");
    try {
        std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kSlots},
                                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7 * kSlots},
                                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots}}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 2 * kSlots;
        pi.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pi.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(context_.device, &pi, nullptr, &pool_), "descriptor pool");
        constexpr VkDescriptorType B = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        const VkDescriptorType binTypes[9] = {B, I, I, I, I, B, B, I, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        makeLayout(bin_, binTypes, 9, sizeof(BinPc));
        {
            // The weights, once: host-visible, written through a mapping.
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = 2 * sizeof(kBayerBinPlain);
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            check(vkCreateBuffer(context_.device, &bi, nullptr, &kernels_), "kernel buffer");
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(context_.device, kernels_, &req);
            VkPhysicalDeviceMemoryProperties props{};
            vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice, &props);
            constexpr auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            std::uint32_t type = UINT32_MAX;
            for (std::uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
                if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) type = i;
            if (type == UINT32_MAX) throw std::runtime_error("bayer bin: no host-visible memory for the kernels");
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            check(vkAllocateMemory(context_.device, &ai, nullptr, &kernelsMemory_), "kernel memory");
            check(vkBindBufferMemory(context_.device, kernels_, kernelsMemory_, 0), "kernel bind");
            void* mapped = nullptr;
            check(vkMapMemory(context_.device, kernelsMemory_, 0, bi.size, 0, &mapped), "kernel map");
            std::memcpy(mapped, kBayerBinPlain, sizeof(kBayerBinPlain));
            std::memcpy(static_cast<char*>(mapped) + sizeof(kBayerBinPlain), kBayerBinSharpDelta, sizeof(kBayerBinSharpDelta));
            vkUnmapMemory(context_.device, kernelsMemory_);
        }
        binPasses_[0] = makeComputePipeline(bin_.layout, bayer_lin0_spv, bayer_lin0_spv_size);
        binPasses_[1] = makeComputePipeline(bin_.layout, bayer_lin1_spv, bayer_lin1_spv_size);
        binPasses_[2] = makeComputePipeline(bin_.layout, bayer_lin2_spv, bayer_lin2_spv_size);
        binPasses_[3] = makeComputePipeline(bin_.layout, bayer_lin3_spv, bayer_lin3_spv_size);
        const VkDescriptorType resampleTypes[2] = {I, I};
        makeLayout(resample_, resampleTypes, 2, sizeof(ResamplePc));
        resample_.pipeline = makeComputePipeline(resample_.layout, bayer_resample_spv, bayer_resample_spv_size);
    } catch (...) {
        destroy();
        throw;
    }
}

BayerBinner::~BayerBinner() { destroy(); }

void BayerBinner::makeLayout(Pipeline& p, const VkDescriptorType* types, std::uint32_t count, std::uint32_t pushBytes) {
    const VkDevice d = context_.device;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (std::uint32_t i = 0; i < count; ++i) bindings.push_back({i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = count;
    li.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(d, &li, nullptr, &p.setLayout), "set layout");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &p.setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(d, &pli, nullptr, &p.layout), "pipeline layout");
    std::array<VkDescriptorSetLayout, kSlots> layouts{};
    layouts.fill(p.setLayout);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = kSlots;
    ai.pSetLayouts = layouts.data();
    check(vkAllocateDescriptorSets(d, &ai, p.sets.data()), "descriptor sets");
}

VkPipeline BayerBinner::makeComputePipeline(VkPipelineLayout layout, const unsigned char* spirv,
                                            std::size_t spirvBytes) {
    const VkDevice d = context_.device;
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = spirvBytes;
    mi.pCode = reinterpret_cast<const std::uint32_t*>(spirv);
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(d, &mi, nullptr, &module), "shader module");
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult created = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    vkDestroyShaderModule(d, module, nullptr);
    check(created, "pipeline");
    return pipeline;
}

void BayerBinner::destroy() noexcept {
    const VkDevice d = context_.device;
    if (!d) return;
    releaseScratch();
    if (kernels_) vkDestroyBuffer(d, kernels_, nullptr);
    if (kernelsMemory_) vkFreeMemory(d, kernelsMemory_, nullptr);
    kernels_ = VK_NULL_HANDLE;
    kernelsMemory_ = VK_NULL_HANDLE;
    for (auto& p : binPasses_)
        if (p) vkDestroyPipeline(d, p, nullptr);
    binPasses_ = {};
    for (auto* p : {&bin_, &resample_}) {
        if (p->pipeline) vkDestroyPipeline(d, p->pipeline, nullptr);
        if (p->layout) vkDestroyPipelineLayout(d, p->layout, nullptr);
        if (p->setLayout) vkDestroyDescriptorSetLayout(d, p->setLayout, nullptr);
        *p = {};
    }
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
}

std::uint64_t BayerBinner::scratchBytes(std::uint32_t outWidth, std::uint32_t outHeight) {
    return std::uint64_t(outWidth) * outHeight * 4 + kHistBytes + kFloorBytes;
}

double BayerBinner::noiseVarianceGain(std::uint32_t outColour, const std::array<float, 3>& gains) {
    return bayerBinNoiseVarianceGain(outColour, gains);
}

void BayerBinner::releaseScratch() noexcept {
    const VkDevice d = context_.device;
    rawr::vk::destroyOwnedImage(d, scratch_.base);
    rawr::vk::destroyOwnedImage(d, scratch_.d);
    if (scratch_.stats) vkDestroyBuffer(d, scratch_.stats, nullptr);
    if (scratch_.statsMemory) vkFreeMemory(d, scratch_.statsMemory, nullptr);
    scratch_ = {};
}

void BayerBinner::ensureScratch(std::uint32_t ow, std::uint32_t oh) {
    if (scratch_.base.image && scratch_.width == ow && scratch_.height == oh) return;
    releaseScratch();
    const VkDevice d = context_.device;
    try {
        scratch_.base = rawr::vk::createOwnedImage(context_.physicalDevice, d, ow, oh, VK_FORMAT_R16_UINT,
                                                   VK_IMAGE_USAGE_STORAGE_BIT);
        scratch_.d = rawr::vk::createOwnedImage(context_.physicalDevice, d, ow, oh, VK_FORMAT_R16_SFLOAT,
                                                VK_IMAGE_USAGE_STORAGE_BIT);
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = kHistBytes + kFloorBytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        check(vkCreateBuffer(d, &bi, nullptr, &scratch_.stats), "stats buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(d, scratch_.stats, &req);
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice, &props);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = i;
        for (std::uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
            if (req.memoryTypeBits & (1u << i)) type = i;
        if (type == UINT32_MAX) throw std::runtime_error("bayer bin: no memory type for stats");
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        check(vkAllocateMemory(d, &ai, nullptr, &scratch_.statsMemory), "stats memory");
        check(vkBindBufferMemory(d, scratch_.stats, scratch_.statsMemory, 0), "stats bind");
    } catch (...) {
        releaseScratch();
        throw;
    }
    scratch_.width = ow;
    scratch_.height = oh;
}

void BayerBinner::bin(VkCommandBuffer command, std::uint32_t slot, const Bin& f) {
    if (slot >= kSlots || !f.source || !f.destination || f.width < 32 || f.height < 32 || f.width % 4 ||
        f.height % 4 || f.rowStrideBytes < f.width * 2 || f.rowStrideBytes % 2 || f.order > 3)
        throw std::invalid_argument("bayer bin: invalid arguments");
    if (!(f.white > 0) || !(f.codeGain > 0) || f.white * f.codeGain > 65535.5f)
        throw std::invalid_argument("bayer bin: white level times code gain must fit 16 bits");
    float maxBlack = 0, maxGain = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(f.black[i]) || f.black[i] < 0 || f.black[i] >= f.white)
            throw std::invalid_argument("bayer bin: black level must be below white");
        if (!std::isfinite(f.wbGains[i]) || !(f.wbGains[i] > 0))
            throw std::invalid_argument("bayer bin: white balance gains must be positive");
        maxBlack = std::max(maxBlack, f.black[i]);
        maxGain = std::max(maxGain, f.wbGains[i]);
    }
    const std::uint32_t ow = f.width / 2, oh = f.height / 2;
    ensureScratch(ow, oh);
    {
        VkDescriptorBufferInfo source{f.source, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo hist{scratch_.stats, 0, kHistBytes};
        VkDescriptorBufferInfo floors{scratch_.stats, kHistBytes, kFloorBytes};
        VkDescriptorBufferInfo weights{kernels_, 0, VK_WHOLE_SIZE};
        const VkImageView views[8] = {VK_NULL_HANDLE, scratch_.base.view, scratch_.base.view, scratch_.d.view,
                                      scratch_.d.view, VK_NULL_HANDLE, VK_NULL_HANDLE, f.destination};
        const VkDescriptorBufferInfo* buffers[8] = {&source, nullptr, nullptr, nullptr, nullptr, &hist, &floors, nullptr};
        std::array<VkDescriptorImageInfo, 8> infos{};
        std::array<VkWriteDescriptorSet, 9> writes{};
        writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet = bin_.sets[slot];
        writes[8].dstBinding = 8;
        writes[8].descriptorCount = 1;
        writes[8].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[8].pBufferInfo = &weights;
        for (std::uint32_t i = 0; i < 8; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = bin_.sets[slot];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            if (buffers[i]) {
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = buffers[i];
            } else {
                infos[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].pImageInfo = &infos[i];
            }
        }
        vkUpdateDescriptorSets(context_.device, 9, writes.data(), 0, nullptr);
    }
    if (f.timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, f.timestamps, 0);
    {
        // Scratch never carries over between frames: images from UNDEFINED, histogram cleared.
        std::array<VkImageMemoryBarrier, 2> toGeneral{};
        const VkImage images[2] = {scratch_.base.image, scratch_.d.image};
        for (int i = 0; i < 2; ++i) {
            auto& b = toGeneral[i];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = images[i];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, toGeneral.data());
        vkCmdFillBuffer(command, scratch_.stats, 0, kHistBytes, 0);
        barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, bin_.layout, 0, 1, &bin_.sets[slot], 0, nullptr);
    BinPc pc{};
    const float span = f.white - maxBlack;
    for (int i = 0; i < 4; ++i) {
        pc.gains[i] = f.wbGains[i] / maxGain;
        pc.scale[i] = pc.gains[i] / span;
        pc.black[i] = f.black[i];
    }
    pc.sw = f.width;
    pc.sh = f.height;
    pc.strideElems = f.rowStrideBytes / 2;
    pc.ow = ow;
    pc.oh = oh;
    pc.order = f.order;
    pc.span = span;
    pc.white = f.white;
    pc.codeGain = f.codeGain;
    const std::uint32_t groups[4][2] = {{(ow + 31) / 32, (oh + 31) / 32},
                                        {((ow + 3) / 4 + 255) / 256, (oh + 3) / 4},
                                        {1, 1},
                                        {(ow + 15) / 16, (oh + 15) / 16}};
    for (std::uint32_t pass = 0; pass < 4; ++pass) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, binPasses_[pass]);
        vkCmdPushConstants(command, bin_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(command, groups[pass][0], groups[pass][1], 1);
        barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    if (f.timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, f.timestamps, 1);
}

void BayerBinner::resample(VkCommandBuffer command, std::uint32_t slot, const Resample& f) {
    if (slot >= kSlots || !f.source || !f.destination || f.inWidth < 8 || f.inHeight < 8 || f.outWidth < 2 ||
        f.outHeight < 2 || (f.outWidth & 1) || (f.outHeight & 1) || f.outWidth > f.inWidth ||
        f.outHeight > f.inHeight)
        throw std::invalid_argument("bayer resample: invalid arguments");
    const float sx = float(double(f.inWidth) / f.outWidth), sy = float(double(f.inHeight) / f.outHeight);
    if (sx > 1.5f || sy > 1.5f) throw std::invalid_argument("bayer resample: scale above 1.5");
    {
        VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, f.source, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, f.destination, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2]{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = resample_.sets[slot];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(context_.device, 2, writes, 0, nullptr);
    }
    ResamplePc pc{f.inWidth,
                  f.inHeight,
                  f.outWidth,
                  f.outHeight,
                  sx,
                  sy,
                  std::int32_t(std::ceil(2.0 * std::max(1.0, double(f.inWidth) / f.outWidth))) + 1,
                  std::int32_t(std::ceil(2.0 * std::max(1.0, double(f.inHeight) / f.outHeight))) + 1,
                  f.maxCode};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, resample_.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, resample_.layout, 0, 1, &resample_.sets[slot], 0,
                            nullptr);
    vkCmdPushConstants(command, resample_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(command, (f.outWidth + 15) / 16, (f.outHeight + 15) / 16, 1);
}

}  // namespace rawr::raw_ingress
