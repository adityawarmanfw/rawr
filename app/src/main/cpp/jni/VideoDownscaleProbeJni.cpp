// 1080p downscale probe (debug tooling). Feeds a Bayer-mosaicked zone plate
// through the shipped VideoDemosaic with each 2x reduction filter, times it on
// the GPU and writes the RGBA16F output for host scoring. No camera involved.
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
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
void fillZonePlate(uint16_t* raw) {
    const auto crop = rawrcam::video::videoSourceRect(kRawWidth, kRawHeight, kOutWidth, kOutHeight);
    const double cx = crop.width / 2.0, cy = crop.height / 2.0;
    const double k = 0.5 / (2.0 * std::sqrt(cx * cx + cy * cy));
    for (uint32_t y = 0; y < kRawHeight; ++y) {
        for (uint32_t x = 0; x < kRawWidth; ++x) {
            const double u = double(x) - crop.x + 0.5 - cx, v = double(y) - crop.y + 0.5 - cy;
            const double zone = 0.5 + 0.4 * std::cos(2.0 * M_PI * k * (u * u + v * v));
            raw[size_t(y) * kRawWidth + x] = static_cast<uint16_t>(std::lround(zone * kWhite));
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
        fillZonePlate(static_cast<uint16_t*>(raw.mapped));
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
        frame.wb = {1, 1, 1, 1};
        frame.white = kWhite;
        frame.cfa = 0;

        constexpr int kWarmup = 3, kTimed = 20;
        bool first = true;
        for (const bool antiAlias : {false, true}) {
            demosaic.setAntiAlias(antiAlias);
            const std::string name = antiAlias ? "lanczos" : "box";
            std::vector<double> times;
            for (int i = 0; i < kWarmup + kTimed; ++i) {
                const uint32_t slot = uint32_t(i) % rawrcam::video::VideoDemosaic::kFramesInFlight;
                const bool last = i == kWarmup + kTimed - 1;
                check(vkResetCommandBuffer(command, 0), "reset");
                VkCommandBufferBeginInfo begin{};
                begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                check(vkBeginCommandBuffer(command, &begin), "begin");
                vkCmdResetQueryPool(command, queries, 0, 2);
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
                demosaic.record(command, slot, frame);
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
                if (last) {
                    VkImageMemoryBarrier toCopy{};
                    toCopy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    toCopy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                    toCopy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    toCopy.image = demosaic.outputImage(slot);
                    toCopy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &toCopy);
                    VkBufferImageCopy region{};
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.imageExtent = {kOutWidth, kOutHeight, 1};
                    vkCmdCopyImageToBuffer(command, demosaic.outputImage(slot), VK_IMAGE_LAYOUT_GENERAL,
                                           readback.buffer, 1, &region);
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
                if (i >= kWarmup &&
                    vkGetQueryPoolResults(device, queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                    stamps[1] > stamps[0])
                    times.push_back(double(stamps[1] - stamps[0]) * properties.limits.timestampPeriod / 1.0e6);
            }
            std::ofstream(outDir + "/downscale_" + name + ".f16", std::ios::binary)
                .write(static_cast<const char*>(readback.mapped), std::streamsize(kOutWidth) * kOutHeight * 8);
            std::sort(times.begin(), times.end());
            const double median = times.empty() ? 0.0 : times[times.size() / 2];
            report += std::string(first ? "" : ",") + "\"" + name + "\":{\"method\":\"" + demosaic.method() +
                      "\",\"gpuMedianMs\":" + std::to_string(median) +
                      ",\"gpuMinMs\":" + std::to_string(times.empty() ? 0.0 : times.front()) + "}";
            first = false;
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
