#include "rawr/raw_ingress/Raw10Unpacker.h"

#include <stdexcept>
#include <string>

#include "raw10_unpack.h"

namespace rawr::raw_ingress {
namespace {
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("raw10 unpack: ") + what + " VkResult=" + std::to_string(r));
}
struct PushConstants {
    std::uint32_t width, height, rowStrideBytes;
};
}  // namespace

std::unique_ptr<Raw10Unpacker> Raw10Unpacker::create(const rawr::vk::GpuContext& context) {
    return std::unique_ptr<Raw10Unpacker>(new Raw10Unpacker(context));
}

Raw10Unpacker::Raw10Unpacker(const rawr::vk::GpuContext& context) : context_(context) {
    if (!context_.device) throw std::invalid_argument("raw10 unpack: no device");
    try {
        const VkDevice d = context_.device;
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
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
        mi.codeSize = raw10_unpack_spv_size;
        mi.pCode = reinterpret_cast<const std::uint32_t*>(raw10_unpack_spv);
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

        std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots},
                                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSlots}}};
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

Raw10Unpacker::~Raw10Unpacker() { destroy(); }

void Raw10Unpacker::destroy() noexcept {
    const VkDevice d = context_.device;
    if (!d) return;
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(d, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(d, pipelineLayout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    pool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void Raw10Unpacker::record(VkCommandBuffer command, std::uint32_t slot, VkBuffer source, VkImageView destination,
                           std::uint32_t width, std::uint32_t height, std::uint32_t rowStrideBytes) {
    if (slot >= kSlots || !source || !destination || width == 0 || width % 4u != 0 || height == 0)
        throw std::invalid_argument("raw10 unpack: invalid record arguments");
    if (rowStrideBytes < width / 4u * 5u) throw std::invalid_argument("raw10 unpack: row stride below RAW10 width");
    // Rewriting a set referenced by a pending command buffer is invalid, so
    // only touch it when the binding actually changes.
    Binding& bound = bound_[slot];
    if (bound.source != source || bound.destination != destination) {
        VkDescriptorBufferInfo bi{source, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, destination, VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = sets_[slot];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &bi;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pBufferInfo = nullptr;
        writes[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(context_.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        bound = {source, destination};
    }
    const PushConstants pc{width, height, rowStrideBytes};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[slot], 0, nullptr);
    vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(command, (width + 15u) / 16u, (height + 15u) / 16u, 1);
}

}  // namespace rawr::raw_ingress
