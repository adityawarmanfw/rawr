#include "develop/SharedHighlightRuntime.h"

#include <android/log.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <raw_preview/RawPreview.hpp>
#include <stdexcept>
#include <utility>

#include "develop/galosh/GaloshShaderTable.h"
#include "galosh/GaloshVkUtils.hpp"
#include "replay_tagged_decode.h"
#include "sensor_clip_pack.h"

namespace rawrcam::develop {
namespace {
void vkCheck(VkResult result, const std::string& what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(what + " VkResult=" + std::to_string(result));
    }
}
uint32_t findHostMemoryType(VkPhysicalDevice physical, uint32_t bits, const std::string& label) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    throw std::runtime_error(label + " still runtime requires HOST_VISIBLE|HOST_COHERENT memory");
}
}  // namespace

SharedHighlightRuntime::SharedHighlightRuntime(std::string label, Diagnostic diagnostic)
    : label_(std::move(label)), diagnostic_(std::move(diagnostic)) {}
SharedHighlightRuntime::~SharedHighlightRuntime() { reset(); }

void SharedHighlightRuntime::configure(const rawrcam::vulkan::VulkanContext& context, std::mutex& queueSubmitMutex,
                                       uint32_t width, uint32_t height, uint32_t rawPreviewCfa,
                                       bool sharedHighlightPacked, VkQueue queueOverride,
                                       std::mutex* queueSubmitMutexOverride, bool externalInput) {
    if (!context.physicalDevice() || !context.device() || !context.queue()) {
        throw std::invalid_argument(label_ + " requires initialized Vulkan context");
    }
    if (configured()) {
        if (device_ != context.device() || width_ != width || height_ != height || rawPreviewCfa_ != rawPreviewCfa ||
            sharedHighlightPacked_ != sharedHighlightPacked || externalInput_ != externalInput) {
            throw std::logic_error(label_ + " still runtime reconfiguration requires reset");
        }
        if (queueOverride) queue_ = queueOverride;
        queueSubmitMutex_ = queueSubmitMutexOverride ? queueSubmitMutexOverride : &queueSubmitMutex;
        float16Compute_ = context.float16ComputeEnabled();
        return;
    }
    physicalDevice_ = context.physicalDevice();
    device_ = context.device();
    queue_ = queueOverride ? queueOverride : context.queue();
    queueFamily_ = context.queueFamily();
    float16Compute_ = context.float16ComputeEnabled();
    queueSubmitMutex_ = queueSubmitMutexOverride ? queueSubmitMutexOverride : &queueSubmitMutex;
    width_ = width;
    height_ = height;
    rawPreviewCfa_ = rawPreviewCfa;
    sharedHighlightPacked_ = sharedHighlightPacked;
    externalInput_ = externalInput;
}

void SharedHighlightRuntime::ensureResources() {
    if (resourcesReady()) return;
    if (!configured() || !queueSubmitMutex_) {
        throw std::logic_error(label_ + " runtime initialization before configure");
    }
    try {
        if (!externalInput_)
            input_ = rawrcam::vulkan::createOwnedImage(physicalDevice_, device_, width_, height_, VK_FORMAT_R16_UINT,
                                                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (sharedHighlightPacked_ && !externalInput_) {
            packed_ = rawrcam::vulkan::createOwnedImage(
                physicalDevice_, device_, width_ / 2u, height_ / 2u, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            clipState_ = rawrcam::vulkan::createOwnedImage(
                physicalDevice_, device_, width_ / 2u, height_ / 2u, VK_FORMAT_R16_UINT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            replayInput_ = rawrcam::vulkan::createOwnedImage(
                physicalDevice_, device_, width_ / 2u, height_ / 2u, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        }
        output_ = rawrcam::vulkan::createOwnedImage(
            physicalDevice_, device_, width_, height_, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (sharedHighlightPacked_ && !externalInput_) {
            createSharedHighlightPipeline();
            createReplayHighlightPipeline();
        }

        if (!externalInput_) {
            const uint64_t bytes64 = static_cast<uint64_t>(width_) * height_ * sizeof(uint16_t);
            if (bytes64 > static_cast<uint64_t>(std::numeric_limits<VkDeviceSize>::max())) {
                throw std::overflow_error(label_ + " upload allocation overflow");
            }
            uploadBytes_ = static_cast<VkDeviceSize>(bytes64);
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = uploadBytes_;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            vkCheck(vkCreateBuffer(device_, &bi, nullptr, &uploadBuffer_), label_ + " create upload buffer");
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, uploadBuffer_, &req);
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = findHostMemoryType(physicalDevice_, req.memoryTypeBits, label_);
            vkCheck(vkAllocateMemory(device_, &ai, nullptr, &uploadMemory_), label_ + " allocate upload memory");
            vkCheck(vkBindBufferMemory(device_, uploadBuffer_, uploadMemory_, 0), label_ + " bind upload memory");
            vkCheck(vkMapMemory(device_, uploadMemory_, 0, uploadBytes_, 0, &uploadMapped_),
                    label_ + " map upload memory");
        }

        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = queueFamily_;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCheck(vkCreateCommandPool(device_, &pi, nullptr, &commandPool_), label_ + " create command pool");
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = commandPool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(device_, &cai, &command_), label_ + " allocate command buffer");
        vkCheck(vkAllocateCommandBuffers(device_, &cai, &spareCommand_), label_ + " allocate spare command buffer");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCheck(vkCreateFence(device_, &fi, nullptr, &fence_), label_ + " create fence");
        vkCheck(vkCreateFence(device_, &fi, nullptr, &spareFence_), label_ + " create spare fence");
    } catch (...) {
        discardWorkspace();
        throw;
    }
}

VkCommandBuffer SharedHighlightRuntime::beginExternal() {
    ensureResources();
    waitSpare();
    vkCheck(vkResetFences(device_, 1, &fence_), label_ + " external reset fence");
    vkCheck(vkResetCommandBuffer(command_, 0), label_ + " external reset command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &begin), label_ + " external begin command buffer");
    VkImageMemoryBarrier out{};
    out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    out.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    out.srcQueueFamilyIndex = out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.image = output_.image;
    out.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    out.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    // The external packed input was written by another queue submission that
    // the caller fence-waited; this barrier only orders the output.
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &out);
    return command_;
}

VkCommandBuffer SharedHighlightRuntime::beginTaggedPackedReplay(const std::vector<uint8_t>& taggedPackedRgba16f,
                                                                const std::array<float, 4>& whiteBalanceRggb,
                                                                uint32_t variant, float anchorTolerance, float madLimit,
                                                                float consensusLimit, uint32_t minSupport) {
    ensureResources();
    if (!sharedHighlightPacked_ || !packed_.image || !replayInput_.image || !replayPipeline_)
        throw std::logic_error(label_ + " tagged replay resources unavailable");
    const size_t expected = static_cast<size_t>(width_ / 2u) * (height_ / 2u) * 8u;
    if (taggedPackedRgba16f.size() != expected || expected != static_cast<size_t>(uploadBytes_))
        throw std::runtime_error(label_ + " tagged replay payload size mismatch");
    std::memcpy(uploadMapped_, taggedPackedRgba16f.data(), expected);
    waitSpare();
    vkCheck(vkResetFences(device_, 1, &fence_), label_ + " tagged replay reset fence");
    vkCheck(vkResetCommandBuffer(command_, 0), label_ + " tagged replay reset command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &begin), label_ + " tagged replay begin command buffer");

    VkImageMemoryBarrier in{};
    in.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    in.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    in.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    in.srcQueueFamilyIndex = in.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    in.image = replayInput_.image;
    in.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    in.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &in);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width_ / 2u, height_ / 2u, 1};
    vkCmdCopyBufferToImage(command_, uploadBuffer_, replayInput_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    in.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    in.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    in.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    in.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &in);

    VkImageMemoryBarrier p{};
    p.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    p.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    p.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    p.srcQueueFamilyIndex = p.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    p.image = packed_.image;
    p.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    VkImageMemoryBarrier clip = p;
    clip.image = clipState_.image;
    VkImageMemoryBarrier replayOutputs[2]{p, clip};
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 2, replayOutputs);

    struct Push {
        float wb[4];
        uint32_t width, height, variant;
        float anchorTolerance, madLimit, consensusLimit;
        uint32_t minSupport;
    } push{};
    static_assert(sizeof(Push) == 44);
    for (size_t i = 0; i < 4; i++) push.wb[i] = whiteBalanceRggb[i];
    push.width = width_;
    push.height = height_;
    push.variant = variant;
    push.anchorTolerance = anchorTolerance;
    push.madLimit = madLimit;
    push.consensusLimit = consensusLimit;
    push.minSupport = minSupport;
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, replayPipeline_);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, replayLayout_, 0, 1, &replaySet_, 0, nullptr);
    vkCmdPushConstants(command_, replayLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &push);
    vkCmdDispatch(command_, ((width_ / 2u) + 7u) / 8u, ((height_ / 2u) + 7u) / 8u, 1);
    p.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    p.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    clip.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    clip.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkImageMemoryBarrier replayReady[2]{p, clip};
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 2, replayReady);

    VkImageMemoryBarrier out{};
    out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    out.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    out.srcQueueFamilyIndex = out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.image = output_.image;
    out.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    out.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &out);
    return command_;
}

void SharedHighlightRuntime::ensureGalosh() {
    if (galosh_) {
        if (galoshDevice_ == device_) return;
        galosh_.reset();
        galoshDevice_ = VK_NULL_HANDLE;
    }
    if (!queueSubmitMutex_) throw std::logic_error(label_ + " galosh without queue mutex");
    galosh_ =
        std::make_unique<::galosh::GaloshRawPipeline>(physicalDevice_, device_, queueFamily_, galosh::rawShaderMap());
    galoshDevice_ = device_;
}

VkCommandBuffer SharedHighlightRuntime::beginFrame(
    const std::vector<uint8_t>& rawCopy, const std::array<float, 4>& blackPhysical, float whiteLevel,
    const std::array<float, 4>& whiteBalanceRggb,
    const rawrcam::develop::highlight::LensShadingMapSnapshot& lensShading, bool highlightReconstructionEnabled,
    const ::galosh::GaloshRawParams& galosh) {
    // Reconstruction now belongs exclusively to the post-WB RGB stage. This
    // argument is retained at the adapter boundary for source compatibility.
    (void)highlightReconstructionEnabled;
    ensureResources();
    if (rawCopy.size() != static_cast<size_t>(uploadBytes_)) {
        throw std::runtime_error(label_ + " worker RAW payload size mismatch");
    }
    std::memcpy(uploadMapped_, rawCopy.data(), static_cast<size_t>(uploadBytes_));
    waitSpare();
    vkCheck(vkResetFences(device_, 1, &fence_), label_ + " reset fence");
    vkCheck(vkResetCommandBuffer(command_, 0), label_ + " reset command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &begin), label_ + " begin command buffer");

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = input_.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width_, height_, 1};
    vkCmdCopyBufferToImage(command_, uploadBuffer_, input_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);

    // GALOSH-RAW tap (P1a): denoise input_ in place before highlight
    // packing. process() submits + fence-waits on its own command buffers,
    // so the recorded upload above must EXECUTE first: end/submit/wait the
    // prefix and begin a fresh CB. Without the flush the tap would denoise
    // stale input (and the upload would clobber its output). Off (or no
    // f16 device support) = the legacy path below runs bit-identically.
    //
    // NOTE (Adreno watchdog): Full died on first submit (device lost ~5 s
    // in) while chroma-only + YUV survived: pass12Banded fired 146x108
    // heavy workgroups in ONE submit, past the driver watchdog. Fixed by
    // 2D micro-banding in GaloshRawPipeline.cpp (bit-identical, bounded
    // per-submit work). This quarantine is LIFTED for verification; if
    // Full dies again on device, reinstate Full->ChromaOnly here.
    ::galosh::GaloshRawParams galoshEff = galosh;
    const bool galoshRawRequested = galoshEff.mode != ::galosh::GaloshRawMode::Off;
    if (galoshRawRequested && !float16Compute_) {
        emit("GALOSH_RAW_SKIP reason=no_f16");
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "GALOSH_RAW_SKIP %s reason=no_f16", label_.c_str());
    } else if (galoshRawRequested && (width_ < 64 || height_ < 64)) {
        emit("GALOSH_RAW_SKIP reason=too_small");
        __android_log_print(ANDROID_LOG_INFO, "RawrCamNative", "GALOSH_RAW_SKIP %s reason=too_small", label_.c_str());
    } else if (galoshRawRequested) {
        // Denoise failures degrade to the legacy path (never fail the shot
        // for denoise), matching the YUV tap policy. ensureGalosh() (pipeline
        // build) runs before the prefix submit so a throw there leaves the
        // recorded upload intact; process() runs after the prefix flush on a
        // fresh CB so a throw there leaves an empty outer CB — both safe to
        // continue.
        try {
            ensureGalosh();
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "GALOSH_RAW_FALLBACK %s reason=%s", label_.c_str(),
                                e.what());
            emit(std::string("GALOSH_RAW_FALLBACK reason=") + e.what());
            galoshEff.mode = ::galosh::GaloshRawMode::Off;
        }
        if (galoshEff.mode != ::galosh::GaloshRawMode::Off) {
            __android_log_print(ANDROID_LOG_INFO, "RawrCamNative",
                                "GALOSH_RAW_TAP %s mode=%d wh=%ux%u strength=%.2f luma=%.2f chroma=%.2f",
                                label_.c_str(), static_cast<int>(galoshEff.mode), width_, height_, galoshEff.strength,
                                galoshEff.lumaStrength, galoshEff.chromaStrength);
            flush();
            try {
                galosh_->process(queue_, *queueSubmitMutex_, input_.view, input_.view, width_, height_, galoshEff,
                                 VK_NULL_HANDLE);
                ::galosh::vk::fullBarrier(command_);
            } catch (const std::exception& e) {
                __android_log_print(ANDROID_LOG_WARN, "RawrCamNative", "GALOSH_RAW_FALLBACK %s reason=%s",
                                    label_.c_str(), e.what());
                emit(std::string("GALOSH_RAW_FALLBACK reason=") + e.what());
                // In-place tap may have partially overwritten input_; the
                // legacy highlight path below still runs (artifact risk
                // preferred over a failed shot). Barrier for visibility.
                ::galosh::vk::fullBarrier(command_);
            }
        }
    }

    if (sharedHighlightPacked_) {
        VkImageMemoryBarrier pw{};
        pw.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        pw.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pw.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        pw.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pw.image = packed_.image;
        pw.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        pw.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        VkImageMemoryBarrier cw = pw;
        cw.image = clipState_.image;
        VkImageMemoryBarrier prep[2]{pw, cw};
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, prep);
        recordSharedHighlight(command_, blackPhysical, whiteLevel, whiteBalanceRggb, lensShading);
        pw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        pw.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        cw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        cw.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkImageMemoryBarrier ready[2]{pw, cw};
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, ready);
    }
    VkImageMemoryBarrier out{};
    out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    out.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    out.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    out.image = output_.image;
    out.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    out.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &out);
    return command_;
}

void SharedHighlightRuntime::flush() {
    vkCheck(vkEndCommandBuffer(command_), label_ + " flush end");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command_;
    {
        std::lock_guard<std::mutex> lock(*queueSubmitMutex_);
        vkCheck(vkQueueSubmit(queue_, 1, &si, fence_), label_ + " flush submit");
    }
    vkCheck(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), label_ + " flush wait");
    vkCheck(vkResetFences(device_, 1, &fence_), label_ + " flush reset fence");
    vkCheck(vkResetCommandBuffer(command_, 0), label_ + " flush reset command");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &begin), label_ + " flush begin");
    VkMemoryBarrier previous{};
    previous.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    previous.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    previous.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &previous, 0,
                         nullptr, 0, nullptr);
}

VkCommandBuffer SharedHighlightRuntime::flushPipelined() {
    vkCheck(vkEndCommandBuffer(command_), label_ + " pipelined flush end");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command_;
    {
        std::lock_guard<std::mutex> lock(*queueSubmitMutex_);
        vkCheck(vkQueueSubmit(queue_, 1, &si, fence_), label_ + " pipelined flush submit");
    }
    // The spare holds the submission before this one; once it retires, its
    // buffer and fence become the recording pair.
    if (sparePending_) {
        vkCheck(vkWaitForFences(device_, 1, &spareFence_, VK_TRUE, UINT64_MAX), label_ + " pipelined flush wait");
        vkCheck(vkResetFences(device_, 1, &spareFence_), label_ + " pipelined flush reset fence");
    }
    std::swap(command_, spareCommand_);
    std::swap(fence_, spareFence_);
    sparePending_ = true;
    vkCheck(vkResetCommandBuffer(command_, 0), label_ + " pipelined flush reset command");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command_, &begin), label_ + " pipelined flush begin");
    // Same-queue barrier: orders this buffer after every earlier submission.
    VkMemoryBarrier previous{};
    previous.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    previous.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    previous.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &previous, 0,
                         nullptr, 0, nullptr);
    return command_;
}

void SharedHighlightRuntime::waitSpare() noexcept {
    if (!sparePending_) return;
    vkWaitForFences(device_, 1, &spareFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &spareFence_);
    sparePending_ = false;
}

void SharedHighlightRuntime::submit(VkCommandBuffer command) {
    vkCheck(vkEndCommandBuffer(command), label_ + " end command buffer");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command;
    {
        std::lock_guard<std::mutex> lock(*queueSubmitMutex_);
        vkCheck(vkQueueSubmit(queue_, 1, &si, fence_), label_ + " queue submit");
    }
}

void SharedHighlightRuntime::waitForCompletion() {
    vkCheck(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), label_ + " wait fence");
    waitSpare();
}

void SharedHighlightRuntime::discardWorkspace() noexcept { destroyWorkspaceResources(false); }

void SharedHighlightRuntime::createSharedHighlightPipeline() {
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].descriptorType = (i == 3) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    di.bindingCount = 4;
    di.pBindings = bindings;
    vkCheck(vkCreateDescriptorSetLayout(device_, &di, nullptr, &highlightDsl_),
            label_ + " create shared-highlight descriptor layout");
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = 80;
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &highlightDsl_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    vkCheck(vkCreatePipelineLayout(device_, &li, nullptr, &highlightLayout_),
            label_ + " create shared-highlight pipeline layout");
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = sensor_clip_pack_spv_size;
    sm.pCode = reinterpret_cast<const uint32_t*>(sensor_clip_pack_spv);
    VkShaderModule shader = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device_, &sm, nullptr, &shader), label_ + " create shared-highlight shader module");
    try {
        VkPipelineShaderStageCreateInfo st{};
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = shader;
        st.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = st;
        ci.layout = highlightLayout_;
        vkCheck(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &highlightPipeline_),
                label_ + " create shared-highlight pipeline");
    } catch (...) {
        vkDestroyShaderModule(device_, shader, nullptr);
        throw;
    }
    vkDestroyShaderModule(device_, shader, nullptr);
    VkDescriptorPoolSize ps[2]{{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    vkCheck(vkCreateDescriptorPool(device_, &pi, nullptr, &highlightPool_),
            label_ + " create shared-highlight descriptor pool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = highlightPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &highlightDsl_;
    vkCheck(vkAllocateDescriptorSets(device_, &ai, &highlightSet_),
            label_ + " allocate shared-highlight descriptor set");
    VkDescriptorImageInfo images[3]{{VK_NULL_HANDLE, input_.view, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, packed_.view, VK_IMAGE_LAYOUT_GENERAL},
                                    {VK_NULL_HANDLE, clipState_.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = highlightSet_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
}

void SharedHighlightRuntime::createReplayHighlightPipeline() {
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    di.bindingCount = 3;
    di.pBindings = b;
    vkCheck(vkCreateDescriptorSetLayout(device_, &di, nullptr, &replayDsl_),
            label_ + " create replay-highlight descriptor layout");
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = 44;
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &replayDsl_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    vkCheck(vkCreatePipelineLayout(device_, &li, nullptr, &replayLayout_),
            label_ + " create replay-highlight pipeline layout");
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = replay_tagged_decode_spv_size;
    sm.pCode = reinterpret_cast<const uint32_t*>(replay_tagged_decode_spv);
    VkShaderModule shader = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device_, &sm, nullptr, &shader), label_ + " create replay-highlight shader module");
    try {
        VkPipelineShaderStageCreateInfo st{};
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = shader;
        st.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = st;
        ci.layout = replayLayout_;
        vkCheck(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &replayPipeline_),
                label_ + " create replay-highlight pipeline");
    } catch (...) {
        vkDestroyShaderModule(device_, shader, nullptr);
        throw;
    }
    vkDestroyShaderModule(device_, shader, nullptr);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    vkCheck(vkCreateDescriptorPool(device_, &pi, nullptr, &replayPool_),
            label_ + " create replay-highlight descriptor pool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = replayPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &replayDsl_;
    vkCheck(vkAllocateDescriptorSets(device_, &ai, &replaySet_), label_ + " allocate replay-highlight descriptor set");
    VkDescriptorImageInfo ii{};
    ii.imageView = replayInput_.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo oo{};
    oo.imageView = packed_.view;
    oo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo co{};
    co.imageView = clipState_.view;
    co.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w[3]{};
    for (auto& x : w) {
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.dstSet = replaySet_;
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    w[0].pImageInfo = &ii;
    w[1].dstBinding = 1;
    w[1].pImageInfo = &oo;
    w[2].dstBinding = 2;
    w[2].pImageInfo = &co;
    vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
}

void SharedHighlightRuntime::destroyReplayHighlightPipeline() noexcept {
    if (!device_) return;
    if (replayPool_) vkDestroyDescriptorPool(device_, replayPool_, nullptr);
    if (replayPipeline_) vkDestroyPipeline(device_, replayPipeline_, nullptr);
    if (replayLayout_) vkDestroyPipelineLayout(device_, replayLayout_, nullptr);
    if (replayDsl_) vkDestroyDescriptorSetLayout(device_, replayDsl_, nullptr);
    replaySet_ = VK_NULL_HANDLE;
    replayPool_ = VK_NULL_HANDLE;
    replayPipeline_ = VK_NULL_HANDLE;
    replayLayout_ = VK_NULL_HANDLE;
    replayDsl_ = VK_NULL_HANDLE;
}

void SharedHighlightRuntime::ensureLensShadingBuffer(VkDeviceSize requiredBytes) {
    if (lscBuffer_ != VK_NULL_HANDLE && lscBytes_ >= requiredBytes) return;

    destroyLensShadingBuffer();

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = requiredBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device_, &bufferInfo, nullptr, &lscBuffer_), label_ + " create LSC buffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, lscBuffer_, &requirements);

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findHostMemoryType(physicalDevice_, requirements.memoryTypeBits, label_ + " LSC");
    vkCheck(vkAllocateMemory(device_, &allocation, nullptr, &lscMemory_), label_ + " allocate LSC memory");
    vkCheck(vkBindBufferMemory(device_, lscBuffer_, lscMemory_, 0), label_ + " bind LSC memory");
    vkCheck(vkMapMemory(device_, lscMemory_, 0, VK_WHOLE_SIZE, 0, &lscMapped_), label_ + " map LSC memory");
    lscBytes_ = requiredBytes;

    VkDescriptorBufferInfo descriptorBuffer{};
    descriptorBuffer.buffer = lscBuffer_;
    descriptorBuffer.offset = 0;
    descriptorBuffer.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = highlightSet_;
    write.dstBinding = 3;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &descriptorBuffer;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void SharedHighlightRuntime::uploadLensShadingMap(
    const rawrcam::develop::highlight::LensShadingMapSnapshot& lensShading) {
    constexpr float kIdentityGains[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const size_t floatCount = lensShading.valid() ? lensShading.gains.size() : 4u;
    const VkDeviceSize requiredBytes = static_cast<VkDeviceSize>(floatCount * sizeof(float));
    ensureLensShadingBuffer(requiredBytes);

    if (lensShading.valid()) {
        std::memcpy(lscMapped_, lensShading.gains.data(), static_cast<size_t>(requiredBytes));
    } else {
        std::memcpy(lscMapped_, kIdentityGains, sizeof(kIdentityGains));
    }
}

void SharedHighlightRuntime::recordSharedHighlight(
    VkCommandBuffer command, const std::array<float, 4>& blackPhysical, float whiteLevel,
    const std::array<float, 4>& whiteBalanceRggb,
    const rawrcam::develop::highlight::LensShadingMapSnapshot& lensShading) {
    uploadLensShadingMap(lensShading);

    struct PushConstants {
        float black[4];
        float invRange[4];
        float wb[4];
        float clipThreshold;
        uint32_t width;
        uint32_t height;
        uint32_t pattern;
        uint32_t lscEnabled;
        uint32_t lscWidth;
        uint32_t lscHeight;
    } push{};
    static_assert(sizeof(PushConstants) == 76);

    for (size_t i = 0; i < 4; ++i) {
        if (!(whiteLevel > blackPhysical[i])) {
            throw std::runtime_error("shared highlight requires whiteLevel > black level");
        }
        push.black[i] = blackPhysical[i];
        push.invRange[i] = 1.0f / (whiteLevel - blackPhysical[i]);
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!(whiteBalanceRggb[i] > 0.0f)) {
            throw std::runtime_error("shared highlight requires positive white balance");
        }
        push.wb[i] = whiteBalanceRggb[i];
    }

    push.clipThreshold = raw_preview::RawFrameParameters{}.clipThreshold;
    push.width = width_;
    push.height = height_;
    push.pattern = rawPreviewCfa_;
    push.lscEnabled = lensShading.valid() ? 1u : 0u;
    push.lscWidth = lensShading.valid() ? lensShading.width : 0u;
    push.lscHeight = lensShading.valid() ? lensShading.height : 0u;

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, highlightPipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, highlightLayout_, 0, 1, &highlightSet_, 0,
                            nullptr);
    vkCmdPushConstants(command, highlightLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &push);
    vkCmdDispatch(command, ((width_ / 2u) + 7u) / 8u, ((height_ / 2u) + 7u) / 8u, 1);
}

void SharedHighlightRuntime::destroyLensShadingBuffer() noexcept {
    if (!device_) return;
    if (lscMapped_ && lscMemory_) vkUnmapMemory(device_, lscMemory_);
    if (lscBuffer_) vkDestroyBuffer(device_, lscBuffer_, nullptr);
    if (lscMemory_) vkFreeMemory(device_, lscMemory_, nullptr);
    lscMapped_ = nullptr;
    lscBuffer_ = VK_NULL_HANDLE;
    lscMemory_ = VK_NULL_HANDLE;
    lscBytes_ = 0;
}

void SharedHighlightRuntime::destroySharedHighlightPipeline() noexcept {
    if (!device_) return;
    destroyLensShadingBuffer();
    if (highlightPool_) vkDestroyDescriptorPool(device_, highlightPool_, nullptr);
    if (highlightPipeline_) vkDestroyPipeline(device_, highlightPipeline_, nullptr);
    if (highlightLayout_) vkDestroyPipelineLayout(device_, highlightLayout_, nullptr);
    if (highlightDsl_) vkDestroyDescriptorSetLayout(device_, highlightDsl_, nullptr);
    highlightSet_ = VK_NULL_HANDLE;
    highlightPool_ = VK_NULL_HANDLE;
    highlightPipeline_ = VK_NULL_HANDLE;
    highlightLayout_ = VK_NULL_HANDLE;
    highlightDsl_ = VK_NULL_HANDLE;
}

void SharedHighlightRuntime::destroyWorkspaceResources(bool keepOutput) noexcept {
    destroyReplayHighlightPipeline();
    destroySharedHighlightPipeline();
    if (device_) {
        if (uploadMapped_ && uploadMemory_) vkUnmapMemory(device_, uploadMemory_);
        uploadMapped_ = nullptr;
        waitSpare();
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (spareFence_) vkDestroyFence(device_, spareFence_, nullptr);
        if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (uploadBuffer_) vkDestroyBuffer(device_, uploadBuffer_, nullptr);
        if (uploadMemory_) vkFreeMemory(device_, uploadMemory_, nullptr);
        rawrcam::vulkan::destroyOwnedImage(device_, input_);
        rawrcam::vulkan::destroyOwnedImage(device_, packed_);
        rawrcam::vulkan::destroyOwnedImage(device_, replayInput_);
        if (!keepOutput) {
            rawrcam::vulkan::destroyOwnedImage(device_, output_);
            rawrcam::vulkan::destroyOwnedImage(device_, clipState_);
        }
    }
    fence_ = VK_NULL_HANDLE;
    command_ = VK_NULL_HANDLE;
    spareFence_ = VK_NULL_HANDLE;
    spareCommand_ = VK_NULL_HANDLE;
    sparePending_ = false;
    commandPool_ = VK_NULL_HANDLE;
    uploadBuffer_ = VK_NULL_HANDLE;
    uploadMemory_ = VK_NULL_HANDLE;
    uploadBytes_ = 0;
}
void SharedHighlightRuntime::releaseWorkspaceKeepOutput() noexcept { destroyWorkspaceResources(true); }

void SharedHighlightRuntime::releaseOutput() noexcept {
    if (device_) {
        rawrcam::vulkan::destroyOwnedImage(device_, output_);
        rawrcam::vulkan::destroyOwnedImage(device_, clipState_);
    }
}

void SharedHighlightRuntime::reset() noexcept {
    // Destroy GALOSH while device_ is still valid (pipeline dtor issues
    // vkDestroy* on its stored VkDevice).
    galosh_.reset();
    galoshDevice_ = VK_NULL_HANDLE;
    destroyWorkspaceResources(false);
    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
    queueSubmitMutex_ = nullptr;
    width_ = 0;
    height_ = 0;
    rawPreviewCfa_ = 0;
    sharedHighlightPacked_ = false;
    externalInput_ = false;
}

bool SharedHighlightRuntime::dumpExternalImage(const std::string& path, VkImage image, uint32_t width, uint32_t height,
                                               uint32_t bytesPerPixel, VkAccessFlags producerAccess) {
    if (!configured() || !image || !width || !height || !bytesPerPixel) return false;
    const uint64_t n = static_cast<uint64_t>(width) * height * bytesPerPixel;
    if (!n || n > std::numeric_limits<VkDeviceSize>::max()) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(n);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence f = VK_NULL_HANDLE;
    try {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(device_, &bi, nullptr, &buffer), label_ + " diagnostic create buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device_, buffer, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findHostMemoryType(physicalDevice_, req.memoryTypeBits, label_);
        vkCheck(vkAllocateMemory(device_, &ai, nullptr, &memory), label_ + " diagnostic allocate memory");
        vkCheck(vkBindBufferMemory(device_, buffer, memory, 0), label_ + " diagnostic bind memory");
        vkCheck(vkMapMemory(device_, memory, 0, bytes, 0, &mapped), label_ + " diagnostic map memory");
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = queueFamily_;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCheck(vkCreateCommandPool(device_, &pi, nullptr, &pool), label_ + " diagnostic create pool");
        VkCommandBufferAllocateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ci.commandPool = pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(device_, &ci, &cmd), label_ + " diagnostic allocate command");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCheck(vkCreateFence(device_, &fi, nullptr, &f), label_ + " diagnostic create fence");
        VkCommandBufferBeginInfo beg{};
        beg.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beg.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(cmd, &beg), label_ + " diagnostic begin");
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = producerAccess;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &cp);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        vkCheck(vkEndCommandBuffer(cmd), label_ + " diagnostic end");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        {
            std::lock_guard<std::mutex> q(*queueSubmitMutex_);
            vkCheck(vkQueueSubmit(queue_, 1, &si, f), label_ + " diagnostic submit");
        }
        vkCheck(vkWaitForFences(device_, 1, &f, VK_TRUE, UINT64_MAX), label_ + " diagnostic wait");
        const std::string tmp = path + ".tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("diagnostic open failed");
        out.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(bytes));
        out.close();
        if (!out) throw std::runtime_error("diagnostic write failed");
        if (::rename(tmp.c_str(), path.c_str()) != 0) {
            ::unlink(tmp.c_str());
            throw std::runtime_error("diagnostic rename failed");
        }
        if (mapped) vkUnmapMemory(device_, memory);
        if (f) vkDestroyFence(device_, f, nullptr);
        if (pool) vkDestroyCommandPool(device_, pool, nullptr);
        if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
        if (memory) vkFreeMemory(device_, memory, nullptr);
        return true;
    } catch (...) {
        if (mapped && memory) vkUnmapMemory(device_, memory);
        if (f) vkDestroyFence(device_, f, nullptr);
        if (pool) vkDestroyCommandPool(device_, pool, nullptr);
        if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
        if (memory) vkFreeMemory(device_, memory, nullptr);
        return false;
    }
}

bool SharedHighlightRuntime::dumpOutputRgba16f(const std::string& path) {
    return dumpExternalImage(path, output_.image, width_, height_, 8u);
}

bool SharedHighlightRuntime::dumpPackedCfaRgba16f(const std::string& path) {
    if (!sharedHighlightPacked_ || !packed_.image) return false;
    return dumpExternalImage(path, packed_.image, width_ / 2u, height_ / 2u, 8u);
}

void SharedHighlightRuntime::emit(const std::string& line) const {
    if (diagnostic_) {
        diagnostic_(line);
    }
}

}  // namespace rawrcam::develop
