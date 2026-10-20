#include "rawr/raw_ingress/RawRemosaicker.h"

#include <stdexcept>
#include <string>

#include "raw_remosaic.h"

namespace rawr::raw_ingress {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("raw remosaic: ") + what + " VkResult=" + std::to_string(r));
}
struct PushConstants {
    std::uint32_t width, height, rowStrideBytes, block, order, raw10, pass, blocksX, blocksY, parityR, parityB;
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

std::unique_ptr<RawRemosaicker> RawRemosaicker::create(const rawr::vk::GpuContext& context) {
    return std::unique_ptr<RawRemosaicker>(new RawRemosaicker(context));
}

RawRemosaicker::RawRemosaicker(const rawr::vk::GpuContext& context) : context_(context) {
    if (!context_.device || !context_.physicalDevice) throw std::invalid_argument("raw remosaic: no device");
    try {
        const VkDevice d = context_.device;
        std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
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
        const VkResult created = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_);
        vkDestroyShaderModule(d, module, nullptr);
        check(created, "pipeline");

        std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kSlots},
                                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * kSlots}}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = kSlots;
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
    if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(d, pipelineLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    pool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void RawRemosaicker::releaseScratch() noexcept {
    const VkDevice d = context_.device;
    rawr::vk::destroyOwnedImage(d, scratch_.raw);
    rawr::vk::destroyOwnedImage(d, scratch_.green);
    if (scratch_.diff) vkDestroyBuffer(d, scratch_.diff, nullptr);
    if (scratch_.diffMemory) vkFreeMemory(d, scratch_.diffMemory, nullptr);
    scratch_ = {};
}

void RawRemosaicker::ensureScratch(std::uint32_t width, std::uint32_t height, std::uint32_t block) {
    if (scratch_.raw.image && scratch_.width == width && scratch_.height == height && scratch_.block == block) return;
    releaseScratch();
    const VkDevice d = context_.device;
    const auto make = [&](VkDeviceSize bytes, VkBuffer& buffer, VkDeviceMemory& memory) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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
        make(VkDeviceSize(bw) * bh * 4u, scratch_.diff, scratch_.diffMemory);
    } catch (...) {
        releaseScratch();
        throw;
    }
    scratch_.width = width;
    scratch_.height = height;
    scratch_.block = block;
}

void RawRemosaicker::record(VkCommandBuffer command, std::uint32_t slot, const Frame& f) {
    if (slot >= kSlots || !f.source || !f.destination || f.width < 2u * f.block || f.height < 2u * f.block ||
        f.width % 2u != 0 || (f.block != 2u && f.block != 4u) || f.order > 3u || (f.raw10 && f.width % 4u != 0) ||
        (!f.raw10 && f.rowStrideBytes % 2u != 0))
        throw std::invalid_argument("raw remosaic: invalid record arguments");
    if (f.rowStrideBytes < (f.raw10 ? f.width / 4u * 5u : f.width * 2u))
        throw std::invalid_argument("raw remosaic: row stride below width");
    ensureScratch(f.width, f.height, f.block);
    Binding& bound = bound_[slot];
    if (bound.source != f.source || bound.destination != f.destination || bound.raw != scratch_.raw.view) {
        VkDescriptorBufferInfo source{f.source, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo image{VK_NULL_HANDLE, f.destination, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo raw{VK_NULL_HANDLE, scratch_.raw.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo green{VK_NULL_HANDLE, scratch_.green.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo diff{scratch_.diff, 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 8> writes{};
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
        vkUpdateDescriptorSets(context_.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        bound = {f.source, f.destination, scratch_.raw.view};
    }
    const std::uint32_t bw = (f.width + f.block - 1) / f.block, bh = (f.height + f.block - 1) / f.block;
    // Block parity ((y & 1) << 1 | (x & 1)) of red and blue for this order (0=RGGB 1=GRBG 2=GBRG 3=BGGR).
    static constexpr std::uint32_t kParityR[4] = {0u, 1u, 2u, 3u}, kParityB[4] = {3u, 2u, 1u, 0u};
    PushConstants pc{f.width, f.height, f.rowStrideBytes, f.block, f.order, f.raw10 ? 1u : 0u, 0u, bw, bh,
                     kParityR[f.order], kParityB[f.order]};
    const auto stamp = [&](std::uint32_t index) {
        if (f.timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, f.timestamps, index);
    };
    stamp(0);
    // The scratch is shared across slots: the previous frame's passes must be
    // done with it before this frame writes it. Its contents never carry over,
    // so each frame starts from UNDEFINED.
    VkImageMemoryBarrier toWrite[2]{};
    for (int i = 0; i < 2; ++i) {
        toWrite[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toWrite[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toWrite[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        toWrite[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toWrite[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toWrite[i].srcQueueFamilyIndex = toWrite[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toWrite[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    toWrite[0].image = scratch_.raw.image;
    toWrite[1].image = scratch_.green.image;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, toWrite);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[slot], 0, nullptr);
    const auto pass = [&](std::uint32_t index, std::uint32_t gx, std::uint32_t gy) {
        pc.pass = index;
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(command, (gx + 15u) / 16u, (gy + 15u) / 16u, 1);
    };
    // Raw rows into the cached image: a transfer for RAW16, an unpack for RAW10.
    if (f.raw10) {
        pass(3u, f.width, f.height);
        memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
        VkBufferImageCopy region{};
        region.bufferRowLength = f.rowStrideBytes / 2u;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {f.width, f.height, 1};
        vkCmdCopyBufferToImage(command, f.source, scratch_.raw.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                             0, nullptr, 0, nullptr);
    }
    stamp(1);
    // One invocation per 2x2 pixel group (see the shader).
    pass(0u, (f.width + 1u) / 2u, (f.height + 1u) / 2u);  // green + block colour difference
    memoryBarrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    stamp(2);
    pass(2u, (f.width + 1u) / 2u, (f.height + 1u) / 2u);  // Bayer out
    stamp(3);
}

}  // namespace rawr::raw_ingress
