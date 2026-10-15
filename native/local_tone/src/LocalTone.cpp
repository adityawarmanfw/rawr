#include "local_tone/LocalTone.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ltm_apply.h"
#include "ltm_box.h"
#include "ltm_coeff.h"
#include "ltm_lift.h"
#include "ltm_luma.h"
#include "rawr/vk/DeviceObjects.h"
#include "rawr/vk/OwnedImage.h"

namespace rawr::local_tone {
namespace {

// Mirrors LocalTonePush in ltm_common.glsl.
struct Push {
    std::uint32_t size[4];
    float toWorking[3][4];
    float logGain;
    float strength;
    float detail;
    float eps;
    std::uint32_t block;
    std::int32_t radius;
    std::uint32_t vertical;
    std::uint32_t pad;
};
static_assert(sizeof(Push) == 96, "local tone push layout");

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("local_tone: ") + what + " (" + std::to_string(result) + ")");
}

enum Pass : std::uint32_t { Luma, BoxH1, BoxV1, Coeff, BoxH2, BoxV2, Lift, Apply0, Apply1, PassCount };

}  // namespace

struct LocalToneProcessor::Impl {
    rawr::vk::GpuContext gpu;
    rawr::vk::DeviceObjects objects;
    VkDescriptorSetLayout twoImages = VK_NULL_HANDLE, threeImages = VK_NULL_HANDLE, apply = VK_NULL_HANDLE;
    VkPipelineLayout twoLayout = VK_NULL_HANDLE, threeLayout = VK_NULL_HANDLE, applyLayout = VK_NULL_HANDLE;
    VkPipeline luma = VK_NULL_HANDLE, box = VK_NULL_HANDLE, coeff = VK_NULL_HANDLE, lift = VK_NULL_HANDLE,
               applyPipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    struct Slot {
        rawr::vk::DeviceObjects images;
        std::uint32_t lowW = 0, lowH = 0;
        rawr::vk::OwnedImage lumaImage, temp, means, coeffs, map;
        VkDescriptorSet sets[PassCount]{};
        bool initialized = false;  // low-res images already in GENERAL layout
    };
    std::vector<Slot> slots;

    explicit Impl(const rawr::vk::GpuContext& context, std::uint32_t slotCount) : gpu(context), objects(context.device) {
        if (!gpu.device || !gpu.physicalDevice) throw std::invalid_argument("local_tone: missing device");
        slotCount = std::clamp<std::uint32_t>(slotCount, 1u, kMaxSlots);
        twoImages = layout({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        threeImages = layout({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        apply = layout({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
        twoLayout = pipelineLayout(twoImages);
        threeLayout = pipelineLayout(threeImages);
        applyLayout = pipelineLayout(apply);
        luma = pipeline(twoLayout, ltm_luma_spv, ltm_luma_spv_size);
        box = pipeline(twoLayout, ltm_box_spv, ltm_box_spv_size);
        coeff = pipeline(twoLayout, ltm_coeff_spv, ltm_coeff_spv_size);
        lift = pipeline(threeLayout, ltm_lift_spv, ltm_lift_spv_size);
        applyPipeline = pipeline(applyLayout, ltm_apply_spv, ltm_apply_spv_size);

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        check(vkCreateSampler(gpu.device, &si, nullptr, &sampler), "sampler");
        objects.adoptSampler(sampler);

        // Per slot: 6 two-image sets, 1 three-image set, 2 apply sets.
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, slotCount * (6u * 2u + 3u + 2u)},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slotCount * 2u}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = slotCount * PassCount;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(gpu.device, &pi, nullptr, &pool), "descriptor pool");
        objects.adoptDescriptorPool(pool);
        slots.resize(slotCount);
        for (auto& slot : slots) {
            VkDescriptorSetLayout layouts[PassCount] = {twoImages, twoImages, twoImages, twoImages, twoImages,
                                                        twoImages, threeImages, apply,     apply};
            VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pool;
            ai.descriptorSetCount = PassCount;
            ai.pSetLayouts = layouts;
            check(vkAllocateDescriptorSets(gpu.device, &ai, slot.sets), "descriptor sets");
        }
    }

    VkDescriptorSetLayout layout(std::initializer_list<VkDescriptorType> types) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::uint32_t index = 0;
        for (const auto type : types) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = index++;
            b.descriptorType = type;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings.push_back(b);
        }
        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = static_cast<std::uint32_t>(bindings.size());
        ci.pBindings = bindings.data();
        VkDescriptorSetLayout out = VK_NULL_HANDLE;
        check(vkCreateDescriptorSetLayout(gpu.device, &ci, nullptr, &out), "set layout");
        return objects.adoptDescriptorSetLayout(out);
    }
    VkPipelineLayout pipelineLayout(VkDescriptorSetLayout set) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &set;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &range;
        VkPipelineLayout out = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(gpu.device, &ci, nullptr, &out), "pipeline layout");
        return objects.adoptPipelineLayout(out);
    }
    VkPipeline pipeline(VkPipelineLayout layout, const unsigned char* code, std::size_t size) {
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

    void ensureImages(Slot& slot, std::uint32_t lowW, std::uint32_t lowH) {
        if (slot.lowW == lowW && slot.lowH == lowH && slot.map.image) return;
        // A size change replaces the slot's images; the caller only reuses a
        // slot once its previous command buffer has completed.
        rawr::vk::DeviceObjects fresh(gpu.device);
        const auto make = [&](VkFormat format, VkImageUsageFlags usage) {
            return fresh.adoptOwnedImage(
                rawr::vk::createOwnedImage(gpu.physicalDevice, gpu.device, lowW, lowH, format, usage));
        };
        const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT;
        const auto lumaImage = make(VK_FORMAT_R32G32_SFLOAT, storage);
        const auto temp = make(VK_FORMAT_R32G32_SFLOAT, storage);
        const auto means = make(VK_FORMAT_R32G32_SFLOAT, storage);
        const auto coeffs = make(VK_FORMAT_R32G32_SFLOAT, storage);
        const auto map = make(VK_FORMAT_R16G16_SFLOAT, storage | VK_IMAGE_USAGE_SAMPLED_BIT);
        slot.images = std::move(fresh);
        slot.lowW = lowW;
        slot.lowH = lowH;
        slot.lumaImage = lumaImage;
        slot.temp = temp;
        slot.means = means;
        slot.coeffs = coeffs;
        slot.map = map;
        slot.initialized = false;
    }

    void write(VkDescriptorSet set, std::initializer_list<std::pair<VkImageView, VkDescriptorType>> items) {
        VkDescriptorImageInfo infos[3]{};
        VkWriteDescriptorSet writes[3]{};
        std::uint32_t n = 0;
        for (const auto& [view, type] : items) {
            infos[n] = {type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? sampler : VK_NULL_HANDLE, view,
                        VK_IMAGE_LAYOUT_GENERAL};
            writes[n] = {};
            writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[n].dstSet = set;
            writes[n].dstBinding = n;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType = type;
            writes[n].pImageInfo = &infos[n];
            ++n;
        }
        vkUpdateDescriptorSets(gpu.device, n, writes, 0, nullptr);
    }
};

LocalToneProcessor::LocalToneProcessor(const rawr::vk::GpuContext& gpu, std::uint32_t slots)
    : impl_(std::make_unique<Impl>(gpu, slots)) {}
LocalToneProcessor::~LocalToneProcessor() = default;

bool LocalToneProcessor::record(const RecordInfo& info) {
    if (!active(info.params) || !info.commandBuffer || !info.measure.view || info.width == 0 || info.height == 0 ||
        info.targetCount < 1 || info.targetCount > 2 || !(info.renderGain > 0.0f) || !std::isfinite(info.renderGain))
        return false;
    for (std::uint32_t t = 0; t < info.targetCount; ++t)
        if (!info.targets[t].image || !info.targets[t].view) return false;
    auto& d = *impl_;
    auto& slot = d.slots[std::min<std::size_t>(info.slot, d.slots.size() - 1)];
    const std::uint32_t block = blockSize(info.width, info.height);
    const std::uint32_t lowW = (info.width + block - 1) / block, lowH = (info.height + block - 1) / block;
    d.ensureImages(slot, lowW, lowH);
    const VkCommandBuffer cmd = info.commandBuffer;

    const auto storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    d.write(slot.sets[Luma], {{info.measure.view, storage}, {slot.lumaImage.view, storage}});
    d.write(slot.sets[BoxH1], {{slot.lumaImage.view, storage}, {slot.temp.view, storage}});
    d.write(slot.sets[BoxV1], {{slot.temp.view, storage}, {slot.means.view, storage}});
    d.write(slot.sets[Coeff], {{slot.means.view, storage}, {slot.coeffs.view, storage}});
    d.write(slot.sets[BoxH2], {{slot.coeffs.view, storage}, {slot.temp.view, storage}});
    d.write(slot.sets[BoxV2], {{slot.temp.view, storage}, {slot.means.view, storage}});
    d.write(slot.sets[Lift], {{slot.means.view, storage}, {slot.lumaImage.view, storage}, {slot.map.view, storage}});
    for (std::uint32_t t = 0; t < info.targetCount; ++t)
        d.write(slot.sets[Apply0 + t],
                {{slot.map.view, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, {info.targets[t].view, storage}});

    // The low-res images start UNDEFINED once; afterwards they stay GENERAL.
    {
        VkImageMemoryBarrier barriers[5]{};
        const VkImage images[5] = {slot.lumaImage.image, slot.temp.image, slot.means.image, slot.coeffs.image,
                                   slot.map.image};
        for (int i = 0; i < 5; ++i) {
            barriers[i] = {};
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barriers[i].oldLayout = slot.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].srcQueueFamilyIndex = barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image = images[i];
            barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 5, barriers);
        slot.initialized = true;
    }

    Push push{};
    push.size[0] = info.width;
    push.size[1] = info.height;
    push.size[2] = lowW;
    push.size[3] = lowH;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) push.toWorking[c][r] = info.cameraToWorking[std::size_t(c * 3 + r)];
    push.logGain = std::log2(info.renderGain);
    push.strength = info.params.strength;
    push.detail = info.params.detail;
    push.eps = kEps;
    push.block = block;
    push.radius = kRadius;

    const auto memory = [&] {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    };
    const auto dispatch = [&](VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set, std::uint32_t w,
                              std::uint32_t h, std::uint32_t group) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (w + group - 1) / group, (h + group - 1) / group, 1);
    };
    dispatch(d.luma, d.twoLayout, slot.sets[Luma], lowW, lowH, 8);
    memory();
    for (const auto [first, second] : {std::pair{BoxH1, BoxV1}, std::pair{BoxH2, BoxV2}}) {
        if (first == BoxH2) {
            dispatch(d.coeff, d.twoLayout, slot.sets[Coeff], lowW, lowH, 8);
            memory();
        }
        push.vertical = 0;
        dispatch(d.box, d.twoLayout, slot.sets[first], lowW, lowH, 8);
        memory();
        push.vertical = 1;
        dispatch(d.box, d.twoLayout, slot.sets[second], lowW, lowH, 8);
        memory();
    }
    dispatch(d.lift, d.threeLayout, slot.sets[Lift], lowW, lowH, 8);
    memory();
    for (std::uint32_t t = 0; t < info.targetCount; ++t)
        dispatch(d.applyPipeline, d.applyLayout, slot.sets[Apply0 + t], info.width, info.height, 16);

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
