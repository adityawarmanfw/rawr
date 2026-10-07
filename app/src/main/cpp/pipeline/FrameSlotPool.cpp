#include "pipeline/FrameSlotPool.h"

#include <stdexcept>
#include <string>

namespace rawrcam::pipeline {
namespace {
void vkCheck(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(result));
}
}  // namespace

void FrameSlotPool::initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily) {
    destroy();
    physical_ = physical;
    device_ = device;
    commands_.create(device_, queueFamily, imaging::kRealtimeFramesInFlight * 4);

    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) {
        auto& slot = slots_[i];
        slot.command = commands_.command(i);
        slot.videoCommand = commands_.command(i + rawrcam::imaging::kRealtimeFramesInFlight);
        slot.videoStageCommands = {commands_.command(i + 2 * rawrcam::imaging::kRealtimeFramesInFlight),
                                   commands_.command(i + 3 * rawrcam::imaging::kRealtimeFramesInFlight)};
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCheck(vkCreateFence(device_, &fenceInfo, nullptr, &slot.fence), "vkCreateFence");
        vkCheck(vkCreateFence(device_, &fenceInfo, nullptr, &slot.previewFence), "vkCreateFence preview");

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.imageAvailable), "vkCreateSemaphore");
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.renderFinished), "vkCreateSemaphore");

        VkExportSemaphoreCreateInfo exportInfo{};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        semaphoreInfo.pNext = &exportInfo;
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.cameraAcquire), "vkCreateSemaphore camera");
    }
}

void FrameSlotPool::createImages(uint32_t rawWidth, uint32_t rawHeight, uint32_t previewWidth, uint32_t previewHeight,
                                 bool retainBridgeCopy) {
    destroyImages();
    for (auto& slot : slots_) {
        if (retainBridgeCopy) slot.rawCopy = createBridgeImage(rawWidth, rawHeight);
        slot.linear = rawrcam::vulkan::createOwnedImage(
            physical_, device_, previewWidth, previewHeight, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        slot.tonemapped = rawrcam::vulkan::createOwnedImage(
            physical_, device_, previewWidth, previewHeight, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        // Half-preview look pair (film, or tonemap below Full viewfinder
        // resolution). TRANSFER bits serve the downscale blit
        // (linear in) and the presentation sampling path (out).
        const uint32_t quarterWidth = std::max(1u, previewWidth / 2u);
        const uint32_t quarterHeight = std::max(1u, previewHeight / 2u);
        slot.lookScaledLinear = rawrcam::vulkan::createOwnedImage(
            physical_, device_, quarterWidth, quarterHeight, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        slot.lookScaledOut = rawrcam::vulkan::createOwnedImage(
            physical_, device_, quarterWidth, quarterHeight, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
}

rawrcam::vulkan::OwnedImage FrameSlotPool::createBridgeImage(uint32_t rawWidth, uint32_t rawHeight) const {
    return rawrcam::vulkan::createOwnedImage(
        physical_, device_, rawWidth, rawHeight, VK_FORMAT_R16_UINT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

bool FrameSlotPool::ensureBridgeImages(uint32_t rawWidth, uint32_t rawHeight, VkQueue queue) {
    std::array<VkImage, rawrcam::imaging::kRealtimeFramesInFlight> created{};
    size_t createdCount = 0;
    for (auto& slot : slots_) {
        if (slot.rawCopy.image != VK_NULL_HANDLE) continue;
        slot.rawCopy = createBridgeImage(rawWidth, rawHeight);
        created[createdCount++] = slot.rawCopy.image;
    }
    if (createdCount == 0) return false;
    // Only the new images: the rest of each slot keeps its contents/layout.
    transitionToGeneral(queue, created.data(), createdCount);
    return true;
}

void FrameSlotPool::transitionImagesToGeneral(VkQueue queue) {
    std::array<VkImage, rawrcam::imaging::kRealtimeFramesInFlight * 5> images{};
    size_t imageCount = 0;
    for (const auto& slot : slots_) {
        for (VkImage image : {slot.rawCopy.image, slot.linear.image, slot.tonemapped.image,
                              slot.lookScaledLinear.image, slot.lookScaledOut.image}) {
            if (image == VK_NULL_HANDLE) continue;  // bridge copy image may be intentionally unallocated
            images[imageCount++] = image;
        }
    }
    transitionToGeneral(queue, images.data(), imageCount);
}

void FrameSlotPool::transitionToGeneral(VkQueue queue, const VkImage* images, size_t imageCount) {
    if (device_ == VK_NULL_HANDLE || commands_.pool() == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        throw std::runtime_error("FrameSlotPool is not initialized");
    }

    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commands_.pool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(device_, &allocInfo, &command), "allocate image-layout command");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command, &beginInfo), "begin image-layout command");

    std::array<VkImageMemoryBarrier, rawrcam::imaging::kRealtimeFramesInFlight * 5> barriers{};
    size_t barrierCount = 0;
    for (size_t i = 0; i < imageCount && i < barriers.size(); ++i) {
        auto& barrier = barriers[barrierCount++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[i];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }

    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, static_cast<uint32_t>(barrierCount), barriers.data());
    vkCheck(vkEndCommandBuffer(command), "end image-layout command");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command;
    vkCheck(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE), "submit image-layout command");
    vkCheck(vkQueueWaitIdle(queue), "wait image-layout command");
    vkFreeCommandBuffers(device_, commands_.pool(), 1, &command);
}

void FrameSlotPool::destroyImages() {
    if (device_ == VK_NULL_HANDLE) return;
    for (auto& slot : slots_) {
        rawrcam::vulkan::destroyOwnedImage(device_, slot.rawCopy);
        rawrcam::vulkan::destroyOwnedImage(device_, slot.linear);
        rawrcam::vulkan::destroyOwnedImage(device_, slot.tonemapped);
        rawrcam::vulkan::destroyOwnedImage(device_, slot.lookScaledLinear);
        rawrcam::vulkan::destroyOwnedImage(device_, slot.lookScaledOut);
    }
}

FrameSlot* FrameSlotPool::findAvailable(uint32_t* slotIndex) {
    for (uint32_t i = 0; i < rawrcam::imaging::kRealtimeFramesInFlight; ++i) {
        if (!slots_[i].submitted) {
            if (slotIndex) *slotIndex = i;
            return &slots_[i];
        }
    }
    return nullptr;
}

void FrameSlotPool::recreateSyncObjects(uint32_t slotIndex) {
    if (device_ == VK_NULL_HANDLE || slotIndex >= rawrcam::imaging::kRealtimeFramesInFlight) {
        throw std::runtime_error("FrameSlotPool sync recovery is unavailable");
    }

    auto& slot = slots_[slotIndex];
    if (slot.submitted) {
        throw std::runtime_error("cannot recover sync objects for submitted slot");
    }
    if (slot.previewSubmitted) {
        vkCheck(vkWaitForFences(device_, 1, &slot.previewFence, VK_TRUE, UINT64_MAX),
                "wait preview before sync recovery");
        slot.previewSubmitted = false;
    }

    VkFence replacementFence = VK_NULL_HANDLE;
    VkSemaphore replacementImageAvailable = VK_NULL_HANDLE;
    VkSemaphore replacementRenderFinished = VK_NULL_HANDLE;
    VkSemaphore replacementCameraAcquire = VK_NULL_HANDLE;

    try {
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCheck(vkCreateFence(device_, &fenceInfo, nullptr, &replacementFence), "vkCreateFence recovery");

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &replacementImageAvailable),
                "vkCreateSemaphore imageAvailable recovery");
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &replacementRenderFinished),
                "vkCreateSemaphore renderFinished recovery");

        VkExportSemaphoreCreateInfo exportInfo{};
        exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        semaphoreInfo.pNext = &exportInfo;
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &replacementCameraAcquire),
                "vkCreateSemaphore cameraAcquire recovery");
    } catch (...) {
        if (replacementCameraAcquire) {
            vkDestroySemaphore(device_, replacementCameraAcquire, nullptr);
        }
        if (replacementRenderFinished) {
            vkDestroySemaphore(device_, replacementRenderFinished, nullptr);
        }
        if (replacementImageAvailable) {
            vkDestroySemaphore(device_, replacementImageAvailable, nullptr);
        }
        if (replacementFence) {
            vkDestroyFence(device_, replacementFence, nullptr);
        }
        throw;
    }

    if (slot.cameraAcquire) {
        vkDestroySemaphore(device_, slot.cameraAcquire, nullptr);
    }
    if (slot.renderFinished) {
        vkDestroySemaphore(device_, slot.renderFinished, nullptr);
    }
    if (slot.imageAvailable) {
        vkDestroySemaphore(device_, slot.imageAvailable, nullptr);
    }
    if (slot.fence) {
        vkDestroyFence(device_, slot.fence, nullptr);
    }

    slot.fence = replacementFence;
    slot.imageAvailable = replacementImageAvailable;
    slot.renderFinished = replacementRenderFinished;
    slot.cameraAcquire = replacementCameraAcquire;
}

void FrameSlotPool::recreatePreviewSyncObjects(uint32_t slotIndex) {
    if (!device_ || slotIndex >= rawrcam::imaging::kRealtimeFramesInFlight)
        throw std::runtime_error("preview sync recovery is unavailable");
    auto& slot = slots_[slotIndex];
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    try {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable),
                "preview recovery imageAvailable");
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished),
                "preview recovery renderFinished");
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCheck(vkCreateFence(device_, &fenceInfo, nullptr, &fence), "preview recovery fence");
    } catch (...) {
        if (imageAvailable) vkDestroySemaphore(device_, imageAvailable, nullptr);
        if (renderFinished) vkDestroySemaphore(device_, renderFinished, nullptr);
        if (fence) vkDestroyFence(device_, fence, nullptr);
        throw;
    }
    if (slot.imageAvailable) vkDestroySemaphore(device_, slot.imageAvailable, nullptr);
    if (slot.renderFinished) vkDestroySemaphore(device_, slot.renderFinished, nullptr);
    if (slot.previewFence) vkDestroyFence(device_, slot.previewFence, nullptr);
    slot.imageAvailable = imageAvailable;
    slot.renderFinished = renderFinished;
    slot.previewFence = fence;
    slot.previewSubmitted = false;
}

void FrameSlotPool::destroy() {
    destroyImages();
    if (device_ != VK_NULL_HANDLE) {
        for (auto& slot : slots_) {
            if (slot.imageLease) {
                AImage_delete(slot.imageLease);
                slot.imageLease = nullptr;
            }
            if (slot.cameraAcquire) vkDestroySemaphore(device_, slot.cameraAcquire, nullptr);
            if (slot.renderFinished) vkDestroySemaphore(device_, slot.renderFinished, nullptr);
            if (slot.imageAvailable) vkDestroySemaphore(device_, slot.imageAvailable, nullptr);
            if (slot.fence) vkDestroyFence(device_, slot.fence, nullptr);
            if (slot.previewFence) vkDestroyFence(device_, slot.previewFence, nullptr);
            slot = FrameSlot{};
        }
    }
    commands_.reset();
    physical_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

}  // namespace rawrcam::pipeline
