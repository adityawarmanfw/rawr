#include "Synchronization.h"
#ifndef VK_QUEUE_FAMILY_FOREIGN_EXT
#define VK_QUEUE_FAMILY_FOREIGN_EXT (~2U)
#endif

namespace rawrcam::vulkan {
namespace {
VkImageMemoryBarrier barrier(VkImage image) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
}  // namespace

void acquireForeignImage(VkCommandBuffer c, VkImage image, uint32_t q, VkPipelineStageFlags dstStage,
                         VkAccessFlags dstAccess) {
    auto b = barrier(image);
    b.srcAccessMask = 0;
    b.dstAccessMask = dstAccess;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b.dstQueueFamilyIndex = q;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}
void releaseForeignImage(VkCommandBuffer c, VkImage image, uint32_t q, VkPipelineStageFlags srcStage,
                         VkAccessFlags srcAccess) {
    auto b = barrier(image);
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = q;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier(c, srcStage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
void acquireForeignBuffer(VkCommandBuffer c, VkBuffer buffer, uint32_t q, VkPipelineStageFlags dstStage,
                          VkAccessFlags dstAccess) {
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b.dstQueueFamilyIndex = q;
    b.buffer = buffer;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dstStage, 0, 0, nullptr, 1, &b, 0, nullptr);
}
void releaseForeignBuffer(VkCommandBuffer c, VkBuffer buffer, uint32_t q, VkPipelineStageFlags srcStage,
                          VkAccessFlags srcAccess) {
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = 0;
    b.srcQueueFamilyIndex = q;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b.buffer = buffer;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(c, srcStage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
}
void computeWriteToComputeRead(VkCommandBuffer c, VkImage image) {
    auto b = barrier(image);
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}
void computeWriteToComputeWrite(VkCommandBuffer c, VkImage image) {
    auto b = barrier(image);
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}
void computeWriteToFragmentRead(VkCommandBuffer c, VkImage image) {
    auto b = barrier(image);
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}
void fragmentReadToComputeWrite(VkCommandBuffer c, VkImage image) {
    auto b = barrier(image);
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
}
void transferWriteToComputeRead(VkCommandBuffer c, VkImage image) {
    auto b = barrier(image);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
}  // namespace rawrcam::vulkan
