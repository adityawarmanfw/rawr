// Exercise both production apply shaders with a known, constant chromaticity
// across an intensity edge. The guide's alpha carries unrelated neighborhood
// brightness; it must never tint the reconstruction of a clipped channel.
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "vk_test_common.hpp"
#include "vulkan_test_context.hpp"

using image_scopes::tests::VulkanTestContext;
using vktest::ck;

static std::vector<uint8_t> halfPixels(const std::vector<float>& values) {
    std::vector<uint8_t> out(values.size() * 2);
    for (size_t i = 0; i < values.size(); ++i) {
        const _Float16 h = static_cast<_Float16>(values[i]);
        std::memcpy(out.data() + i * 2, &h, 2);
    }
    return out;
}

static void run(VulkanTestContext& vk, const std::string& directory, bool preview, const std::array<float, 3>& color,
                unsigned clippedChannel, float shading, bool seedOnly = false) {
    constexpr unsigned W = 128, H = 32;
    const unsigned step = preview ? 4 : 8, gw = W / step, gh = H / step;
    auto input = vk.createRgbaImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT);
    auto output = vk.createRgbaImage(W, H, VK_FORMAT_R16G16B16A16_SFLOAT);
    auto guide = vk.createRgbaImage(gw, gh, VK_FORMAT_R16G16B16A16_SFLOAT);
    const unsigned sensorScale = preview ? 1 : 2, mw = W / sensorScale, mh = H / sensorScale;
    auto mask = vk.createRgbaImage(mw, mh, VK_FORMAT_R16_UINT);
    std::vector<uint16_t> maskValues(mw * mh);
    std::vector<float> pixels(W * H * 4), expected(pixels.size()), guides(gw * gh * 4);
    struct Push {
        unsigned w, h, enabled, pad;
        float ceilings[4];
    } push{W, H, 1, 0, {4, 4, 4, 0}};
    if (clippedChannel < 3) push.ceilings[clippedChannel] = 1;
    for (unsigned y = 0; y < H; ++y) {
        for (unsigned x = 0; x < W; ++x) {
            // Dark/light step plus a gentle ramp: include unclipped, partially
            // repaired and fully clipped pixels, without losing all channels.
            const float level = x < W / 2 ? .4f + .4f * x / (W / 2) : 1.2f + .4f * (x - W / 2) / (W / 2);
            const size_t i = (y * W + x) * 4;
            for (unsigned k = 0; k < 3; ++k) {
                expected[i + k] = color[k] * level;
                if (expected[i + k] >= push.ceilings[k]) {
                    const unsigned bit = k == 0 ? 1 : k == 1 ? 6 : 8;
                    maskValues[(y / sensorScale) * mw + x / sensorScale] |= bit;
                }
                pixels[i + k] = std::min(expected[i + k], push.ceilings[k]) * shading;
                expected[i + k] *= shading;
            }
            pixels[i + 3] = expected[i + 3] = 1;
        }
    }
    const float sum = color[0] + color[1] + color[2];
    for (unsigned y = 0; y < gh; ++y) {
        for (unsigned x = 0; x < gw; ++x) {
            const size_t i = (y * gw + x) * 4;
            for (unsigned k = 0; k < 3; ++k) guides[i + k] = color[k] / sum;
            guides[i + 3] = .5f + 2.0f * y / (gh - 1);  // independent brightness field
        }
    }
    vk.uploadRgba(input, halfPixels(pixels));
    vk.uploadRgba(output, halfPixels(std::vector<float>(pixels.size())));
    vk.uploadRgba(guide, halfPixels(guides));
    vk.uploadRgba(mask, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(maskValues.data()),
                                                 maskValues.size() * sizeof(uint16_t)));
    const std::string shader = seedOnly ? (preview ? "/highlight_seed_1.spv" : "/highlight_seed_2.spv")
                                        : (preview ? "/highlight_apply_inplace.spv" : "/highlight_recover.spv");
    std::ifstream file(directory + shader, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("missing shader");
    std::vector<uint32_t> code(size_t(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = code.size() * 4;
    sm.pCode = code.data();
    VkShaderModule module;
    ck(vkCreateShaderModule(vk.device(), &sm, nullptr, &module), "shader");
    const unsigned count = preview || seedOnly ? 3 : 4;
    std::vector<VkDescriptorSetLayoutBinding> bindings(count);
    for (unsigned i = 0; i < count; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = count;
    si.pBindings = bindings.data();
    VkDescriptorSetLayout layout;
    ck(vkCreateDescriptorSetLayout(vk.device(), &si, nullptr, &layout), "set layout");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    VkPipelineLayout pipelineLayout;
    ck(vkCreatePipelineLayout(vk.device(), &li, nullptr, &pipelineLayout), "pipeline layout");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_COMPUTE_BIT,
                module,
                "main",
                nullptr};
    ci.layout = pipelineLayout;
    VkPipeline pipeline;
    ck(vkCreateComputePipelines(vk.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline), "pipeline");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    VkDescriptorPool pool;
    ck(vkCreateDescriptorPool(vk.device(), &pi, nullptr, &pool), "pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set;
    ck(vkAllocateDescriptorSets(vk.device(), &ai, &set), "set");
    const std::vector<VkImageView> views = seedOnly  ? std::vector{input.view, mask.view, guide.view}
                                           : preview ? std::vector{input.view, mask.view, guide.view}
                                                     : std::vector{input.view, output.view, mask.view, guide.view};
    std::vector<VkDescriptorImageInfo> infos(count);
    std::vector<VkWriteDescriptorSet> writes(count);
    for (unsigned i = 0; i < count; ++i) {
        infos[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,   set,     i,      0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,       &infos[i], nullptr, nullptr};
    }
    vkUpdateDescriptorSets(vk.device(), count, writes.data(), 0, nullptr);
    vk.beginCommands();
    vkCmdBindPipeline(vk.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(vk.commandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
    if (seedOnly) {
        push.enabled = gw;
        push.pad = gh;
    }  // seed push: guide width/height
    vkCmdPushConstants(vk.commandBuffer(), pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(vk.commandBuffer(), ((seedOnly ? gw : W) + 15) / 16, ((seedOnly ? gh : H) + 15) / 16, 1);
    vk.submitAndWait();
    const auto result = vk.downloadRgba(seedOnly ? guide : preview ? input : output);
    float maxError = 0;
    if (seedOnly) {
        // First guide column is fully valid but above its nominal WB ceiling
        // after shading. It must remain an authoritative, correctly colored seed.
        for (unsigned y = 0; y < gh; ++y) {
            for (unsigned k = 0; k < 4; ++k) {
                uint16_t h;
                std::memcpy(&h, result.data() + 2 * (y * gw * 4 + k), 2);
                const float actual = vktest::halfToFloat(h);
                if (k == 3) {
                    if (!(actual < 0)) throw std::runtime_error("valid shaded seed was rejected");
                } else
                    maxError = std::max(maxError, std::abs(actual - color[k] / sum));
            }
        }
    } else
        for (size_t i = 0; i < expected.size(); ++i) {
            uint16_t h;
            std::memcpy(&h, result.data() + 2 * i, 2);
            const float actual = vktest::halfToFloat(h);
            if (!std::isfinite(actual)) throw std::runtime_error("nonfinite reconstruction");
            maxError = std::max(maxError, std::abs(actual - expected[i]));
        }
    std::cout << (seedOnly ? "seed " : "apply ") << (preview ? "preview" : "still/video")
              << " clipped=" << clippedChannel << " shading=" << shading << " color=" << color[0] << ',' << color[1]
              << ',' << color[2] << " maxError=" << maxError << '\n';
    vkDestroyDescriptorPool(vk.device(), pool, nullptr);
    vkDestroyPipeline(vk.device(), pipeline, nullptr);
    vkDestroyPipelineLayout(vk.device(), pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(vk.device(), layout, nullptr);
    vkDestroyShaderModule(vk.device(), module, nullptr);
    vk.destroyRgbaImage(input);
    vk.destroyRgbaImage(output);
    vk.destroyRgbaImage(guide);
    vk.destroyRgbaImage(mask);
    if (maxError > .004f * shading)
        throw std::runtime_error("highlight color/intensity contaminated by neighborhood brightness");
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: highlight_fit_validation SPIRV_DIRECTORY");
        VulkanTestContext vk;
        std::cout << "GPU: " << vk.deviceName() << '\n';
        for (bool preview : {false, true})
            for (unsigned channel = 0; channel < 4; ++channel)
                for (const auto& color :
                     {std::array{1.f, 1.f, 1.f}, std::array{1.2f, 1.f, .8f}, std::array{1.2f, .8f, 1.1f}})
                    for (float shading : {1.f, 2.5f}) run(vk, argv[1], preview, color, channel, shading);
        for (bool preview : {false, true}) run(vk, argv[1], preview, {1.2f, .8f, 1.1f}, 1, 4.f, true);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
