#include "post_demosaic/LabDefringe.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include "lab_defringe_prepare.h"
#include "lab_defringe_reconstruct.h"
#include "lab_defringe_reduce.h"
#include "rawr/vk/OwnedImage.h"

namespace rawr::post {
namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("Lab defringe Vulkan resource/record failure");
}
struct Push {
    uint32_t width, height;
    float strength, threshold = 13.0f, edgeMode = 1.0f;
    uint32_t pad[3]{};
    float forward[3][4]{}, inverse[3][4]{};
};
static_assert(sizeof(Push) == 128);
Push parameters(uint32_t w, uint32_t h, float strength, const float* camera) {
    if (!camera) throw std::invalid_argument("Lab defringe requires camera-to-sRGB calibration");
    Push p{};
    p.width = w;
    p.height = h;
    p.strength = std::clamp(strength, 0.0f, 1.0f);
    constexpr double srgbToD50[9] = {.4360747, .3850649, .1430804, .2225045, .7168786,
                                     .0606169, .0139322, .0971045, .7141733};
    double m[9]{};
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(camera[i])) throw std::invalid_argument("Nonfinite Lab calibration");
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            for (int k = 0; k < 3; ++k) m[y * 3 + x] += srgbToD50[y * 3 + k] * camera[k * 3 + x];
    double inv[9] = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                     m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                     m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
    double det = m[0] * inv[0] + m[1] * inv[3] + m[2] * inv[6];
    if (!std::isfinite(det) || std::abs(det) < 1e-10) throw std::invalid_argument("Singular Lab calibration");
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) {
            p.forward[y][x] = float(m[y * 3 + x]);
            p.inverse[y][x] = float(inv[y * 3 + x] / det);
        }
    return p;
}
}  // namespace
struct LabDefringe::Impl {
    VkDevice device;
    uint32_t width, height;
    rawr::vk::OwnedImage scores{};
    VkBuffer stats = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t bytes = 0;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2]{};
    VkPipeline pipelines[3]{};
    bool initialized = false;
    Impl(VkPhysicalDevice pd, VkDevice d, uint32_t w, uint32_t h) : device(d), width(w), height(h) {
        try {
            if (!w || !h) throw std::invalid_argument("Lab defringe empty geometry");
            scores = rawr::vk::createOwnedImage(pd, d, (w + 1) / 2, h, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(d, scores.image, &req);
            bytes = req.size;
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = 4 * (1 + VkDeviceSize((w + 15) / 16) * ((h + 15) / 16));
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            check(vkCreateBuffer(d, &bi, nullptr, &stats));
            vkGetBufferMemoryRequirements(d, stats, &req);
            bytes += req.size;
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(pd, &mp);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((req.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    type = i;
                    break;
                }
            if (type == UINT32_MAX) throw std::runtime_error("Lab defringe memory type unavailable");
            VkMemoryAllocateInfo ma{};
            ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ma.allocationSize = req.size;
            ma.memoryTypeIndex = type;
            check(vkAllocateMemory(d, &ma, nullptr, &memory));
            check(vkBindBufferMemory(d, stats, memory, 0));
            VkDescriptorSetLayoutBinding b[4]{};
            for (uint32_t i = 0; i < 4; ++i)
                b[i] = {i, i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo di{};
            di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            di.bindingCount = 4;
            di.pBindings = b;
            check(vkCreateDescriptorSetLayout(d, &di, nullptr, &dsl));
            VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
            VkPipelineLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            li.setLayoutCount = 1;
            li.pSetLayouts = &dsl;
            li.pushConstantRangeCount = 1;
            li.pPushConstantRanges = &range;
            check(vkCreatePipelineLayout(d, &li, nullptr, &layout));
            const unsigned char* shaders[] = {lab_defringe_prepare_spv, lab_defringe_reduce_spv,
                                              lab_defringe_reconstruct_spv};
            const size_t lengths[] = {lab_defringe_prepare_spv_size, lab_defringe_reduce_spv_size,
                                      lab_defringe_reconstruct_spv_size};
            for (int i = 0; i < 3; ++i) {
                VkShaderModuleCreateInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                si.codeSize = lengths[i];
                si.pCode = reinterpret_cast<const uint32_t*>(shaders[i]);
                VkShaderModule module{};
                check(vkCreateShaderModule(d, &si, nullptr, &module));
                VkComputePipelineCreateInfo ci{};
                ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                ci.layout = layout;
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = module;
                ci.stage.pName = "main";
                auto result = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines[i]);
                vkDestroyShaderModule(d, module, nullptr);
                check(result);
            }
            VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
            VkDescriptorPoolCreateInfo pi{};
            pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pi.maxSets = 2;
            pi.poolSizeCount = 2;
            pi.pPoolSizes = ps;
            check(vkCreateDescriptorPool(d, &pi, nullptr, &pool));
            VkDescriptorSetLayout layouts[] = {dsl, dsl};
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 2;
            ai.pSetLayouts = layouts;
            check(vkAllocateDescriptorSets(d, &ai, sets));
        } catch (...) {
            cleanup();
            throw;
        }
    }
    void cleanup() noexcept {
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        for (auto p : pipelines)
            if (p) vkDestroyPipeline(device, p, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(device, dsl, nullptr);
        if (stats) vkDestroyBuffer(device, stats, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        rawr::vk::destroyOwnedImage(device, scores);
    }
    ~Impl() { cleanup(); }
    void record(VkCommandBuffer cmd, VkImageView source, VkImageView target, const float* camera, float strength) {
        auto pc = parameters(width, height, strength, camera);
        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.image = scores.image;
        ib.oldLayout = initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ib.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : 0;
        ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 1, &ib);
        initialized = true;
        VkImageView views[2][3] = {{source, scores.view, source}, {scores.view, target, source}};
        VkDescriptorBufferInfo buf{stats, 0, VK_WHOLE_SIZE};
        for (int set = 0; set < 2; ++set) {
            VkDescriptorImageInfo images[3]{};
            VkWriteDescriptorSet writes[4]{};
            for (int i = 0; i < 4; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = sets[set];
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType =
                    i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (i < 3) {
                    images[i] = {VK_NULL_HANDLE, views[set][i], VK_IMAGE_LAYOUT_GENERAL};
                    writes[i].pImageInfo = &images[i];
                } else
                    writes[i].pBufferInfo = &buf;
            }
            vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
        }
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        for (int i = 0; i < 3; ++i) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[i == 2 ? 1 : 0], 0,
                                    nullptr);
            vkCmdDispatch(cmd, i == 1 ? 1 : (width + 15) / 16, i == 1 ? 1 : (height + 15) / 16, 1);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &mb, 0, nullptr, 0, nullptr);
        }
    }
};
LabDefringe::LabDefringe(VkPhysicalDevice p, VkDevice d, uint32_t w, uint32_t h)
    : impl_(std::make_unique<Impl>(p, d, w, h)) {}
LabDefringe::~LabDefringe() = default;
void LabDefringe::record(VkCommandBuffer cmd, VkImageView source, VkImageView target, const float* camera,
                         float strength) {
    impl_->record(cmd, source, target, camera, strength);
}
uint64_t LabDefringe::allocatedBytes() const noexcept { return impl_->bytes; }
}  // namespace rawr::post
