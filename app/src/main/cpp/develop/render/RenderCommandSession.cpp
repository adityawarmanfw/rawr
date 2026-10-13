#include "develop/render/RenderCommandSession.h"

#include <android/log.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>
namespace rawrcam::develop::rendered {
namespace {
void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}
}  // namespace
void RenderCommandSession::create(const RenderDeviceContext& context) {
    context_ = context;
    commands_.create(context_.device, context_.queueFamily, 1);
    command_ = commands_.command();
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCheck(vkCreateFence(context_.device, &fi, nullptr, &fence_), "rendered create fence");

    // Timestamp instrumentation was present in Candidate11 but its query
    // pool was never created. Candidate12 makes the existing timing path
    // real so LUT validation reports actual device GPU time.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice, &props);
    timestampPeriodNs_ = props.limits.timestampPeriod;
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qfamilies(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice, &qcount, qfamilies.data());
    if (context_.queueFamily < qfamilies.size())
        timestampValidBits_ = qfamilies[context_.queueFamily].timestampValidBits;
    // Queue topology for still/preview isolation planning: a second
    // queue would let heavy stills run off the preview queue so the
    // viewfinder doesn't serialize behind multi-second film work.
    for (uint32_t qi = 0; qi < qcount; ++qi) {
        __android_log_print(
            ANDROID_LOG_INFO, "RawrCamNative", "STILL_QUEUE family=%u/%u queues=%u flags=0x%x stampBits=%u", qi, qcount,
            qfamilies[qi].queueCount, (unsigned)qfamilies[qi].queueFlags, qfamilies[qi].timestampValidBits);
    }
    if (timestampValidBits_ > 0 && timestampPeriodNs_ > 0.0f) {
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        // 0-7 WB/HL/FCC/tonemap, 8-9 gainmap, 10-11 denoise,
        // 14-15 galosh-yuv (12-13 spare).
        qi.queryCount = timingQueryCount;
        vkCheck(vkCreateQueryPool(context_.device, &qi, nullptr, &timingPool_), "rendered create timing query pool");
    }
}
void RenderCommandSession::begin() {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &begin), "rendered begin command");
    if (timingPool_) vkCmdResetQueryPool(command_, timingPool_, 0, timingQueryCount);
}
void RenderCommandSession::submitAndRestart(const std::string& stage) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const auto chunkBegin = std::chrono::steady_clock::now();
    vkCheck(vkEndCommandBuffer(command_), (stage + " end command").c_str());
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    {
        std::lock_guard<std::mutex> q(*context_.queueSubmitMutex);
        vkCheck(vkQueueSubmit(context_.queue, 1, &submit, fence_), (stage + " submit").c_str());
    }
    vkCheck(vkWaitForFences(context_.device, 1, &fence_, VK_TRUE, 60ull * 1000000000ull), (stage + " wait").c_str());
    chunkWaitMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - chunkBegin).count();
    vkCheck(vkResetFences(context_.device, 1, &fence_), (stage + " reset fence").c_str());
    vkCheck(vkResetCommandBuffer(command_, 0), (stage + " reset command").c_str());
    vkCheck(vkBeginCommandBuffer(command_, &begin), (stage + " begin command").c_str());
    VkMemoryBarrier previousChunk{};
    previousChunk.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    previousChunk.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    previousChunk.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &previousChunk, 0,
                         nullptr, 0, nullptr);
}
void RenderCommandSession::finish(uint64_t requestId) {
    vkCheck(vkEndCommandBuffer(command_), "rendered end command");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_;
    {
        std::lock_guard<std::mutex> lock(*context_.queueSubmitMutex);
        vkCheck(vkQueueSubmit(context_.queue, 1, &submit, fence_), "rendered submit");
    }
    __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "STILL_RENDER_SUBMITTED requestId=%llu",
                        (unsigned long long)requestId);
    vkCheck(vkWaitForFences(context_.device, 1, &fence_, VK_TRUE, 60ull * 1000000000ull), "rendered wait fence");
}
std::optional<std::array<uint64_t, RenderCommandSession::timingQueryCount>> RenderCommandSession::timestamps() {
    if (!timingPool_) return std::nullopt;
    std::array<uint64_t, timingQueryCount> ts{};
    VkResult queryResult = VK_NOT_READY;
    for (int attempt = 0; attempt < 500; ++attempt) {
        queryResult = vkGetQueryPoolResults(context_.device, timingPool_, 0, timingQueryCount, sizeof(ts), ts.data(),
                                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (queryResult == VK_SUCCESS || queryResult != VK_NOT_READY) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (queryResult != VK_SUCCESS) return std::nullopt;
    return ts;
}
double RenderCommandSession::milliseconds(const std::array<uint64_t, timingQueryCount>& ts, int a,
                                          int b) const noexcept {
    const uint64_t mask = timestampValidBits_ >= 64 ? ~uint64_t(0) : ((uint64_t(1) << timestampValidBits_) - 1u);
    return double((ts[b] - ts[a]) & mask) * double(timestampPeriodNs_) * 1e-6;
}
void RenderCommandSession::reset() noexcept {
    if (context_.device) {
        if (timingPool_) vkDestroyQueryPool(context_.device, timingPool_, nullptr);
        if (fence_) vkDestroyFence(context_.device, fence_, nullptr);
        commands_.reset();
    }
    timingPool_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    command_ = VK_NULL_HANDLE;
    timestampPeriodNs_ = 0.0f;
    timestampValidBits_ = 0;
    chunkWaitMs_ = 0.0;
    context_ = {};
}
}  // namespace rawrcam::develop::rendered
