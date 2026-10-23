#include "develop/render/DcpRenderStage.h"

#include <dcp.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "color/ColorMath.h"
#include "color/DcpRenderData.h"
#include "vulkan/HostMemory.h"
namespace rawrcam::develop::rendered {
namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("DCP GPU setup failed: " + std::to_string(r));
}
struct Push {
    float toProfile[12];
    float toCamera[12];
    float weight;
};
}  // namespace
DcpRenderStage::DcpRenderStage(VkPhysicalDevice physical, VkDevice device)
    : physical_(physical), device_(device), objects_(device), storage_(device) {
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    di.bindingCount = 2;
    di.pBindings = bindings;
    VkDescriptorSetLayout dsl;
    check(vkCreateDescriptorSetLayout(device_, &di, nullptr, &dsl));
    objects_.adoptDescriptorSetLayout(dsl);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &dsl;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device_, &li, nullptr, &layout_));
    objects_.adoptPipelineLayout(layout_);
    VkShaderModuleCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = dcp_spv_size;
    si.pCode = reinterpret_cast<const uint32_t*>(dcp_spv);
    VkShaderModule shader;
    check(vkCreateShaderModule(device_, &si, nullptr, &shader));
    objects_.adoptShaderModule(shader);
    VkComputePipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = shader;
    pi.stage.pName = "main";
    pi.layout = layout_;
    check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_));
    objects_.adoptPipeline(pipeline_);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool));
    objects_.adoptDescriptorPool(pool);
    VkDescriptorSetLayout layouts[] = {dsl, dsl};
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    check(vkAllocateDescriptorSets(device_, &ai, sets_.data()));
}
void DcpRenderStage::record(VkCommandBuffer command, const color::DcpProfilePtr& profile,
                            const std::array<float, 9>& cameraToSrgb, float weight1, uint32_t width, uint32_t height,
                            VkImageView first, VkImageView second) {
    if (!profile || !first) return;
    if (profile->hueSatMap1.empty() && profile->lookTable.empty() && profile->toneCurve.empty() &&
        profile->baselineExposureOffset.value_or(0) == 0)
        return;
    using namespace color::math;
    // DCP tables are defined in linear ProPhoto RGB (D50).
    constexpr Matrix3 proPhotoToD50{.7976749f, .1351917f, .0313534f, .2880402f, .7118741f, .0000857f, 0, 0, .82521f};
    const auto proPhotoToSrgb = multiply(kXyzD65ToLinearSrgb, multiply(kD50ToD65Bradford, proPhotoToD50));
    const auto srgbToProfile = inverse(proPhotoToSrgb);
    const auto toProfile = multiply(*srgbToProfile, cameraToSrgb);
    const auto toCamera = inverse(toProfile);
    if (!toCamera) throw std::runtime_error("DCP render matrix is singular");
    if (profile_ != profile) {
        const auto data = color::dcpRenderData(*profile);
        rawr::vk::DeviceObjects storage(device_);
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = data.size() * sizeof(float);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer;
        check(vkCreateBuffer(device_, &bi, nullptr, &buffer));
        // Adopt memory before the buffer so destruction frees the buffer first.
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = req.size;
        try {
            mi.memoryTypeIndex = vulkan::findCoherentHostMemoryType(physical_, req.memoryTypeBits);
        } catch (...) {
            vkDestroyBuffer(device_, buffer, nullptr);
            throw;
        }
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const auto allocated = vkAllocateMemory(device_, &mi, nullptr, &memory);
        if (allocated != VK_SUCCESS) {
            vkDestroyBuffer(device_, buffer, nullptr);
            check(allocated);
        }
        storage.adoptMemory(memory);
        storage.adoptBuffer(buffer);
        check(vkBindBufferMemory(device_, buffer, memory, 0));
        void* mapped = nullptr;
        check(vkMapMemory(device_, memory, 0, bi.size, 0, &mapped));
        std::memcpy(mapped, data.data(), bi.size);
        vkUnmapMemory(device_, memory);
        storage_ = std::move(storage);
        buffer_ = buffer;
        profile_ = profile;
    }
    Push push{};
    for (size_t row = 0; row < 3; ++row)
        for (size_t col = 0; col < 3; ++col) {
            push.toProfile[row * 4 + col] = toProfile[row * 3 + col];
            push.toCamera[row * 4 + col] = (*toCamera)[row * 3 + col];
        }
    push.weight = std::clamp(weight1, 0.0f, 1.0f);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
    const VkImageView views[] = {first, second};
    for (size_t i = 0; i < 2; ++i) {
        if (!views[i] || (i == 1 && second == first)) continue;
        VkDescriptorImageInfo image{VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo buffer{buffer_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2]{};
        for (auto& w : writes) {
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = sets_[i];
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &image;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets_[i], 0, nullptr);
        vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(command, (width + 15) / 16, (height + 15) / 16, 1);
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
}
}  // namespace rawrcam::develop::rendered
