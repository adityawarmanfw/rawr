#include <rawr/raw_gpu_pipeline/RextureFrameScorer.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "mf_rx_frame_score_prod.h"

namespace rawr::raw_gpu_pipeline {
namespace {
void ck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("rexture scorer: ") + what);
}
template <typename T>
T init(VkStructureType type) {
    T v{};
    v.sType = type;
    return v;
}
struct Pc {
    float black[4];
    float white;
    std::int32_t w, h, cfa;
    float alpha, beta;
    std::int32_t frameSlot, workgroups;
};
}  // namespace

void RextureFrameScorer::initialize(VkPhysicalDevice physical, VkDevice device, std::uint32_t qf, Submit submit) {
    reset();
    if (!physical || !device || !submit) throw std::invalid_argument("rexture scorer: invalid initialize");
    physical_ = physical;
    device_ = device;
    submit_ = std::move(submit);
    try {
        const VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        auto dc = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        dc.bindingCount = 2;
        dc.pBindings = bindings;
        ck(vkCreateDescriptorSetLayout(device, &dc, nullptr, &dsl_), "descriptor layout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Pc)};
        auto lc = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        lc.setLayoutCount = 1;
        lc.pSetLayouts = &dsl_;
        lc.pushConstantRangeCount = 1;
        lc.pPushConstantRanges = &pr;
        ck(vkCreatePipelineLayout(device, &lc, nullptr, &layout_), "pipeline layout");
        auto mi = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        mi.codeSize = mf_rx_frame_score_prod_spv_size;
        mi.pCode = reinterpret_cast<const std::uint32_t*>(mf_rx_frame_score_prod_spv);
        VkShaderModule module = VK_NULL_HANDLE;
        ck(vkCreateShaderModule(device, &mi, nullptr, &module), "shader module");
        auto pc = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        pc.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        pc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.stage.module = module;
        pc.stage.pName = "main";
        pc.layout = layout_;
        const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline_);
        vkDestroyShaderModule(device, module, nullptr);
        ck(r, "compute pipeline");
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxFrames},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxFrames}};
        auto dp = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        dp.maxSets = kMaxFrames;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = sizes;
        ck(vkCreateDescriptorPool(device, &dp, nullptr, &pool_), "descriptor pool");
        auto cp = init<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = qf;
        ck(vkCreateCommandPool(device, &cp, nullptr, &commandPool_), "command pool");
        auto ai = init<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        ai.commandPool = commandPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        ck(vkAllocateCommandBuffers(device, &ai, &command_), "command buffer");
        auto fi = init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        ck(vkCreateFence(device, &fi, nullptr, &fence_), "fence");
    } catch (...) {
        reset();
        throw;
    }
}

std::vector<RextureFrameScorer::Score> RextureFrameScorer::score(const std::vector<BurstFrame>& frames, std::uint32_t width,
                                                                 std::uint32_t height, std::uint32_t cfa, float noiseAlpha,
                                                                 float noiseBeta) {
    if (!ready() || frames.empty() || frames.size() > kMaxFrames || width < 8 || height < 8)
        throw std::invalid_argument("rexture scorer: invalid request");
    const std::uint32_t gx = ((width / 4u) + 15u) / 16u, gy = ((height / 4u) + 15u) / 16u, groups = gx * gy;
    const VkDeviceSize bytes = VkDeviceSize(frames.size()) * groups * 4u * sizeof(float);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    struct Cleanup {
        VkDevice d;
        VkBuffer& b;
        VkDeviceMemory& m;
        void*& p;
        ~Cleanup() {
            if (p) vkUnmapMemory(d, m);
            if (b) vkDestroyBuffer(d, b, nullptr);
            if (m) vkFreeMemory(d, m, nullptr);
        }
    } cleanup{device_, buffer, memory, mapped};
    auto bi = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ck(vkCreateBuffer(device_, &bi, nullptr, &buffer), "buffer");
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device_, buffer, &mr);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t type = UINT32_MAX;
    for (std::uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((mr.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == UINT32_MAX) throw std::runtime_error("rexture scorer: no host-visible memory");
    auto ma = init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = type;
    ck(vkAllocateMemory(device_, &ma, nullptr, &memory), "memory");
    ck(vkBindBufferMemory(device_, buffer, memory, 0), "bind");
    ck(vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map");

    ck(vkResetDescriptorPool(device_, pool_, 0), "reset pool");
    ck(vkResetCommandBuffer(command_, 0), "reset command");
    auto cb = init<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ck(vkBeginCommandBuffer(command_, &cb), "begin");
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    for (std::uint32_t i = 0; i < frames.size(); ++i) {
        VkDescriptorSet set = VK_NULL_HANDLE;
        auto sa = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        sa.descriptorPool = pool_;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &dsl_;
        ck(vkAllocateDescriptorSets(device_, &sa, &set), "descriptor set");
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, frames[i].raw.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo di{buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2] = {init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET),
                                     init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)};
        w[0].dstSet = set;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[0].pImageInfo = &ii;
        w[1].dstSet = set;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &di;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
        const auto& norm = frames[i].parameters.normalization;
        Pc pc{};
        std::copy(norm.blackByPhase.begin(), norm.blackByPhase.end(), pc.black);
        pc.white = norm.whiteLevel;
        pc.w = std::int32_t(width);
        pc.h = std::int32_t(height);
        pc.cfa = std::int32_t(cfa);
        pc.alpha = noiseAlpha;
        pc.beta = noiseBeta;
        pc.frameSlot = std::int32_t(i);
        pc.workgroups = std::int32_t(groups);
        vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(command_, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(command_, gx, gy, 1);
    }
    auto toHost = init<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0,
                         nullptr, 0, nullptr);
    ck(vkEndCommandBuffer(command_), "end");
    auto si = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command_;
    ck(vkResetFences(device_, 1, &fence_), "reset fence");
    submit_(si, fence_);
    ck(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "wait");

    std::vector<Score> out(frames.size());
    const auto* p = static_cast<const float*>(mapped);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        double sumOk = 0, okCount = 0, clipCount = 0, count = 0;
        for (std::uint32_t g = 0; g < groups; ++g) {
            const float* v = p + (i * groups + g) * 4u;
            sumOk += v[0];
            okCount += v[1];
            clipCount += v[2];
            count += v[3];
        }
        out[i].sharp = okCount > 0 ? std::max(sumOk / okCount, 0.0) : 0.0;
        out[i].sat = count > 0 ? clipCount / count : 0.0;
    }
    return out;
}

void RextureFrameScorer::reset() noexcept {
    if (device_) {
        vkDeviceWaitIdle(device_);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (dsl_) vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
    }
    fence_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    dsl_ = VK_NULL_HANDLE;
    command_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
}

}  // namespace rawr::raw_gpu_pipeline
