#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "vk_test_common.hpp"

namespace {
using namespace vktest;
struct Push {
    float black[4];
    float invRange[4];
    float wb[4];
    uint32_t width, height, outWidth, outHeight;
    uint32_t cropX, cropY, pattern, stridePixels;
    uint32_t bufferEnabled, reduceCfa, lscEnabled, lscWidth, lscHeight, monitorEnabled;
};
static_assert(sizeof(Push) == 104);

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void barrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout,
             VkImageLayout newLayout, VkAccessFlags source, VkAccessFlags destination,
             VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcAccessMask = source;
    b.dstAccessMask = destination;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

int run(int argc, char** argv) {
    if (argc != 19) {
        std::cerr << "usage: runner shader.spv raw.u16 output.rgba16f rawW rawH outW outH white cfa "
                     "blackR blackG1 blackG2 blackB wbR wbG1 wbG2 wbB repetitions\n"
                     "output is pre-WB sensor-linear RGB; RUNNER_CLIP_OUT=path also exports the r16ui clip-state image\n";
        return 2;
    }
    const uint32_t rawW = std::stoul(argv[4]), rawH = std::stoul(argv[5]);
    const uint32_t outW = std::stoul(argv[6]), outH = std::stoul(argv[7]);
    const float white = std::stof(argv[8]);
    const uint32_t cfa = std::stoul(argv[9]);
    const int repeats = std::max(1, std::stoi(argv[18]));
    const bool reduce = uint64_t(outW) * 2u <= rawW && uint64_t(outH) * 2u <= rawH;
    const uint32_t sourceW = reduce ? outW * 2u : outW;
    const uint32_t sourceH = reduce ? outH * 2u : outH;
    if ((rawW & 1u) || (outW & 1u) || (outH & 1u) || rawW < sourceW || rawH < sourceH || cfa > 3u)
        throw std::runtime_error("unsupported geometry or CFA");
    const auto shader = readFile(argv[1]);
    const auto raw = readFile(argv[2]);
    if (raw.size() != size_t(rawW) * rawH * 2u || (shader.size() & 3u))
        throw std::runtime_error("RAW or SPIR-V size invalid");
    Push push{};
    for (int i = 0; i < 4; ++i) {
        push.black[i] = std::stof(argv[10 + i]);
        push.invRange[i] = 1.0f / std::max(white - push.black[i], 1.0f);
        push.wb[i] = std::stof(argv[14 + i]);
    }
    push.width = rawW;
    push.height = rawH;
    push.outWidth = outW;
    push.outHeight = outH;
    push.cropX = ((rawW - sourceW) / 2u) & ~1u;
    push.cropY = ((rawH - sourceH) / 2u) & ~1u;
    push.pattern = cfa;
    push.stridePixels = rawW;
    push.bufferEnabled = 1;
    push.reduceCfa = reduce ? 1u : 0u;
    // RUNNER_LSC=1 enables a unity 17x13 lens-shading grid so timings include
    // the per-sample lookup the app performs with lens shading on.
    // RUNNER_LSC_RAMP=1 instead uploads a smooth synthetic falloff grid
    // (corners ~1.2-1.35x, per-channel like a real shading map) to validate
    // hoisted-LSC variants against per-tap LSC with varying gains.
    const bool lscRamp = std::getenv("RUNNER_LSC_RAMP") != nullptr;
    const bool lscOn = std::getenv("RUNNER_LSC") != nullptr || lscRamp;
    const uint32_t lscW = lscOn ? 17u : 1u, lscH = lscOn ? 13u : 1u;
    push.lscEnabled = lscOn ? 1u : 0u;
    push.lscWidth = lscW;
    push.lscHeight = lscH;

    Ctx context = ctx();
    const VkDevice device = context.dev;
    const Buf rawBuffer = mkBuf(context.pd, device, raw.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const VkDeviceSize lscBytes = VkDeviceSize(lscW) * lscH * 16u;
    const Buf lscBuffer = mkBuf(context.pd, device, lscBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const Buf readback = mkBuf(context.pd, device, size_t(outW) * outH * 8u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const char* clipOutPath = std::getenv("RUNNER_CLIP_OUT");
    const Buf clipReadback = mkBuf(context.pd, device, size_t(outW / 2u) * (outH / 2u) * 2u,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Img dummy = mkImg(context.pd, device, 1, 1, VK_FORMAT_R16_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
    Img output = mkImg(context.pd, device, outW, outH, VK_FORMAT_R16G16B16A16_SFLOAT,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Img clipState = mkImg(context.pd, device, outW / 2u, outH / 2u, VK_FORMAT_R16_UINT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    void* mapped = nullptr;
    ck(vkMapMemory(device, rawBuffer.m, 0, raw.size(), 0, &mapped), "map RAW");
    std::memcpy(mapped, raw.data(), raw.size());
    vkUnmapMemory(device, rawBuffer.m);
    ck(vkMapMemory(device, lscBuffer.m, 0, lscBytes, 0, &mapped), "map LSC");
    std::vector<float> gains(size_t(lscBytes / 4u), 1.0f);
    if (lscRamp) {
        // Interleaved [R, G_even, G_odd, B] per grid point; radial falloff.
        const float k[4] = {0.35f, 0.20f, 0.22f, 0.30f};
        for (uint32_t gy = 0; gy < lscH; ++gy) {
            for (uint32_t gx = 0; gx < lscW; ++gx) {
                const float nx = float(gx) / float(lscW - 1u) - 0.5f;
                const float ny = float(gy) / float(lscH - 1u) - 0.5f;
                const float r2 = 4.0f * (nx * nx + ny * ny);  // 0 center, ~1 corners
                for (uint32_t c = 0; c < 4; ++c)
                    gains[(size_t(gy) * lscW + gx) * 4u + c] = 1.0f + k[c] * r2;
            }
        }
    }
    std::memcpy(mapped, gains.data(), size_t(lscBytes));
    vkUnmapMemory(device, lscBuffer.m);

    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = bindings.size();
    setInfo.pBindings = bindings.data();
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    ck(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout), "descriptor layout");
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    ck(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "pipeline layout");
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = shader.size();
    shaderInfo.pCode = reinterpret_cast<const uint32_t*>(shader.data());
    VkShaderModule module = VK_NULL_HANDLE;
    ck(vkCreateShaderModule(device, &shaderInfo, nullptr, &module), "shader module");
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ck(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "compute pipeline");
    vkDestroyShaderModule(device, module, nullptr);
    std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = sizes.size();
    poolInfo.pPoolSizes = sizes.data();
    VkDescriptorPool pool = VK_NULL_HANDLE;
    ck(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool), "descriptor pool");
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    ck(vkAllocateDescriptorSets(device, &alloc, &set), "descriptor set");
    VkDescriptorImageInfo rawImage{};
    rawImage.imageView = dummy.v;
    rawImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo rawWords{rawBuffer.b, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo outputImage{};
    outputImage.imageView = output.v;
    outputImage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo lsc{lscBuffer.b, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo clipImage{VK_NULL_HANDLE, clipState.v, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &rawImage;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &rawWords;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &outputImage;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &lsc;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &clipImage;
    vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);

    VkCommandPoolCreateInfo commandPoolInfo{};
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.queueFamilyIndex = context.qf;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    ck(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &commandPool), "command pool");
    VkCommandBufferAllocateInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandInfo.commandPool = commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    ck(vkAllocateCommandBuffers(device, &commandInfo, &command), "command buffer");
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    ck(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence");
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;
    VkQueryPool queries = VK_NULL_HANDLE;
    ck(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "timestamp queries");

    std::vector<double> gpuMs;
    for (int iteration = 0; iteration < repeats + 2; ++iteration) {
        ck(vkResetCommandBuffer(command, 0), "reset command");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ck(vkBeginCommandBuffer(command, &begin), "begin command");
        if (iteration == 0) {
            barrier(command, dummy.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        barrier(command, output.i, iteration == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL, iteration == 0 ? 0 : VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, iteration == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, clipState.i, iteration == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL, iteration == 0 ? 0 : VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, iteration == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdResetQueryPool(command, queries, 0, 2);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 0);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        // VIDEO_TILE matches the shader's workgroup side (8 or 16).
        const uint32_t tile = std::getenv("VIDEO_TILE") ? uint32_t(std::stoul(std::getenv("VIDEO_TILE"))) : 8u;
        vkCmdDispatch(command, (outW + tile - 1u) / tile, (outH + tile - 1u) / tile, 1);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
        if (iteration == repeats + 1) {
            barrier(command, output.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {outW, outH, 1};
            vkCmdCopyImageToBuffer(command, output.i, VK_IMAGE_LAYOUT_GENERAL, readback.b, 1, &copy);
            if (clipOutPath) {
                barrier(command, clipState.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy clipCopy{};
                clipCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                clipCopy.imageExtent = {outW / 2u, outH / 2u, 1};
                vkCmdCopyImageToBuffer(command, clipState.i, VK_IMAGE_LAYOUT_GENERAL, clipReadback.b, 1, &clipCopy);
            }
        }
        ck(vkEndCommandBuffer(command), "end command");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        ck(vkQueueSubmit(context.q, 1, &submit, fence), "submit");
        ck(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait fence");
        uint64_t times[2]{};
        ck(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(times), times, sizeof(uint64_t),
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "timestamp results");
        if (iteration >= 2) gpuMs.push_back(double(times[1] - times[0]) * context.prop.limits.timestampPeriod / 1e6);
        ck(vkResetFences(device, 1, &fence), "reset fence");
    }
    ck(vkMapMemory(device, readback.m, 0, readback.size, 0, &mapped), "map output");
    std::ofstream outputFile(argv[3], std::ios::binary);
    outputFile.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(readback.size));
    if (!outputFile) throw std::runtime_error("write output failed");
    vkUnmapMemory(device, readback.m);
    if (clipOutPath) {
        void* clipMapped = nullptr;
        ck(vkMapMemory(device, clipReadback.m, 0, clipReadback.size, 0, &clipMapped), "map clip output");
        std::ofstream clipFile(clipOutPath, std::ios::binary);
        clipFile.write(static_cast<const char*>(clipMapped), static_cast<std::streamsize>(clipReadback.size));
        if (!clipFile) throw std::runtime_error("write clip output failed");
        vkUnmapMemory(device, clipReadback.m);
    }
    std::sort(gpuMs.begin(), gpuMs.end());
    std::cout << "{\"gpu\":\"" << context.prop.deviceName << "\",\"raw\":[" << rawW << ',' << rawH
              << "],\"output\":[" << outW << ',' << outH << "],\"crop\":[" << push.cropX << ','
              << push.cropY << ',' << sourceW << ',' << sourceH << "],\"reduced\":"
              << (reduce ? "true" : "false") << ",\"samples\":" << gpuMs.size()
              << ",\"medianGpuMs\":" << gpuMs[gpuMs.size() / 2]
              << ",\"p95GpuMs\":" << gpuMs[std::min(gpuMs.size() - 1,
                    static_cast<size_t>(gpuMs.size() * 0.95))] << "}\n";

    vkDestroyQueryPool(device, queries, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    delImg(device, output);
    delImg(device, clipState);
    delImg(device, dummy);
    auto rawOwned = rawBuffer, lscOwned = lscBuffer, readbackOwned = readback, clipReadbackOwned = clipReadback;
    delBuf(device, rawOwned);
    delBuf(device, lscOwned);
    delBuf(device, readbackOwned);
    delBuf(device, clipReadbackOwned);
    delCtx(context);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "VIDEO_DEMOSAIC_MOLTENVK_FAIL " << error.what() << '\n'; return 1; }
}
