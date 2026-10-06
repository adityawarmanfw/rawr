// 1080p downscale probe (debug tooling). Feeds a Bayer-mosaicked zone plate
// through the shipped VideoDemosaic with each 2x reduction filter, times it on
// the GPU and writes the RGBA16F output for host scoring. No camera involved.
#include <jni.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "video_pipeline/VideoCrop.h"
#include "video_pipeline/VideoDemosaic.h"
#include "vulkan/VulkanContext.h"
#include "vulkan/VulkanDispatch.h"

namespace {
constexpr uint32_t kRawWidth = 4080, kRawHeight = 3072, kOutWidth = 1920, kOutHeight = 1080;
constexpr float kWhite = 16383.0f;

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

HostBuffer makeBuffer(rawrcam::vulkan::VulkanContext& context, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    HostBuffer out;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    check(vkCreateBuffer(context.device(), &info, nullptr, &out.buffer), "buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device(), out.buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &properties);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) throw std::runtime_error("no host-visible memory");
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = type;
    check(vkAllocateMemory(context.device(), &alloc, nullptr, &out.memory), "memory");
    check(vkBindBufferMemory(context.device(), out.buffer, out.memory, 0), "bind");
    check(vkMapMemory(context.device(), out.memory, 0, bytes, 0, &out.mapped), "map");
    return out;
}

void destroyBuffer(VkDevice device, HostBuffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}

// Zone plate over the 1080p source crop: local frequency grows with radius
// and reaches the RAW Nyquist (0.5 cycles/pixel) at the crop corners. Grey,
// so every CFA site carries the same value.
void fillZonePlate(uint16_t* raw, const std::array<float, 4>& wb) {
    const auto crop = rawrcam::video::videoSourceRect(kRawWidth, kRawHeight, kOutWidth, kOutHeight);
    const double cx = crop.width / 2.0, cy = crop.height / 2.0;
    const double k = 0.5 / (2.0 * std::sqrt(cx * cx + cy * cy));
    for (uint32_t y = 0; y < kRawHeight; ++y) {
        for (uint32_t x = 0; x < kRawWidth; ++x) {
            const double u = double(x) - crop.x + 0.5 - cx, v = double(y) - crop.y + 0.5 - cy;
            const double zone = 0.5 + 0.4 * std::cos(2.0 * M_PI * k * (u * u + v * v));
            const int site = int(x & 1u) | int((y & 1u) << 1);  // RGGB
            raw[size_t(y) * kRawWidth + x] = static_cast<uint16_t>(std::lround(zone * kWhite / wb[site]));
        }
    }
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_VideoDownscaleProbe_nativeRun(JNIEnv* env, jobject,
                                                                                          jstring outDirJava) {
    const char* outChars = env->GetStringUTFChars(outDirJava, nullptr);
    const std::string outDir = outChars;
    env->ReleaseStringUTFChars(outDirJava, outChars);
    std::string report = "{";
    rawrcam::vulkan::VulkanContext context;
    HostBuffer raw, readback;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    try {
        rawrcam::vulkan::dispatch::configure("", "");
        context.createInstance();
        context.createDeviceForSurface(VK_NULL_HANDLE);
        const VkDevice device = context.device();
        raw = makeBuffer(context, VkDeviceSize(kRawWidth) * kRawHeight * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        readback = makeBuffer(context, VkDeviceSize(kOutWidth) * kOutHeight * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = context.queueFamily();
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "pool");
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &alloc, &command), "command");
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence");
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        check(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "queries");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(context.physicalDevice(), &properties);

        rawrcam::video::VideoDemosaic demosaic(context.gpuContext(), kRawWidth, kRawHeight, kOutWidth, kOutHeight);
        rawrcam::video::VideoDemosaic::Frame frame{};
        frame.rawBuffer = raw.buffer;
        frame.rawStridePixels = kRawWidth;
        frame.black = {0, 0, 0, 0};
        frame.white = kWhite;
        frame.cfa = 0;

        // Same RAW, 4K output: exactly the first pass of the former two-pass 1080p path.
        rawrcam::video::VideoDemosaic demosaic4k(context.gpuContext(), kRawWidth, kRawHeight, 3840, 2160);
        struct Variant {
            const char* name;
            bool uhd;
            bool antiAlias;
        };
        const Variant variants[] = {{"box", false, false}, {"strip", false, true}, {"demosaic_4k", true, false}};
        constexpr int kVariants = sizeof(variants) / sizeof(variants[0]);
        uint32_t frameIndex = 0;
        auto runOnce = [&](const Variant& variant, bool copy) -> double {
            auto& stage = variant.uhd ? demosaic4k : demosaic;
            stage.setAntiAlias(variant.antiAlias);
            const uint32_t slot = frameIndex++ % rawrcam::video::VideoDemosaic::kFramesInFlight;
            check(vkResetCommandBuffer(command, 0), "reset");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command, &begin), "begin");
            vkCmdResetQueryPool(command, queries, 0, 2);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
            stage.record(command, slot, frame);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
            if (copy && !variant.uhd) {
                VkImageMemoryBarrier toCopy{};
                toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                toCopy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                toCopy.image = stage.outputImage(slot);
                toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &toCopy);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {kOutWidth, kOutHeight, 1};
                vkCmdCopyImageToBuffer(command, stage.outputImage(slot), VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1,
                                       &region);
            }
            check(vkEndCommandBuffer(command), "end");
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            check(vkResetFences(device, 1, &fence), "reset fence");
            check(vkQueueSubmit(context.queue(), 1, &submit, fence), "submit");
            check(vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull), "wait");
            uint64_t stamps[2]{};
            if (vkGetQueryPoolResults(device, queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) != VK_SUCCESS ||
                stamps[1] <= stamps[0])
                return -1.0;
            return double(stamps[1] - stamps[0]) * properties.limits.timestampPeriod / 1.0e6;
        };
        // Neutral white balance, then a strong cast: the zone plate stays grey
        // after white balance, so any chroma in the second scene is the
        // filter's fault.
        const std::array<float, 4> balances[] = {{1, 1, 1, 1}, {1.9f, 1, 1, 1.5f}};
        bool first = true;
        for (int scene = 0; scene < 2; ++scene) {
            fillZonePlate(static_cast<uint16_t*>(raw.mapped), balances[scene]);
            frame.wb = balances[scene];
            // Warm the GPU clock up, then interleave the variants so they are
            // timed under the same clock instead of one after another.
            for (int i = 0; i < 30; ++i) (void)runOnce(variants[i % kVariants], false);
            std::array<std::vector<double>, kVariants> times;
            for (int round = 0; round < 30; ++round)
                for (int v = 0; v < kVariants; ++v)
                    if (const double ms = runOnce(variants[v], false); ms > 0) times[v].push_back(ms);
            for (int v = 0; v < kVariants; ++v) {
                (void)runOnce(variants[v], true);
                const std::string name = std::string(variants[v].name) + (scene == 0 ? "" : "_wb");
                if (!variants[v].uhd)
                    std::ofstream(outDir + "/downscale_" + name + ".f16", std::ios::binary)
                        .write(static_cast<const char*>(readback.mapped),
                               std::streamsize(kOutWidth) * kOutHeight * 8);
                std::sort(times[v].begin(), times[v].end());
                const double median = times[v].empty() ? 0.0 : times[v][times[v].size() / 2];
                report += std::string(first ? "" : ",") + "\"" + name + "\":{\"method\":\"" +
                          (variants[v].uhd ? demosaic4k : demosaic).method() + "\",\"gpuMedianMs\":" +
                          std::to_string(median) + "}";
                first = false;
            }
        }
        report += ",\"success\":true}";
    } catch (const std::exception& error) {
        report += std::string("\"success\":false,\"error\":\"") + error.what() + "\"}";
    }
    if (context.device()) {
        (void)context.waitIdle();
        if (queries) vkDestroyQueryPool(context.device(), queries, nullptr);
        if (fence) vkDestroyFence(context.device(), fence, nullptr);
        if (pool) vkDestroyCommandPool(context.device(), pool, nullptr);
        destroyBuffer(context.device(), raw);
        destroyBuffer(context.device(), readback);
    }
    return env->NewStringUTF(report.c_str());
}

// Debug: try to create the strip pass's compute pipeline from every *.spv in
// `dir` (same descriptor layout as the shipped strip pass) and report each
// VkResult, to bisect driver compiler rejections without app rebuilds.
#include <dirent.h>
extern "C" JNIEXPORT jstring JNICALL Java_com_rawr_camera_video_VideoDownscaleProbe_nativeCompileCheck(JNIEnv* env,
                                                                                                   jobject,
                                                                                                   jstring dirJava) {
    const char* dirChars = env->GetStringUTFChars(dirJava, nullptr);
    const std::string dir = dirChars;
    env->ReleaseStringUTFChars(dirJava, dirChars);
    std::string report = "{";
    rawrcam::vulkan::VulkanContext context;
    try {
        rawrcam::vulkan::dispatch::configure("", "");
        context.createInstance();
        context.createDeviceForSurface(VK_NULL_HANDLE);
        const VkDevice device = context.device();
        const VkDescriptorType types[6] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        VkDescriptorSetLayoutBinding bindings[6]{};
        for (uint32_t i = 0; i < 6; ++i) bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount = 6;
        setInfo.pBindings = bindings;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout), "set layout");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "layout");
        std::vector<std::string> names;
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() > 4 && n.substr(n.size() - 4) == ".spv") names.push_back(n);
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        bool first = true;
        for (const auto& name : names) {
            std::ifstream in(dir + "/" + name, std::ios::binary);
            std::vector<char> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            VkShaderModuleCreateInfo shaderInfo{};
            shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderInfo.codeSize = code.size();
            shaderInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
            VkShaderModule module = VK_NULL_HANDLE;
            VkResult result = vkCreateShaderModule(device, &shaderInfo, nullptr, &module);
            if (result == VK_SUCCESS) {
                VkComputePipelineCreateInfo pipelineInfo{};
                pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                pipelineInfo.stage.module = module;
                pipelineInfo.stage.pName = "main";
                pipelineInfo.layout = layout;
                VkPipeline pipeline = VK_NULL_HANDLE;
                result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
                if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
                vkDestroyShaderModule(device, module, nullptr);
            }
            report += std::string(first ? "" : ",") + "\"" + name + "\":" + std::to_string(result);
            first = false;
        }
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        report += ",\"success\":true}";
    } catch (const std::exception& error) {
        report += std::string("\"success\":false,\"error\":\"") + error.what() + "\"}";
    }
    return env->NewStringUTF(report.c_str());
}
