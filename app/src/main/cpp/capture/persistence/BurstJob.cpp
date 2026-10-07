#include "capture/persistence/ProcessingRecipe.h"
#include <rawr/raw_multiframe_output/MultiframeOutputAdapter.h>

#include <cstring>
#include <chrono>
#include <android/log.h>
#include <stdexcept>
#include <vector>

#include "capture/persistence/CaptureJob.h"
#include "vulkan/VulkanContext.h"

namespace rawrcam::capture::persistence {
namespace {
void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("capture_job_vulkan_" + std::to_string(r));
}
struct Upload {
    VkDevice device{};
    VkPhysicalDevice physical{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkBuffer buffer{};
    VkDeviceMemory bufferMemory{};
    void* mapped{};
    struct Image {
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
    };
    std::vector<Image> images;
    ~Upload() {
        for (auto& i : images) {
            if (i.view) vkDestroyImageView(device, i.view, nullptr);
            if (i.image) vkDestroyImage(device, i.image, nullptr);
            if (i.memory) vkFreeMemory(device, i.memory, nullptr);
        }
        if (mapped) vkUnmapMemory(device, bufferMemory);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (bufferMemory) vkFreeMemory(device, bufferMemory, nullptr);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
    }
    uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("capture_job_memory_type");
    }
    void initialize(const vulkan::VulkanContext& vk, uint64_t bytes) {
        device = vk.device();
        physical = vk.physicalDevice();
        VkCommandPoolCreateInfo p{};
        p.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        p.queueFamilyIndex = vk.queueFamily();
        p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &p, nullptr, &pool));
        VkCommandBufferAllocateInfo c{};
        c.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        c.commandPool = pool;
        c.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        c.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &c, &command));
        VkFenceCreateInfo f{};
        f.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device, &f, nullptr, &fence));
        VkBufferCreateInfo b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        b.size = bytes;
        b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(device, &b, nullptr, &buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo a{};
        a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        a.allocationSize = req.size;
        a.memoryTypeIndex =
            memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device, &a, nullptr, &bufferMemory));
        check(vkBindBufferMemory(device, buffer, bufferMemory, 0));
        check(vkMapMemory(device, bufferMemory, 0, bytes, 0, &mapped));
    }
    rawr::zsl_ring::GpuRawImageView add(const vulkan::VulkanContext& vk, std::mutex& mutex, uint32_t width,
                                        uint32_t height, uint64_t timestamp, const std::vector<uint8_t>& raw) {
        images.emplace_back();
        auto& i = images.back();
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16_UINT;
        ci.extent = {width, height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(device, &ci, nullptr, &i.image));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, i.image, &req);
        VkMemoryAllocateInfo a{};
        a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        a.allocationSize = req.size;
        a.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device, &a, nullptr, &i.memory));
        check(vkBindImageMemory(device, i.image, i.memory, 0));
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = i.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R16_UINT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &vi, nullptr, &i.view));
        std::memcpy(mapped, raw.data(), raw.size());
        check(vkResetCommandBuffer(command, 0));
        check(vkResetFences(device, 1, &fence));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = i.image;
        barrier.subresourceRange = vi.subresourceRange;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(command, buffer, i.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        {
            std::lock_guard<std::mutex> lock(mutex);
            check(vkQueueSubmit(vk.queue(), 1, &submit, fence));
        }
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        return {{images.size(), timestamp, width, height}, i.image, i.view, VK_FORMAT_R16_UINT, req.size};
    }
};
}  // namespace
void saveBurst(const std::string& path, multiframe::MultiframeWorkItem& work, const vulkan::VulkanContext& vk,
               std::mutex& mutex) {
    const auto started = std::chrono::steady_clock::now();
    double readbackMs = 0.0;
    // Sharpest-reference selection already ran on the GPU in the spool thread
    // before this call (MfsrCaptureService::spoolLoop), so the persisted
    // reference is final here. Each frame is synchronously read back before
    // its bytes are written; this is not one batched GPU transfer.
    auto& burst = *work.capture;
    CaptureJob job;
    job.multiframe = true;
    job.frame.width = burst.frames.front().raw.ref.width;
    job.frame.height = burst.frames.front().raw.ref.height;
    job.frame.packedRowStrideBytes = job.frame.width * 2;
    job.frame.metadata = burst.referenceMetadata;
    job.frame.colorState = burst.referenceColor;
    job.frame.requestId = work.requestId;
    work.baseDng.resolvedRecipe = resolvedRecipe(work.frozenTonemap, work.frozenTonemap.aePostGain,
                                                work.filmEnabled, work.filmLook);
    work.mergedDng.resolvedRecipe = work.baseDng.resolvedRecipe;
    job.dng = work.baseDng;
    job.mergedDng = work.mergedDng;
    job.jpeg = work.mergedJpeg;
    job.jpegRequested = work.jpegRequested;
    job.tone = work.frozenTonemap;
    job.filmEnabled = work.filmEnabled;
    job.film = work.filmLook;
    job.tuning = work.tuning;
    job.baseFrameMode = work.baseFrameMode;
    // Never persist a score vector that disagrees with the burst; a size
    // mismatch means a bug upstream, and readers treat empty as "unmeasured".
    job.sharpnessScores =
        (work.sharpnessScores.size() == burst.frames.size()) ? work.sharpnessScores : std::vector<float>{};
    job.sharpnessMs = work.sharpnessMs;
    job.referenceIndex = burst.referenceIndex;
    job.metadata = burst.metadata;
    for (auto& f : burst.frames) job.parameters.push_back(f.parameters);
    rawr::raw_multiframe_output::MultiframeOutputAdapter adapter;
    adapter.initialize(
        vk.physicalDevice(), vk.device(), vk.queueFamily(),
        [&](const VkSubmitInfo& info, VkFence fence) {
            std::lock_guard<std::mutex> lock(mutex);
            check(vkQueueSubmit(vk.queue(), 1, &info, fence));
        },
        job.frame.width, job.frame.height);
    save(path, job, [&](size_t i) {
        const auto begin = std::chrono::steady_clock::now();
        auto pixels = adapter.readBase(burst.frames[i].raw.image).pixels;
        readbackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        return pixels;
    });
    const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
        "BURST_SAVE requestId=%llu frames=%zu bytes=%llu totalMs=%.1f readbackMs=%.1f storageAndSetupMs=%.1f",
        static_cast<unsigned long long>(work.requestId), burst.frames.size(),
        static_cast<unsigned long long>(uint64_t(job.frame.width) * job.frame.height * 2 * burst.frames.size()),
        totalMs, readbackMs, totalMs - readbackMs);
}
std::unique_ptr<multiframe::MultiframeWorkItem> loadBurst(const std::string& path, const vulkan::VulkanContext& vk,
                                                          std::mutex& mutex) {
    const auto started = std::chrono::steady_clock::now();
    double uploadMs = 0.0;
    auto work = std::make_unique<multiframe::MultiframeWorkItem>();
    work->capture = std::make_unique<multiframe::PendingMultiframeCapture>();
    auto uploaded = std::make_shared<Upload>();
    auto job = load(path, [&](CaptureJob& j, size_t i, const std::vector<uint8_t>& raw) {
        if (!j.multiframe) throw std::runtime_error("capture_job_not_burst");
        const auto begin = std::chrono::steady_clock::now();
        if (i == 0) uploaded->initialize(vk, raw.size());
        rawr::raw_gpu_pipeline::BurstFrame frame;
        frame.raw = uploaded->add(vk, mutex, j.frame.width, j.frame.height, j.metadata[i].timestampNs, raw);
        frame.parameters = j.parameters[i];
        work->capture->frames.push_back(frame);
        work->capture->refs.push_back(frame.raw.ref);
        uploadMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    });
    auto& b = *work->capture;
    b.recoveryImages = uploaded;
    b.metadata = job.metadata;
    b.referenceIndex = job.referenceIndex;
    b.referenceMetadata = job.frame.metadata;
    b.referenceColor = job.frame.colorState;
    work->requestId = job.frame.requestId;
    work->baseDng = job.dng;
    work->mergedDng = job.mergedDng;
    work->mergedJpeg = job.jpeg;
    work->jpegRequested = job.jpegRequested;
    work->frozenTonemap = job.tone;
    work->filmEnabled = job.filmEnabled;
    work->filmLook = job.film;
    work->tuning = job.tuning;
    work->baseFrameMode = job.baseFrameMode;
    work->sharpnessScores = job.sharpnessScores;
    work->sharpnessMs = job.sharpnessMs;
    const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
        "BURST_LOAD requestId=%llu frames=%zu totalMs=%.1f uploadMs=%.1f storageAndSetupMs=%.1f",
        static_cast<unsigned long long>(work->requestId), b.frames.size(), totalMs, uploadMs, totalMs - uploadMs);
    return work;
}
}  // namespace rawrcam::capture::persistence
