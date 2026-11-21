#include "rawr/raw_ingress/BayerBinner.h"
#include "rawr/raw_ingress/BayerBinKernels.h"
#include "rawr/raw_ingress/BayerBinNoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "bayer_bin3.h"
#include "bayer_lin.h"
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
constexpr std::uint32_t kBin3TileWidth = 32, kBin3TileHeight = 16;  // shaders/bayer_bin3.comp TW, TH
struct Bin3Pc {
    BinPc base;
    std::int32_t ox, oy;
};
static_assert(sizeof(Bin3Pc) == 92, "push constant layout");
struct ResamplePc {
    std::uint32_t iw, ih, ow, oh;
    float sx, sy;
    std::int32_t rx, ry;
    float maxCode;
};
}  // namespace

std::unique_ptr<BayerBinner> BayerBinner::create(const rawr::vk::GpuContext& context) {
    return std::unique_ptr<BayerBinner>(new BayerBinner(context));
}

BayerBinner::BayerBinner(const rawr::vk::GpuContext& context) : context_(context) {
    if (!context_.device || !context_.physicalDevice) throw std::invalid_argument("bayer bin: no device");
    try {
        std::array<VkDescriptorPoolSize, 3> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kSlots},
                                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * kSlots},
                                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSlots}}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 3 * kSlots;
        pi.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pi.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(context_.device, &pi, nullptr, &pool_), "descriptor pool");
        constexpr VkDescriptorType B = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        const VkDescriptorType binTypes[3] = {B, I, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        makeLayout(bin_, binTypes, 3, sizeof(BinPc));
        makeLayout(bin3_, binTypes, 3, sizeof(Bin3Pc));
        makeKernels(kBayerBinPlain, sizeof(kBayerBinPlain), kernels_, kernelsMemory_);
        makeKernels(kBayerBin3, sizeof(kBayerBin3), kernels3_, kernels3Memory_);
        bin_.pipeline = makeComputePipeline(bin_.layout, bayer_lin_spv, bayer_lin_spv_size);
        bin3_.pipeline = makeComputePipeline(bin3_.layout, bayer_bin3_spv, bayer_bin3_spv_size);
        const VkDescriptorType resampleTypes[2] = {I, I};
        makeLayout(resample_, resampleTypes, 2, sizeof(ResamplePc));
        resample_.pipeline = makeComputePipeline(resample_.layout, bayer_resample_spv, bayer_resample_spv_size);
    } catch (...) {
        destroy();
        throw;
    }
}

BayerBinner::~BayerBinner() { destroy(); }

// The weights, once: host-visible, written through a mapping.
void BayerBinner::makeKernels(const float* weights, std::size_t bytes, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    check(vkCreateBuffer(context_.device, &bi, nullptr, &buffer), "kernel buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(context_.device, buffer, &req);
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
    check(vkAllocateMemory(context_.device, &ai, nullptr, &memory), "kernel memory");
    check(vkBindBufferMemory(context_.device, buffer, memory, 0), "kernel bind");
    void* mapped = nullptr;
    check(vkMapMemory(context_.device, memory, 0, bytes, 0, &mapped), "kernel map");
    std::memcpy(mapped, weights, bytes);
    vkUnmapMemory(context_.device, memory);
}



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
    for (auto* b : {&kernels_, &kernels3_}) {
        if (*b) vkDestroyBuffer(d, *b, nullptr);
        *b = VK_NULL_HANDLE;
    }
    for (auto* m : {&kernelsMemory_, &kernels3Memory_}) {
        if (*m) vkFreeMemory(d, *m, nullptr);
        *m = VK_NULL_HANDLE;
    }
    for (auto* p : {&bin_, &bin3_, &resample_}) {
        if (p->pipeline) vkDestroyPipeline(d, p->pipeline, nullptr);
        if (p->layout) vkDestroyPipelineLayout(d, p->layout, nullptr);
        if (p->setLayout) vkDestroyDescriptorSetLayout(d, p->setLayout, nullptr);
        *p = {};
    }
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
}

double BayerBinner::noiseVarianceGain(std::uint32_t outColour, const std::array<float, 3>& gains,
                                      std::uint32_t ratio) {
    return bayerBinNoiseVarianceGain(outColour, gains, ratio);
}

void BayerBinner::bin(VkCommandBuffer command, std::uint32_t slot, const Bin& f) {
    if (slot >= kSlots || !f.source || !f.destination || f.width < 32 || f.height < 32 || f.width % 4 ||
        f.height % 4 || f.rowStrideBytes < f.width * 2 || f.rowStrideBytes % 2 || f.order > 3 ||
        (f.ratio != 2 && f.ratio != 3) || (f.ratio == 3 && (f.width < 48 || f.height < 48)))
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
    const Size out = outputSize(f.width, f.height, f.ratio);
    const std::uint32_t ow = out.width, oh = out.height;
    Pipeline& pipe = f.ratio == 3 ? bin3_ : bin_;
    {
        VkDescriptorBufferInfo source{f.source, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo destination{VK_NULL_HANDLE, f.destination, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo weights{f.ratio == 3 ? kernels3_ : kernels_, 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = pipe.sets[slot];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &source;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &destination;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &weights;
        vkUpdateDescriptorSets(context_.device, 3, writes.data(), 0, nullptr);
    }
    if (f.timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, f.timestamps, 0);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout, 0, 1, &pipe.sets[slot], 0, nullptr);
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
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipeline);
    if (f.ratio == 3) {
        const Bin3Pc pc3{pc, std::int32_t(out.offsetX), std::int32_t(out.offsetY)};
        vkCmdPushConstants(command, pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc3), &pc3);
        vkCmdDispatch(command, (ow + kBin3TileWidth - 1) / kBin3TileWidth, (oh + kBin3TileHeight - 1) / kBin3TileHeight, 1);
    } else {
        vkCmdPushConstants(command, pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(command, (ow + 31) / 32, (oh + 31) / 32, 1);
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
