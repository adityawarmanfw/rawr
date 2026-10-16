#include "ImageScopesProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "image_scopes/image_scopes.h"

namespace rawrcam::monitoring {
namespace {
void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(what);
}

uint32_t findHostVisibleMemoryType(VkPhysicalDevice physical, uint32_t bits, bool* coherent) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) &&
            (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            *coherent = true;
            return i;
        }
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            *coherent = false;
            return i;
        }
    }
    throw std::runtime_error("image scopes AE feedback: no host-visible memory");
}

void computeWriteToComputeRead(VkCommandBuffer command, VkImage image) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}
}  // namespace

ImageScopesProcessor::ImageScopesProcessor() = default;

ImageScopesProcessor::~ImageScopesProcessor() { reset(); }

void ImageScopesProcessor::initialize(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue,
                                      VkCommandPool commandPool, uint32_t queueFamilyIndex, float timestampPeriodNs,
                                      uint32_t inputWidth, uint32_t inputHeight, Diagnostic diagnostic) {
    reset();
    if (physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE ||
        commandPool == VK_NULL_HANDLE || inputWidth == 0 || inputHeight == 0) {
        throw std::invalid_argument("ImageScopesProcessor invalid initialization");
    }
    physicalDevice_ = physicalDevice;
    device_ = device;
    diagnostic_ = std::move(diagnostic);
    timestampPeriodNs_ = timestampPeriodNs;
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
    if (queueFamilyIndex < queueFamilyCount && timestampPeriodNs_ > 0.0f) {
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());
        timestampValidBits_ = queueFamilies[queueFamilyIndex].timestampValidBits;
        if (timestampValidBits_ > 0) {
            VkQueryPoolCreateInfo qi{};
            qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = rawrcam::imaging::kRealtimeFramesInFlight * 2u;
            if (vkCreateQueryPool(device_, &qi, nullptr, &exposureStatsTimingPool_) != VK_SUCCESS) {
                exposureStatsTimingPool_ = VK_NULL_HANDLE;
                timestampValidBits_ = 0;
            }
        }
    }
    inputWidth_ = inputWidth;
    inputHeight_ = inputHeight;
    image_scopes::CreateInfo ci{};
    ci.context.physicalDevice = physicalDevice;
    ci.context.device = device;
    ci.maxFramesInFlight = rawrcam::imaging::kRealtimeFramesInFlight;
    backend_ = std::make_unique<image_scopes::ImageScopes>(ci);
    for (auto& rb : exposureStatsReadback_) createReadback(rb, sizeof(image_scopes::DisplayExposureStatsData));
    for (auto& rb : sceneStatsReadback_) createReadback(rb, sizeof(image_scopes::SceneExposureStatsData));
    for (auto& slot : targets_) {
        for (auto& target : slot) {
            target = rawrcam::vulkan::createOwnedImage(physicalDevice, device, kRenderWidth, kRenderHeight,
                                                       VK_FORMAT_R8G8B8A8_UNORM,
                                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        }
    }
    transitionImagesToGeneral(queue, commandPool);
}

void ImageScopesProcessor::createReadback(ExposureStatsReadback& rb, VkDeviceSize bytes) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device_, &bi, nullptr, &rb.buffer), "image scopes readback buffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, rb.buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findHostVisibleMemoryType(physicalDevice_, req.memoryTypeBits, &rb.coherent);
    vkCheck(vkAllocateMemory(device_, &ai, nullptr, &rb.memory), "image scopes readback memory");
    vkCheck(vkBindBufferMemory(device_, rb.buffer, rb.memory, 0), "image scopes readback bind");
    vkCheck(vkMapMemory(device_, rb.memory, 0, VK_WHOLE_SIZE, 0, &rb.mapped), "image scopes readback map");
    rb.allocationSize = req.size;
    std::memset(rb.mapped, 0, static_cast<size_t>(bytes));
}

void ImageScopesProcessor::destroyReadback(ExposureStatsReadback& rb) noexcept {
    if (rb.mapped && rb.memory) vkUnmapMemory(device_, rb.memory);
    if (rb.buffer) vkDestroyBuffer(device_, rb.buffer, nullptr);
    if (rb.memory) vkFreeMemory(device_, rb.memory, nullptr);
    rb = {};
}

bool ImageScopesProcessor::readReadback(ExposureStatsReadback& rb, void* out, size_t bytes) {
    if (!rb.recorded || !rb.mapped || !rb.memory) return false;
    if (!rb.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = rb.memory;
        range.offset = 0;
        range.size = rb.allocationSize;
        vkInvalidateMappedMemoryRanges(device_, 1, &range);
    }
    std::memcpy(out, rb.mapped, bytes);
    rb.recorded = false;
    return true;
}

void ImageScopesProcessor::reset() noexcept {
    backend_.reset();
    if (device_ != VK_NULL_HANDLE && exposureStatsTimingPool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, exposureStatsTimingPool_, nullptr);
    }
    exposureStatsTimingPool_ = VK_NULL_HANDLE;
    timestampPeriodNs_ = 0.0f;
    timestampValidBits_ = 0;
    exposureStatsTimingSamples_ = 0;
    exposureStatsTimingTotalNs_ = 0.0;
    exposureStatsTimingBestNs_ = 0.0;
    exposureStatsTimingWorstNs_ = 0.0;
    exposureStatsTimingComplete_ = false;

    if (device_ != VK_NULL_HANDLE) {
        for (auto& rb : exposureStatsReadback_) destroyReadback(rb);
        for (auto& rb : sceneStatsReadback_) destroyReadback(rb);
    }
    if (device_ != VK_NULL_HANDLE) {
        for (auto& slot : targets_)
            for (auto& target : slot) rawrcam::vulkan::destroyOwnedImage(device_, target);
    }
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    diagnostic_ = {};
    inputWidth_ = inputHeight_ = 0;
}

void ImageScopesProcessor::record(VkCommandBuffer command, uint32_t frameSlot, VkImageView displayView,
                                  VkImage displayImage, uint32_t sourceQuarterTurns, bool measureExposureFeedback,
                                  uint32_t sourceWidth, uint32_t sourceHeight, image_scopes::SamplingMode sampling) {
    if (!backend_ || frameSlot >= rawrcam::imaging::kRealtimeFramesInFlight) return;
    bool any = false;
    for (const auto& placement : state_.placements)
        if (placement.type != ScopeType::None) {
            any = true;
            break;
        }
    if (!any && !measureExposureFeedback) return;
    exposureStatsReadback_[frameSlot].recorded = false;
    bool needsDisplay = measureExposureFeedback;
    for (const auto& placement : state_.placements) {
        needsDisplay |= placement.type == ScopeType::Waveform || placement.type == ScopeType::Vectorscope;
    }
    if (needsDisplay) computeWriteToComputeRead(command, displayImage);
    image_scopes::DisplayImageView input{displayView, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL,
                                         sourceWidth ? sourceWidth : inputWidth_,
                                         sourceHeight ? sourceHeight : inputHeight_};
    if (measureExposureFeedback) {
        if (exposureStatsTimingPool_ != VK_NULL_HANDLE && !exposureStatsTimingComplete_) {
            const uint32_t q = frameSlot * 2u;
            vkCmdResetQueryPool(command, exposureStatsTimingPool_, q, 2u);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, exposureStatsTimingPool_, q);
        }
        image_scopes::DisplayExposureStatsRecordInfo feedback{};
        feedback.commandBuffer = command;
        feedback.input = input;
        feedback.frameSlot = frameSlot;
        feedback.sampleBlockSize = 4u;
        backend_->recordDisplayExposureStats(feedback);
        backend_->recordCopyDisplayExposureStats(command, frameSlot, exposureStatsReadback_[frameSlot].buffer, 0);
        if (exposureStatsTimingPool_ != VK_NULL_HANDLE && !exposureStatsTimingComplete_) {
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TRANSFER_BIT, exposureStatsTimingPool_, frameSlot * 2u + 1u);
        }
        VkBufferMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = exposureStatsReadback_[frameSlot].buffer;
        host.offset = 0;
        host.size = sizeof(image_scopes::DisplayExposureStatsData);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &host, 0, nullptr);
        exposureStatsReadback_[frameSlot].recorded = true;
    }
    for (uint32_t i = 0; i < state_.placements.size(); ++i) {
        const auto type = state_.placements[i].type;
        if (type == ScopeType::None) continue;
        image_scopes::ScopeRenderTarget target{targets_[frameSlot][i].view, VK_FORMAT_R8G8B8A8_UNORM,
                                               VK_IMAGE_LAYOUT_GENERAL, kRenderWidth, kRenderHeight};
        if (type == ScopeType::Waveform) {
            image_scopes::DisplayWaveformRecordInfo info{};
            info.commandBuffer = command;
            info.input = input;
            info.renderTarget = target;
            info.frameSlot = frameSlot;
            info.samplingMode = sampling;
            info.render.mode = state_.placements[i].variant == 1u ? image_scopes::WaveformMode::RgbOverlay
                                                                  : image_scopes::WaveformMode::Luma;
            // The input remains the native WxH display buffer. Only waveform positional X
            // needs the authoritative sensor->display mapping; vectorscope never does.
            // This does not rotate/reallocate the source image or change measurement geometry.
            info.sourceQuarterTurns = sourceQuarterTurns;
            backend_->recordDisplayWaveform(info);
        } else if (type == ScopeType::Vectorscope) {
            image_scopes::VectorscopeRecordInfo info{};
            info.commandBuffer = command;
            info.input = input;
            info.renderTarget = target;
            info.frameSlot = frameSlot;
            info.samplingMode = sampling;
            info.grid = image_scopes::VectorscopeGrid::Reference256;
            backend_->recordVectorscope(info);
        }
    }
}

void ImageScopesProcessor::recordSceneExposure(VkCommandBuffer command, uint32_t frameSlot, VkImageView linearView,
                                               uint32_t width, uint32_t height,
                                               const std::array<float, 3>& lumaWeights, float clipLuma,
                                               const std::array<float, 4>& spot) {
    if (!backend_ || frameSlot >= rawrcam::imaging::kRealtimeFramesInFlight || linearView == VK_NULL_HANDLE) return;
    auto& rb = sceneStatsReadback_[frameSlot];
    rb.recorded = false;
    sceneClipLuma_[frameSlot] = clipLuma;
    image_scopes::SceneExposureStatsRecordInfo info{};
    info.commandBuffer = command;
    info.input = {linearView, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL, width, height};
    info.frameSlot = frameSlot;
    info.sampleBlockSize = 4u;
    info.lumaWeights[0] = lumaWeights[0];
    info.lumaWeights[1] = lumaWeights[1];
    info.lumaWeights[2] = lumaWeights[2];
    info.clipLuma = clipLuma;
    info.spotX0 = spot[0];
    info.spotY0 = spot[1];
    info.spotX1 = spot[2];
    info.spotY1 = spot[3];
    backend_->recordSceneExposureStats(info);
    backend_->recordCopySceneExposureStats(command, frameSlot, rb.buffer, 0);
    VkBufferMemoryBarrier host{};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = rb.buffer;
    host.offset = 0;
    host.size = sizeof(image_scopes::SceneExposureStatsData);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host,
                         0, nullptr);
    rb.recorded = true;
}

std::optional<SceneExposureStats> ImageScopesProcessor::consumeSceneExposure(uint32_t frameSlot) {
    if (frameSlot >= sceneStatsReadback_.size()) return std::nullopt;
    SceneExposureStats out{};
    if (!readReadback(sceneStatsReadback_[frameSlot], &out, sizeof(out))) return std::nullopt;
    if (out.sampleCount == 0) return std::nullopt;
    return out;
}

void ImageScopesProcessor::retireFrameSlot(uint32_t frameSlot) {
    if (backend_) backend_->retireFrameSlot(frameSlot);
}
std::optional<RenderedExposureFeedback> ImageScopesProcessor::consumeExposureFeedback(uint32_t frameSlot) {
    if (frameSlot >= exposureStatsReadback_.size()) return std::nullopt;
    consumeExposureStatsTiming(frameSlot);
    RenderedExposureFeedback out{};
    if (!readReadback(exposureStatsReadback_[frameSlot], &out, sizeof(out))) return std::nullopt;
    if (out.sampleCount == 0) return std::nullopt;
    return out;
}
void ImageScopesProcessor::consumeExposureStatsTiming(uint32_t frameSlot) noexcept {
    if (exposureStatsTimingComplete_ || exposureStatsTimingPool_ == VK_NULL_HANDLE || timestampValidBits_ == 0 ||
        frameSlot >= rawrcam::imaging::kRealtimeFramesInFlight)
        return;
    struct TimestampResult {
        uint64_t value;
        uint64_t available;
    } results[2]{};
    const VkResult result =
        vkGetQueryPoolResults(device_, exposureStatsTimingPool_, frameSlot * 2u, 2u, sizeof(results), results,
                              sizeof(TimestampResult), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS || results[0].available == 0 || results[1].available == 0) return;
    const uint64_t mask = timestampValidBits_ >= 64u ? ~uint64_t{0} : ((uint64_t{1} << timestampValidBits_) - 1u);
    const uint64_t ticks = (results[1].value - results[0].value) & mask;
    const double ns = static_cast<double>(ticks) * static_cast<double>(timestampPeriodNs_);
    if (!std::isfinite(ns) || ns < 0.0) return;
    ++exposureStatsTimingSamples_;
    exposureStatsTimingTotalNs_ += ns;
    if (exposureStatsTimingSamples_ == 1u) {
        exposureStatsTimingBestNs_ = exposureStatsTimingWorstNs_ = ns;
    } else {
        exposureStatsTimingBestNs_ = std::min(exposureStatsTimingBestNs_, ns);
        exposureStatsTimingWorstNs_ = std::max(exposureStatsTimingWorstNs_, ns);
    }
    if (exposureStatsTimingSamples_ >= 120u) {
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(3);
        out << "DISPLAY_EXPOSURE_STATS_GPU_BENCH samples=" << exposureStatsTimingSamples_
            << " sampling=6.25pct cadence_max=15Hz"
            << " avgMs=" << (exposureStatsTimingTotalNs_ / static_cast<double>(exposureStatsTimingSamples_) / 1e6)
            << " bestMs=" << (exposureStatsTimingBestNs_ / 1e6) << " worstMs=" << (exposureStatsTimingWorstNs_ / 1e6)
            << " includes=measure+reduce+32B_copy";
        const std::string line = out.str();
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "%s", line.c_str());
#endif
        if (diagnostic_) diagnostic_(line);
        exposureStatsTimingComplete_ = true;
    }
}

void ImageScopesProcessor::discardFrameSlot(uint32_t frameSlot) noexcept {
    if (frameSlot < exposureStatsReadback_.size()) exposureStatsReadback_[frameSlot].recorded = false;
    if (frameSlot < sceneStatsReadback_.size()) sceneStatsReadback_[frameSlot].recorded = false;
    if (!backend_) return;
    try {
        backend_->discardFrameSlotRecordings(frameSlot);
    } catch (...) {
    }
}

VkImageView ImageScopesProcessor::renderedView(uint32_t frameSlot, uint32_t placementIndex) const {
    if (frameSlot >= targets_.size() || placementIndex >= 3) return VK_NULL_HANDLE;
    return targets_[frameSlot][placementIndex].view;
}
VkImage ImageScopesProcessor::renderedImage(uint32_t frameSlot, uint32_t placementIndex) const {
    if (frameSlot >= targets_.size() || placementIndex >= 3) return VK_NULL_HANDLE;
    return targets_[frameSlot][placementIndex].image;
}

void ImageScopesProcessor::transitionImagesToGeneral(VkQueue queue, VkCommandPool commandPool) {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(device_, &ai, &command), "scope allocate transition command");
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command, &bi), "scope begin transition command");
    std::array<VkImageMemoryBarrier, rawrcam::imaging::kRealtimeFramesInFlight * 3> barriers{};
    size_t n = 0;
    for (const auto& slot : targets_)
        for (const auto& target : slot) {
            auto& b = barriers[n++];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = target.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, static_cast<uint32_t>(n), barriers.data());
    vkCheck(vkEndCommandBuffer(command), "scope end transition command");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command;
    vkCheck(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "scope submit transition command");
    vkCheck(vkQueueWaitIdle(queue), "scope transition wait");
    vkFreeCommandBuffers(device_, commandPool, 1, &command);
}

}  // namespace rawrcam::monitoring
