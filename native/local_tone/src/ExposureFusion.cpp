#include "local_tone/ExposureFusion.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fus_apply.h"
#include "fus_base.h"
#include "fus_blend.h"
#include "fus_down.h"
#include "fus_lap.h"
#include "rawr/vk/DeviceObjects.h"
#include "rawr/vk/OwnedImage.h"

namespace rawr::local_tone {
namespace {

// Mirrors FusionPush in fus_common.glsl.
struct Push {
    std::uint32_t size[4];
    std::uint32_t level[4];
    float to709[3][4];
    float shadows;
    float highlights;
    float sigmaSq;
    std::uint32_t block;
    std::uint32_t boost;
    std::uint32_t pad[3];
};
static_assert(sizeof(Push) == 112, "exposure fusion push layout");

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("exposure_fusion: ") + what + " (" + std::to_string(result) + ")");
}

// AP1 (ACEScg) -> linear Rec.709, row-major.
constexpr float kAp1To709[3][3] = {{1.70505f, -0.62179f, -0.08326f},
                                   {-0.13026f, 1.14080f, -0.01055f},
                                   {-0.02400f, -0.12897f, 1.15297f}};

constexpr std::uint32_t kLevels = kFusionMaxDownLevels + 1;
// Per slot: base, downs, blend, laplacians, two applies.
constexpr std::uint32_t kSetsPerSlot = 1u + kFusionMaxDownLevels + 1u + kFusionMaxDownLevels + 2u;

}  // namespace

struct ExposureFusionProcessor::Impl {
    rawr::vk::GpuContext gpu;
    rawr::vk::DeviceObjects objects;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline base = VK_NULL_HANDLE, down = VK_NULL_HANDLE, blend = VK_NULL_HANDLE, lap = VK_NULL_HANDLE,
               apply = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    struct Level {
        std::uint32_t w = 0, h = 0;
        rawr::vk::OwnedImage lightness, weights, assembled;
    };
    struct Slot {
        rawr::vk::DeviceObjects images;
        std::uint32_t fullW = 0, fullH = 0, downLevels = 0;
        Level levels[kLevels];
        VkDescriptorSet sets[kSetsPerSlot]{};
        bool initialized = false;
    };
    std::vector<Slot> slots;

    Impl(const rawr::vk::GpuContext& context, std::uint32_t slotCount) : gpu(context), objects(context.device) {
        if (!gpu.device || !gpu.physicalDevice) throw std::invalid_argument("exposure_fusion: missing device");
        slotCount = std::clamp<std::uint32_t>(slotCount, 1u, kMaxSlots);
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (std::uint32_t b = 0; b < 6; ++b) {
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = b;
            binding.descriptorType =
                b < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings.push_back(binding);
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = static_cast<std::uint32_t>(bindings.size());
        lci.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(gpu.device, &lci, nullptr, &setLayout), "set layout");
        objects.adoptDescriptorSetLayout(setLayout);
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pci.setLayoutCount = 1;
        pci.pSetLayouts = &setLayout;
        pci.pushConstantRangeCount = 1;
        pci.pPushConstantRanges = &range;
        check(vkCreatePipelineLayout(gpu.device, &pci, nullptr, &layout), "pipeline layout");
        objects.adoptPipelineLayout(layout);
        base = pipeline(fus_base_spv, fus_base_spv_size);
        down = pipeline(fus_down_spv, fus_down_spv_size);
        blend = pipeline(fus_blend_spv, fus_blend_spv_size);
        lap = pipeline(fus_lap_spv, fus_lap_spv_size);
        apply = pipeline(fus_apply_spv, fus_apply_spv_size);

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        check(vkCreateSampler(gpu.device, &si, nullptr, &sampler), "sampler");
        objects.adoptSampler(sampler);

        const std::uint32_t sets = slotCount * kSetsPerSlot;
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets * 4u},
                                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 2u}};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = sets;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &pool), "descriptor pool");
        objects.adoptDescriptorPool(pool);
        slots.resize(slotCount);
        for (auto& slot : slots) {
            std::vector<VkDescriptorSetLayout> layouts(kSetsPerSlot, setLayout);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pool;
            ai.descriptorSetCount = kSetsPerSlot;
            ai.pSetLayouts = layouts.data();
            check(vkAllocateDescriptorSets(gpu.device, &ai, slot.sets), "descriptor sets");
        }
    }

    VkPipeline pipeline(const unsigned char* code, std::size_t size) {
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = size;
        mi.pCode = reinterpret_cast<const std::uint32_t*>(code);
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(gpu.device, &mi, nullptr, &module), "shader module");
        objects.adoptShaderModule(module);
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        VkPipeline out = VK_NULL_HANDLE;
        check(vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &ci, nullptr, &out), "pipeline");
        return objects.adoptPipeline(out);
    }

    void ensureImages(Slot& slot, std::uint32_t fullW, std::uint32_t fullH, std::uint32_t downLevels) {
        if (slot.fullW == fullW && slot.fullH == fullH && slot.downLevels == downLevels) return;
        // A size change replaces the slot's images; the caller only reuses a
        // slot once its previous command buffer has completed.
        rawr::vk::DeviceObjects fresh(gpu.device);
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const std::uint32_t block = 1u << kFusionBaseMip;
        std::uint32_t w = (fullW + block - 1) / block, h = (fullH + block - 1) / block;
        for (std::uint32_t i = 0; i <= downLevels; ++i) {
            if (i > 0) {
                w = std::max(1u, w / 2);
                h = std::max(1u, h / 2);
            }
            auto& level = slot.levels[i];
            level.w = w;
            level.h = h;
            const auto make = [&] {
                return fresh.adoptOwnedImage(rawr::vk::createOwnedImage(gpu.physicalDevice, gpu.device, w, h,
                                                                        VK_FORMAT_R16G16B16A16_SFLOAT, usage));
            };
            level.lightness = make();
            level.weights = make();
            level.assembled = make();
        }
        slot.images = std::move(fresh);
        slot.fullW = fullW;
        slot.fullH = fullH;
        slot.downLevels = downLevels;
        slot.initialized = false;
    }

    // Bindings 0-3 storage images, 4-5 sampled; null views are left unwritten.
    void write(VkDescriptorSet set, std::initializer_list<std::pair<std::uint32_t, VkImageView>> items) {
        VkDescriptorImageInfo infos[6]{};
        VkWriteDescriptorSet writes[6]{};
        std::uint32_t n = 0;
        for (const auto& [binding, view] : items) {
            const bool sampled = binding >= 4;
            infos[n] = {sampled ? sampler : VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            writes[n] = {};
            writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[n].dstSet = set;
            writes[n].dstBinding = binding;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType =
                sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[n].pImageInfo = &infos[n];
            ++n;
        }
        vkUpdateDescriptorSets(gpu.device, n, writes, 0, nullptr);
    }
};

ExposureFusionProcessor::ExposureFusionProcessor(const rawr::vk::GpuContext& gpu, std::uint32_t slots)
    : impl_(std::make_unique<Impl>(gpu, slots)) {}
ExposureFusionProcessor::~ExposureFusionProcessor() = default;

bool ExposureFusionProcessor::record(const FusionRecordInfo& info) {
    if (!validParams(info.params) || !info.commandBuffer || !info.measure.view || info.width == 0 ||
        info.height == 0 || info.targetCount < 1 || info.targetCount > 2 || !(info.renderGain > 0.0f) ||
        !std::isfinite(info.renderGain))
        return false;
    for (std::uint32_t t = 0; t < info.targetCount; ++t)
        if (!info.targets[t].image || !info.targets[t].view) return false;
    auto& d = *impl_;
    auto& slot = d.slots[std::min<std::size_t>(info.slot, d.slots.size() - 1)];
    const std::uint32_t downLevels = info.params.coarsestMip - kFusionBaseMip;
    d.ensureImages(slot, info.width, info.height, downLevels);
    const VkCommandBuffer cmd = info.commandBuffer;
    const auto& L = slot.levels;

    // Descriptor sets: 0 base, 1..n downs, n+1 blend, n+2..2n+1 laplacians, 2n+2.. applies.
    const std::uint32_t n = downLevels;
    d.write(slot.sets[0], {{0, info.measure.view}, {1, L[0].lightness.view}, {2, L[0].weights.view}});
    for (std::uint32_t i = 0; i < n; ++i)
        d.write(slot.sets[1 + i], {{0, L[i].lightness.view},
                                   {1, L[i].weights.view},
                                   {2, L[i + 1].lightness.view},
                                   {3, L[i + 1].weights.view}});
    d.write(slot.sets[n + 1], {{0, L[n].lightness.view}, {1, L[n].weights.view}, {2, L[n].assembled.view}});
    for (std::uint32_t i = n; i > 0; --i)  // level i -> i-1
        d.write(slot.sets[n + 1 + (n - i + 1)], {{0, L[i - 1].lightness.view},
                                                 {1, L[i - 1].weights.view},
                                                 {2, L[i - 1].assembled.view},
                                                 {4, L[i].lightness.view},
                                                 {5, L[i].assembled.view}});
    for (std::uint32_t t = 0; t < info.targetCount; ++t)
        d.write(slot.sets[2 * n + 2 + t],
                {{0, info.targets[t].view}, {4, L[0].lightness.view}, {5, L[0].assembled.view}});

    {
        std::vector<VkImageMemoryBarrier> barriers;
        for (std::uint32_t i = 0; i <= n; ++i)
            for (const VkImage image : {L[i].lightness.image, L[i].weights.image, L[i].assembled.image}) {
                VkImageMemoryBarrier b{};
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                b.oldLayout = slot.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barriers.push_back(b);
            }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, static_cast<std::uint32_t>(barriers.size()), barriers.data());
        slot.initialized = true;
    }

    Push push{};
    push.size[0] = info.width;
    push.size[1] = info.height;
    push.size[2] = L[0].w;
    push.size[3] = L[0].h;
    const float gain = info.renderGain * info.params.exposure;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) {
            float v = 0.0f;
            for (int k = 0; k < 3; ++k) v += kAp1To709[r][k] * info.cameraToWorking[std::size_t(c * 3 + k)];
            push.to709[c][r] = v * gain;
        }
    push.shadows = std::exp2(info.params.shadowsEv);
    push.highlights = std::exp2(-info.params.highlightsEv);
    push.sigmaSq = info.params.sigma * info.params.sigma;
    push.block = 1u << kFusionBaseMip;
    push.boost = info.params.boostLocalContrast ? 1u : 0u;

    const auto memory = [&] {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                             0, nullptr, 0, nullptr);
    };
    const auto dispatch = [&](VkPipeline pipeline, VkDescriptorSet set, std::uint32_t w, std::uint32_t h,
                              std::uint32_t group) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (w + group - 1) / group, (h + group - 1) / group, 1);
    };
    const auto setLevel = [&](std::uint32_t sw, std::uint32_t sh, std::uint32_t dw, std::uint32_t dh) {
        push.level[0] = sw;
        push.level[1] = sh;
        push.level[2] = dw;
        push.level[3] = dh;
    };

    dispatch(d.base, slot.sets[0], L[0].w, L[0].h, 8);
    memory();
    for (std::uint32_t i = 0; i < n; ++i) {
        setLevel(L[i].w, L[i].h, L[i + 1].w, L[i + 1].h);
        dispatch(d.down, slot.sets[1 + i], L[i + 1].w, L[i + 1].h, 8);
        memory();
    }
    setLevel(L[n].w, L[n].h, L[n].w, L[n].h);
    dispatch(d.blend, slot.sets[n + 1], L[n].w, L[n].h, 8);
    memory();
    for (std::uint32_t i = n; i > 0; --i) {
        setLevel(L[i].w, L[i].h, L[i - 1].w, L[i - 1].h);
        dispatch(d.lap, slot.sets[n + 1 + (n - i + 1)], L[i - 1].w, L[i - 1].h, 8);
        memory();
    }
    for (std::uint32_t t = 0; t < info.targetCount; ++t)
        dispatch(d.apply, slot.sets[2 * n + 2 + t], info.width, info.height, 16);

    VkImageMemoryBarrier done[2]{};
    for (std::uint32_t t = 0; t < info.targetCount; ++t) {
        done[t] = {};
        done[t].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        done[t].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        done[t].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        done[t].oldLayout = done[t].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        done[t].srcQueueFamilyIndex = done[t].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        done[t].image = info.targets[t].image;
        done[t].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, info.targetCount, done);
    return true;
}

}  // namespace rawr::local_tone
